#include "ray_lib.h"
#include "ray_state.h"
#include "ray_stream.h"

/* used by udp and stream */
uv_buf_t rayL_alloc_cb(uv_handle_t* handle, size_t size) {
  ray_state_t* self = container_of(handle, ray_state_t, h);
  memset(self->buf.base, 0, self->buf.len);
  return self->buf;
}

/* used by tcp and pipe */
void rayL_connect_cb(uv_connect_t* req, int status) {
  ray_state_t* self = container_of(req, ray_state_t, r);
  lua_pushinteger(self->L, status);
  rayS_notify(self, 1);
}

static void _read_cb(uv_stream_t* stream, ssize_t len, uv_buf_t buf) {
  ray_state_t* self = container_of(stream, ray_state_t, h);
  lua_settop(self->L, 0);
  TRACE("read callback\n");
  if (len < 0) {
    uv_err_t err = uv_last_error(stream->loop);
    lua_pushnil(self->L);
    lua_pushstring(self->L, uv_strerror(err));
    if (err.code == UV_EOF) {
      TRACE("got EOF\n");
      rayL_stream_stop(self);
      rayS_notify(self, 2);
    }
    else {
      TRACE("got ERROR, closing\n");
      rayS_close(self);
    }
  }
  else {
    lua_pushinteger(self->L, len);
    lua_pushlstring(self->L, (char*)buf.base, len);
    rayS_notify(self, 2);
    assert(lua_gettop(self->L) == 0);
  }
}

static void _write_cb(uv_write_t* req, int status) {
  TRACE("write callback\n");
  ray_state_t* self = (ray_state_t*)req->data;
  lua_settop(self->L, 0);
  lua_pushinteger(self->L, status);
  rayS_notify(self, 1);
}

static void _shutdown_cb(uv_shutdown_t* req, int status) {
  ray_state_t* self = (ray_state_t*)req->data;
  lua_settop(self->L, 0);
  lua_pushinteger(self->L, status);
  rayS_notify(self, 1);
}

static void _listen_cb(uv_stream_t* server, int status) {
  ray_state_t* self = container_of(server, ray_state_t, h);
  TRACE("connection...\n");
  if (lua_gettop(self->L) > 0) {
    TRACE("have client socket...\n");
    luaL_checktype(self->L, 1, LUA_TUSERDATA);
    ray_state_t* conn = (ray_state_t*)lua_touserdata(self->L, 1);
    TRACE("accepting %p\n", conn);
    int rv = uv_accept(&self->h.stream, &conn->h.stream);
    if (rv) {
      TRACE("ACCEPT ERROR!\n");
      uv_err_t err = uv_last_error(self->h.stream.loop);
      lua_pushnil(self->L);
      lua_pushstring(self->L, uv_strerror(err));
    }
    TRACE("%p[%p] NOTIFIED, BEFORE: %i\n", self, self->L, lua_gettop(self->L));
    rayS_notify(self, LUA_MULTRET);
    TRACE("%p[%p] NOTIFIED, STACK NOW: %i\n", self, self->L, lua_gettop(self->L));
  }
  else {
    lua_pushboolean(self->L, 1);
  }
}

static void _close_cb(uv_handle_t* handle) {
  ray_state_t* self = container_of(handle, ray_state_t, h);
  rayS_notify(self, lua_gettop(self->L));
  rayM_state_close(self);
}

static ray_vtable_t ray_stream_v = {
  await: rayM_stream_await,
  rouse: rayM_stream_rouse,
  close: rayM_stream_close
};

int rayM_stream_await(ray_state_t* self, ray_state_t* that) {
  TRACE("STREAM AWAIT\n");
  return rayL_stream_stop(self);
}
int rayM_stream_rouse(ray_state_t* self, ray_state_t* from) {
  TRACE("ROUSE STREAM\n");
  return rayL_stream_start(self);
}
int rayM_stream_close(ray_state_t* self) {
  uv_close(&self->h.handle, _close_cb);
  if (self->buf.len) {
    free(self->buf.base);
    self->buf.base = NULL;
    self->buf.len  = 0;
  }
  return 1;
}


static int ray_stream_listen(lua_State* L) {
  luaL_checktype(L, 1, LUA_TUSERDATA);
  ray_state_t* self = (ray_state_t*)lua_touserdata(L, 1);
  int backlog = luaL_optinteger(L, 2, 128);
  if (uv_listen(&self->h.stream, backlog, _listen_cb)) {
    uv_err_t err = uv_last_error(self->h.stream.loop);
    return luaL_error(L, "listen: %s", uv_strerror(err));
  }
  return 1;
}

static int ray_stream_accept(lua_State *L) {
  luaL_checktype(L, 1, LUA_TUSERDATA);
  ray_state_t* self = (ray_state_t*)lua_touserdata(L, 1);
  if (lua_gettop(self->L) > 0) {
    TRACE("have backlog...\n");
    ray_state_t* conn = (ray_state_t*)lua_touserdata(L, 2);
    TRACE("connection object: %p\n", conn);
    luaL_checktype(self->L, 1, LUA_TBOOLEAN);
    lua_pop(self->L, 1);
    TRACE("popped boolean, top now: %i\n", lua_gettop(self->L));
    int rv = uv_accept(&self->h.stream, &conn->h.stream);
    if (rv) {
      TRACE("ARSE got an error from accept\n");
      uv_err_t err = uv_last_error(self->h.stream.loop);
      lua_settop(L, 0);
      lua_pushnil(L);
      lua_pushstring(L, uv_strerror(err));
      return 2;
    }
    return 1;
  }
  else {
    ray_state_t* curr = rayS_get_self(L);
    lua_xmove(L, self->L, 1);
    TRACE("calling await: curr %p, self: %p\n", curr, self);
    return rayS_await(curr, self);
  }
}

int rayL_stream_start(ray_state_t* self) {
  self->flags |= RAY_START;
  return uv_read_start(&self->h.stream, rayL_alloc_cb, _read_cb);
}
int rayL_stream_stop(ray_state_t* self) {
  self->flags &= ~RAY_START;
  return uv_read_stop(&self->h.stream);
}


#define STREAM_ERROR(L,fmt,loop) do { \
  uv_err_t err = uv_last_error(loop); \
  lua_settop(L, 0); \
  lua_pushboolean(L, 0); \
  lua_pushfstring(L, fmt, uv_strerror(err)); \
  TRACE("STREAM ERROR: %s\n", lua_tostring(L, -1)); \
} while (0)

static int ray_stream_start(lua_State* L) {
  ray_state_t* self = (ray_state_t*)lua_touserdata(L, 1);
  if (rayL_stream_start(self)) {
    rayL_stream_stop(self);
    rayS_close(self);
    STREAM_ERROR(L, "read start: %s", rayS_get_loop(L));
    return 2;
  }
  lua_pushboolean(L, 1);
  return 1;
}

static int ray_stream_stop(lua_State* L) {
  ray_state_t* self = (ray_state_t*)lua_touserdata(L, 1);
  if (rayL_stream_stop(self)) {
    STREAM_ERROR(L, "read stop: %s", rayS_get_loop(L));
    return 2;
  }
  lua_pushboolean(L, 1);
  return 1;
}

static int ray_stream_read(lua_State* L) {
  ray_state_t* self = (ray_state_t*)lua_touserdata(L, 1);
  ray_state_t* curr = rayS_get_self(L);
  int len = luaL_optinteger(L, 2, RAY_BUF_SIZE);
  if (lua_gettop(self->L) > 0) {
    lua_xmove(self->L, L, 2);
    return 2;
  }
  if (self->buf.len != len) {
    self->buf.base = realloc(self->buf.base, len);
    self->buf.len  = len;
  }
  if (!rayS_is_start(self)) {
    rayL_stream_start(self);
  }
  return rayS_await(curr, self);
}

static int ray_stream_write(lua_State* L) {
  ray_state_t* self = (ray_state_t*)lua_touserdata(L, 1);
  ray_state_t* curr = rayS_get_self(L);

  size_t len;
  const char* chunk = luaL_checklstring(L, 2, &len);

  uv_buf_t buf = uv_buf_init((char*)chunk, len);
  curr->r.req.data = self;

  if (uv_write(&curr->r.write, &self->h.stream, &buf, 1, _write_cb)) {
    rayS_close(self);
    STREAM_ERROR(L, "write: %s", rayS_get_loop(L));
    return 2;
  }
  if (rayS_is_start(self)) {
    rayL_stream_stop(self);
  }
  return rayS_await(curr, self);
}

static int ray_stream_shutdown(lua_State* L) {
  ray_state_t* self = (ray_state_t*)lua_touserdata(L, 1);
  ray_state_t* curr = rayS_get_self(L);
  curr->r.req.data = self;

  uv_shutdown(&curr->r.shutdown, &self->h.stream, _shutdown_cb);

  return rayS_await(curr, self);
}
static int ray_stream_readable(lua_State* L) {
  ray_state_t* self = (ray_state_t*)lua_touserdata(L, 1);
  lua_pushboolean(L, uv_is_readable(&self->h.stream));
  return 1;
}

static int ray_stream_writable(lua_State* L) {
  ray_state_t* self = (ray_state_t*)lua_touserdata(L, 1);
  lua_pushboolean(L, uv_is_writable(&self->h.stream));
  return 1;
}

static int ray_stream_close(lua_State* L) {
  ray_state_t* self = (ray_state_t*)lua_touserdata(L, 1);
  ray_state_t* curr = rayS_get_self(L);
  if (!rayS_is_closed(self)) {
    rayS_close(self);
    return rayS_await(curr, self);
  }
  return 0;
}

void rayL_stream_free(ray_state_t* self) {
  if (!rayS_is_closed(self)) {
    rayS_close(self);
    TRACE("free stream: %p\n", self);
    if (self->buf.len) {
      free(self->buf.base);
      self->buf.base = NULL;
      self->buf.len  = 0;
    }
  }
}

static int ray_stream_free(lua_State* L) {
  ray_state_t* self = (ray_state_t*)lua_touserdata(L, 1);
  rayL_stream_free(self);
  return 1;
}

static int ray_stream_tostring(lua_State* L) {
  ray_state_t* self = (ray_state_t*)lua_touserdata(L, 1);
  lua_pushfstring(L, "userdata<ray.stream>: %p", self);
  return 1;
}

luaL_Reg ray_stream_meths[] = {
  {"read",      ray_stream_read},
  {"readable",  ray_stream_readable},
  {"write",     ray_stream_write},
  {"writable",  ray_stream_writable},
  {"start",     ray_stream_start},
  {"stop",      ray_stream_stop},
  {"listen",    ray_stream_listen},
  {"accept",    ray_stream_accept},
  {"shutdown",  ray_stream_shutdown},
  {"close",     ray_stream_close},
  {"__gc",      ray_stream_free},
  {"__tostring",ray_stream_tostring},
  {NULL,        NULL}
};
