#include <lua.h>
#include <lauxlib.h>

/* On success the chunk's return values are on the stack. On error Lua leaves its error object. */
int port_lua_chunk(lua_State *state, const char *bytes, size_t size, const char *name)
{
    int error = luaL_loadbuffer(state, bytes, size, name);
    return error ? error : lua_pcall(state, 0, LUA_MULTRET, 0);
}
