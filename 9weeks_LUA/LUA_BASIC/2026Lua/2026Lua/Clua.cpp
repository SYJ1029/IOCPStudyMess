#include <iostream>
#include "include/lua.hpp"

#pragma comment(lib, "lua55.lib")

int main() {

	const char* buff = "print \"Hello, Lua!\"";

	lua_State* L = luaL_newstate();
	luaL_openlibs(L);
	luaL_loadbuffer(L, buff, strlen(buff), "line") || luaL_loadstring(L, buff);
	int error = lua_pcall(L, 0, LUA_MULTRET, 0);
	if (error) {
		std::cerr << "Error executing Lua code: " << lua_tostring(L, -1) << std::endl;
		lua_pop(L, 1); // Remove error message from stack
	}
	lua_close(L);
	return 0;
}