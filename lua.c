/*
 * uhttpd - Tiny single-threaded httpd
 *
 *   Copyright (C) 2010-2013 Jo-Philipp Wich <xm@subsignal.org>
 *   Copyright (C) 2013 Felix Fietkau <nbd@openwrt.org>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define _GNU_SOURCE
#include <libubox/blobmsg.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <stdio.h>
#include <poll.h>
#include <string.h>
#include <ctype.h>

#include "uhttpd.h"
#include "plugin.h"

#define UH_LUA_CB	"handle_request"

static const struct uhttpd_ops *ops;
static struct config *_conf;
#define conf (*_conf)

static lua_State *_L;

static int uh_lua_recv(lua_State *L)
{
	static struct pollfd pfd = {
		.fd = STDIN_FILENO,
		.events = POLLIN,
	};
	luaL_Buffer B;
	int data_len = 0;
	int len;
	int r;

	len = luaL_optnumber(L, 1, LUAL_BUFFERSIZE);
	luaL_buffinit(L, &B);
	while(len > 0) {
		char *buf;

		buf = luaL_prepbuffer(&B);
		r = read(STDIN_FILENO, buf,
		         len < LUAL_BUFFERSIZE ? len : LUAL_BUFFERSIZE);
		if (r < 0) {
			if (errno == EWOULDBLOCK || errno == EAGAIN) {
				pfd.revents = 0;
				poll(&pfd, 1, 1000);
				if (pfd.revents & POLLIN)
					continue;
			}
			if (errno == EINTR)
				continue;

			if (!data_len)
				data_len = -1;
			break;
		}
		if (!r)
			break;

		luaL_addsize(&B, r);
		data_len += r;
		len -= r;
		if (r != LUAL_BUFFERSIZE)
			break;
	}

	luaL_pushresult(&B);
	lua_pushnumber(L, data_len);
	if (data_len > 0) {
		lua_pushvalue(L, -2);
		lua_remove(L, -3);
		return 2;
	} else {
		lua_remove(L, -2);
		return 1;
	}
}

static int uh_lua_send(lua_State *L)
{
	const char *buf;
	size_t len;

	buf = luaL_checklstring(L, 1, &len);
	if (len > 0)
		len = write(STDOUT_FILENO, buf, len);

	lua_pushnumber(L, len);
	return 1;
}

static int uh_lua_recv_to_file(lua_State *L)
{
	static struct pollfd pfd = {
		.fd = STDIN_FILENO,
		.events = POLLIN,
	};
	static char buf[4096];
	ssize_t total = 0, max_size = -1, remaining;
	int fd, r, read_size;
	FILE *file;

	/* First parameter should be a Lua file handle from io.open() */
	if (!lua_isuserdata(L, 1)) {
		lua_pushnil(L);
		lua_pushstring(L, "Expected file handle from io.open()");
		return 2;
	}

	/* Get the FILE* from the Lua file handle */
	file = *(FILE**)luaL_checkudata(L, 1, LUA_FILEHANDLE);
	if (!file) {
		lua_pushnil(L);
		lua_pushstring(L, "File handle is closed or invalid");
		return 2;
	}

	/* Get the underlying file descriptor */
	fd = fileno(file);
	if (fd < 0) {
		lua_pushnil(L);
		lua_pushstring(L, "Cannot get file descriptor from file handle");
		return 2;
	}

	/* Optional second parameter: maximum size to read from request */
	if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
		max_size = luaL_checkinteger(L, 2);
		if (max_size < 0) {
			lua_pushnil(L);
			lua_pushstring(L, "Invalid maximum size");
			return 2;
		}
	}

	remaining = max_size;
	while (1) {
		/* Determine how much to read this iteration */
		if (max_size >= 0) {
			if (remaining <= 0)
				break;
			read_size = (remaining < (ssize_t)sizeof(buf)) ? remaining : (int)sizeof(buf);
		} else {
			read_size = sizeof(buf);
		}

		r = read(STDIN_FILENO, buf, read_size);
		if (r < 0) {
			if (errno == EWOULDBLOCK || errno == EAGAIN) {
				if(max_size >= 0)
					/* No data available right now - return what we have so far */
					break;

				pfd.revents = 0;
				poll(&pfd, 1, 1000);
				if (pfd.revents & POLLIN)
					continue;
			}
			if (errno == EINTR)
				continue;
			lua_pushnil(L);
			lua_pushstring(L, strerror(errno));
			return 2;
		}

		if (!r)
			break;

		if (write(fd, buf, r) != r) {
			lua_pushnil(L);
			lua_pushstring(L, "write error");
			return 2;
		}

		total += r;
		if (max_size >= 0)
			remaining -= r;

		/* For non-blocking behavior, break after reading any amount of data */
		if (total > 0)
			break;
	}

	lua_pushnumber(L, total);
	return 1;
}

/* Multipart parser state */
enum multipart_state {
	MP_BOUNDARY_SEARCH,
	MP_HEADERS,
	MP_FILE_DATA,
	MP_END
};

static int multipart_error(lua_State *L, FILE *file, int headers_ref,
			   const char *location, const char *msg)
{
	if (file) {
		fclose(file);
		if (location)
			remove(location);
	}

	if (headers_ref != LUA_NOREF)
		luaL_unref(L, LUA_REGISTRYINDEX, headers_ref);

	lua_pushnil(L);
	lua_pushstring(L, msg);
	return 2;
}

/* Helper: Parse multipart headers and push Lua table on stack */
static int parse_multipart_headers(lua_State *L, const char *header_data, size_t header_len)
{
	char *header_copy = malloc(header_len + 1);
	if (!header_copy)
		return -1;

	memcpy(header_copy, header_data, header_len);
	header_copy[header_len] = '\0';

	lua_newtable(L);

	/* Parse each header line */
	char *line_start = header_copy;
	char *line_end;

	while ((line_end = strstr(line_start, "\r\n")) != NULL) {
		*line_end = '\0';

		/* Find the colon separator */
		char *colon = strchr(line_start, ':');
		if (colon) {
			*colon = '\0';
			char *header_name = line_start;
			char *header_value = colon + 1;

			/* Convert header name to lowercase to simplify parsing lua-side */
			for (char *p = header_name; p < colon; p++)
				*p = tolower((unsigned char)*p);

			/* Skip leading whitespace in value */
			while (*header_value == ' ' || *header_value == '\t')
				header_value++;

			/* Add to Lua table */
			lua_pushstring(L, header_value);
			lua_setfield(L, -2, header_name);
		}

		/* Move to next line */
		line_start = line_end + 1;
		if (*line_start == '\n')
			line_start++;
	}

	free(header_copy);
	return 0;
}

static int uh_lua_recv_multipart_to_file(lua_State *L)
{
	struct pollfd pfd = {
		.fd = STDIN_FILENO,
		.events = POLLIN,
	};
	char buf[8192];
	char boundary[256];
	char full_boundary[260];  /* "--" + boundary */
	char next_boundary[264];  /* "\r\n--" + boundary */

	ssize_t total_written = 0, max_size, remaining;
	int fd, read_size, boundary_len, full_boundary_len, next_boundary_len;
	FILE *file = NULL;
	const char *location, *boundary_param;
	size_t location_len, boundary_param_len;

	enum multipart_state state = MP_BOUNDARY_SEARCH;
	char *data_start = buf;
	int buf_pos = 0;
	int headers_ref = LUA_NOREF;

	/* Validate and extract parameters */
	location = luaL_checklstring(L, 1, &location_len);
	max_size = luaL_checkinteger(L, 2);
	if (max_size < 0) {
		lua_pushnil(L);
		lua_pushstring(L, "Invalid maximum size");
		return 2;
	}

	boundary_param = luaL_checklstring(L, 3, &boundary_param_len);
	if (boundary_param_len == 0 || boundary_param_len >= sizeof(boundary)) {
		lua_pushnil(L);
		lua_pushstring(L, "Boundary too long");
		return 2;
	}

	/* Prepare boundary strings */
	memcpy(boundary, boundary_param, boundary_param_len);
	boundary[boundary_param_len] = '\0';
	boundary_len = boundary_param_len;
	snprintf(full_boundary, sizeof(full_boundary), "--%s", boundary);
	full_boundary_len = boundary_len + 2;
	snprintf(next_boundary, sizeof(next_boundary), "\r\n--%s", boundary);
	next_boundary_len = boundary_len + 4;

	/* Open file for writing */
	file = fopen(location, "wb");
	if (!file) {
		lua_pushnil(L);
		lua_pushstring(L, "Cannot open file for writing");
		return 2;
	}

	fd = fileno(file);
	if (fd < 0)
		return multipart_error(L, file, headers_ref, location,
				       "Cannot get file descriptor");

	remaining = max_size;

	/* Main processing loop */
	while (state != MP_END) {
		/* Read more data if buffer is not full */
		if (buf_pos < (int)sizeof(buf)) {
			/* Determine how much to read */
			if (max_size >= 0 && remaining <= 0)
				return multipart_error(L, file, headers_ref,
						       location,
						       "Maximum size exceeded");

			read_size = (int)sizeof(buf) - buf_pos;
			if (max_size >= 0 && remaining < (ssize_t)read_size)
				read_size = (int)remaining;

			ssize_t r;
			while (1) {
				r = read(STDIN_FILENO, buf + buf_pos, read_size);

				if (r > 0)
					break; // Successful read

				if (r == 0)
					return multipart_error(L, file, headers_ref,
								location, "Unexpected end of input");

				if (errno == EINTR)
					continue; // Retry read

				if ((errno == EWOULDBLOCK || errno == EAGAIN) && max_size < 0) {
					pfd.revents = 0;
					poll(&pfd, 1, 1000);
					if (pfd.revents & POLLIN)
						continue; // Retry read after waiting
				}

				return multipart_error(L, file, headers_ref,
						       location, strerror(errno));
			}

			buf_pos += r;
			if (max_size >= 0)
				remaining -= r;
		}

		char *search_start = data_start;
		int search_len = buf_pos - (data_start - buf);

		/* Process buffer content based on current state */
		switch (state) {
		case MP_BOUNDARY_SEARCH: {
			char *boundary_pos = memmem(search_start, search_len, full_boundary, full_boundary_len);
			if (!boundary_pos) {
				/* Keep enough data to handle split boundary */
				int keep_len = full_boundary_len - 1;
				if (keep_len < 0)
					return multipart_error(L, file, headers_ref,
							       location, "Invalid boundary");
				if (search_len > keep_len) {
					if (keep_len > 0)
						memmove(buf, buf + buf_pos - keep_len, keep_len);
					buf_pos = keep_len;
					data_start = buf;
				}
				break;
			}

			data_start = boundary_pos + full_boundary_len;
			/* Skip CRLF after boundary */
			if (data_start + 1 < buf + buf_pos && data_start[0] == '\r' && data_start[1] == '\n')
				data_start += 2;

			state = MP_HEADERS;
			break;
		}

		case MP_HEADERS: {
			const size_t header_limit = 4096;
			char *header_end = memmem(search_start, search_len, "\r\n\r\n", 4);
			if (!header_end) {
				if (search_len >= (int)sizeof(buf) ||
				    search_len > (int)header_limit)
					return multipart_error(L, file, headers_ref,
							       location,
							       "Multipart headers too large");

				if (data_start != buf) {
					memmove(buf, data_start, search_len);
					data_start = buf;
					buf_pos = search_len;
				}

				break;
			}

			/* Parse headers if not already done */
			if (headers_ref == LUA_NOREF) {
				size_t header_len = (header_end + 2) - search_start; // +2 to include the final CRLF
				if (parse_multipart_headers(L, search_start, header_len) < 0)
					return multipart_error(L, file, headers_ref,
							       location,
							       "Out of memory");
				headers_ref = luaL_ref(L, LUA_REGISTRYINDEX);
			}

			data_start = header_end + 4;
			state = MP_FILE_DATA;
			break;
		}

		case MP_FILE_DATA: {
			char *boundary_pos = memmem(search_start, search_len, next_boundary, next_boundary_len);
			int write_len;
			int is_final_write = 0;

			if (boundary_pos) {
				/* Write file data up to boundary */
				write_len = boundary_pos - search_start;
				is_final_write = 1;
			} else {
				/* Write safe portion, keeping enough for boundary detection */
				write_len = search_len - next_boundary_len;
			}

			if (write_len > 0) {
				/* Write all data, handling partial writes and EINTR */
				ssize_t written = 0;
				while (written < (ssize_t)write_len) {
					ssize_t n = write(fd, search_start + written, write_len - written);
					if (n < 0) {
						if (errno == EINTR)
							continue;
						return multipart_error(L, file, headers_ref,
								       location, "write error");
					}
					written += n;
				}
				total_written += write_len;
			}

			if (is_final_write) {
				state = MP_END;
			} else if (write_len > 0) {
				/* Move remaining data to start of buffer */
				int remaining_buf = search_len - write_len;
				memmove(buf, search_start + write_len, remaining_buf);
				buf_pos = remaining_buf;
				data_start = buf;
			}
			break;
		}

		case MP_END:
			break;
		}
	}

	fclose(file);

	if (headers_ref == LUA_NOREF) {
		remove(location);
		lua_pushnil(L);
		lua_pushstring(L, "Missing multipart headers");
		return 2;
	}

	/* Return byte count and headers table */
	lua_pushnumber(L, total_written);
	lua_rawgeti(L, LUA_REGISTRYINDEX, headers_ref);
	luaL_unref(L, LUA_REGISTRYINDEX, headers_ref);

	return 2;
}

static int
uh_lua_strconvert(lua_State *L, int (*convert)(char *, int, const char *, int))
{
	const char *in_buf;
	static char out_buf[4096];
	size_t in_len;
	int out_len;

	in_buf = luaL_checklstring(L, 1, &in_len);
	out_len = convert(out_buf, sizeof(out_buf), in_buf, in_len);

	if (out_len < 0) {
		const char *error;

		if (out_len == -1)
			error = "buffer overflow";
		else
			error = "malformed string";

		luaL_error(L, "%s on URL conversion\n", error);
	}

	lua_pushlstring(L, out_buf, out_len);
	return 1;
}

static int uh_lua_urldecode(lua_State *L)
{
	return uh_lua_strconvert(L, ops->urldecode);
}

static int uh_lua_urlencode(lua_State *L)
{
	return uh_lua_strconvert(L, ops->urlencode);
}

static lua_State *uh_lua_state_init(struct lua_prefix *lua)
{
	const char *msg = "(unknown error)";
	const char *status;
	lua_State *L;
	int ret;

	L = luaL_newstate();
	luaL_openlibs(L);

	/* build uhttpd api table */
	lua_newtable(L);

	lua_pushcfunction(L, uh_lua_send);
	lua_setfield(L, -2, "send");

	lua_pushcfunction(L, uh_lua_send);
	lua_setfield(L, -2, "sendc");

	lua_pushcfunction(L, uh_lua_recv);
	lua_setfield(L, -2, "recv");

	lua_pushcfunction(L, uh_lua_recv_to_file);
	lua_setfield(L, -2, "recv_to_file");

	lua_pushcfunction(L, uh_lua_recv_multipart_to_file);
	lua_setfield(L, -2, "recv_multipart_to_file");

	lua_pushcfunction(L, uh_lua_urldecode);
	lua_setfield(L, -2, "urldecode");

	lua_pushcfunction(L, uh_lua_urlencode);
	lua_setfield(L, -2, "urlencode");

	lua_pushstring(L, conf.docroot);
	lua_setfield(L, -2, "docroot");

	lua_setglobal(L, "uhttpd");

	ret = luaL_loadfile(L, lua->handler);
	if (ret) {
		status = "loading";
		goto error;
	}

	ret = lua_pcall(L, 0, 0, 0);
	if (ret) {
		status = "initializing";
		goto error;
	}

	lua_getglobal(L, UH_LUA_CB);
	if (!lua_isfunction(L, -1)) {
		fprintf(stderr, "Error: Lua handler %s provides no "
		                UH_LUA_CB "() callback.\n", lua->handler);
		exit(1);
	}

	lua->ctx = L;

	return L;

error:
	if (!lua_isnil(L, -1))
		msg = lua_tostring(L, -1);

	fprintf(stderr, "Error %s %s Lua handler: %s\n",
	        status, lua->handler, msg);
	exit(1);
	return NULL;
}

static void lua_main(struct client *cl, struct path_info *pi, char *url)
{
	struct blob_attr *cur;
	const char *error;
	struct env_var *var;
	lua_State *L = _L;
	int path_len, prefix_len;
	char *str;
	int rem;

	lua_getglobal(L, UH_LUA_CB);

	/* new env table for this request */
	lua_newtable(L);

	prefix_len = strlen(pi->name);
	path_len = strlen(url);
	str = strchr(url, '?');
	if (str) {
		if (*(str + 1))
			pi->query = str + 1;
		path_len = str - url;
	}

	if (prefix_len > 0 && pi->name[prefix_len - 1] == '/')
		prefix_len--;

	if (path_len > prefix_len) {
		lua_pushlstring(L, url + prefix_len,
				path_len - prefix_len);
		lua_setfield(L, -2, "PATH_INFO");
	}

	for (var = ops->get_process_vars(cl, pi); var->name; var++) {
		if (!var->value)
			continue;

		lua_pushstring(L, var->value);
		lua_setfield(L, -2, var->name);
	}

	lua_pushnumber(L, 0.9 + (cl->request.version / 10.0));
	lua_setfield(L, -2, "HTTP_VERSION");

	lua_newtable(L);
	blob_for_each_attr(cur, cl->hdr.head, rem) {
		lua_pushstring(L, blobmsg_data(cur));
		lua_setfield(L, -2, blobmsg_name(cur));
	}
	lua_setfield(L, -2, "headers");

	switch(lua_pcall(L, 1, 0, 0)) {
	case LUA_ERRMEM:
	case LUA_ERRRUN:
		error = luaL_checkstring(L, -1);
		if (!error)
			error = "(unknown error)";

		printf("Status: 500 Internal Server Error\r\n\r\n"
	       "Unable to launch the requested Lua program:\n"
	       "  %s: %s\n", pi->phys, error);
	}

	exit(0);
}

static void lua_handle_request(struct client *cl, char *url, struct path_info *pi)
{
	struct lua_prefix *p;
	static struct path_info _pi;

	list_for_each_entry(p, &conf.lua_prefix, list) {
		if (!ops->path_match(p->prefix, url))
			continue;

		pi = &_pi;
		pi->name = p->prefix;
		pi->phys = p->handler;

		_L = p->ctx;

		if (!ops->create_process(cl, pi, url, lua_main)) {
			ops->client_error(cl, 500, "Internal Server Error",
			                  "Failed to create CGI process: %s",
			                  strerror(errno));
		}

		return;
	}

	ops->client_error(cl, 500, "Internal Server Error",
	                  "Failed to lookup matching handler");
}

static bool check_lua_url(const char *url)
{
	struct lua_prefix *p;

	list_for_each_entry(p, &conf.lua_prefix, list)
		if (ops->path_match(p->prefix, url))
			return true;

	return false;
}

static struct dispatch_handler lua_dispatch = {
	.script = true,
	.check_url = check_lua_url,
	.handle_request = lua_handle_request,
};

static int lua_plugin_init(const struct uhttpd_ops *o, struct config *c)
{
	struct lua_prefix *p;

	ops = o;
	_conf = c;

	list_for_each_entry(p, &conf.lua_prefix, list)
		uh_lua_state_init(p);

	ops->dispatch_add(&lua_dispatch);
	return 0;
}

struct uhttpd_plugin uhttpd_plugin = {
	.init = lua_plugin_init,
};
