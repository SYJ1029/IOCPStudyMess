#include <iostream>
#include "include/lua.hpp"

#pragma comment(lib, "lua55.lib")

int main(void)
{
	int rows, cols;
	lua_State* L = luaL_newstate(); //루아를연다.
	luaL_openlibs(L); //루아표준라이브러리를연다.
	luaL_loadfile(L, "dragon.lua");
	int error = lua_pcall(L, 0, 0, 0);

	if (error)
	{
		std::cerr << "Error: " << lua_tostring(L, -1) << std::endl;
		lua_pop(L, 1);
	}

	lua_getglobal(L, "pos_x");
	lua_getglobal(L, "pos_y");
	rows = (int)lua_tonumber(L, -2);
	cols = (int)lua_tonumber(L, -1);
	std::cout << "Rows " << rows << ", Cols " << cols << std::endl;
	lua_pop(L, 2);
	lua_close(L);
	return 0;
}
