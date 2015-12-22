#!/bin/sh
PATCH_PATH=$(pwd)/`dirname $0`
cd ${PATCH_PATH}

cp ./lualib-src/*.c ../lualib-src
cp ./lualib/snax/wsgateserver.lua ../lualib/snax
cp ./service/wsgate.lua ../service
cp ./examples/* ../examples

cd ..
# cp Makefile_1.0.patch ../Makefile
# or
SYSTEM=`uname -s`
echo ${SYSTEM}
if [ ${SYSTEM} = "Linux" ]; then
	sed 's/LUA_CLIB =/LUA_CLIB = websocketnetpack clientwebsocket/' Makefile > Makefile.1
	sed 's/$(LUA_CLIB_PATH)\/clientsocket.so/$(LUA_CLIB_PATH)\/websocketnetpack.so : lualib-src\/lua-websocketnetpack.c | $(LUA_CLIB_PATH)\n\t$(CC) $(CFLAGS) $(SHARED) $^ -Iskynet-src -o $@ \n\n$(LUA_CLIB_PATH)\/clientwebsocket.so :lualib-src\/lua-clientwebsocket.c | $(LUA_CLIB_PATH)\n\t$(CC) $(CFLAGS) $(SHARED) $^ -o $@ -lpthread\n\n$(LUA_CLIB_PATH)\/clientsocket.so/' Makefile.1 > Makefile.patch
elif [ ${SYSTEM} = "Darwin" ]; then
	gsed 's/LUA_CLIB =/LUA_CLIB = websocketnetpack clientwebsocket/' Makefile > Makefile.1
	gsed 's/$(LUA_CLIB_PATH)\/clientsocket.so/$(LUA_CLIB_PATH)\/websocketnetpack.so : lualib-src\/lua-websocketnetpack.c | $(LUA_CLIB_PATH)\n\t$(CC) $(CFLAGS) $(SHARED) $^ -Iskynet-src -o $@ \n\n$(LUA_CLIB_PATH)\/clientwebsocket.so :lualib-src\/lua-clientwebsocket.c | $(LUA_CLIB_PATH)\n\t$(CC) $(CFLAGS) $(SHARED) $^ -o $@ -lpthread\n\n$(LUA_CLIB_PATH)\/clientsocket.so/' Makefile.1 > Makefile.patch
fi
rm Makefile.1
mv Makefile Makefile.skynet
mv Makefile.patch Makefile
echo 'patch end'
echo 'please do: make ''PLATFORM'' # PLATFORM can be linux, macosx, freebsd now'
# sed 's/LUA_CLIB =/LUA_CLIB = websocketnetpack clientwebsocket/' Makefile.old > Makefile.1
# sed 's/$(LUA_CLIB_PATH)\/clientsocket.so/$(LUA_CLIB_PATH)\/websocketnetpack.so : lualib-src\/lua-websocketnetpack.c | $(LUA_CLIB_PATH)\n\t$(CC) $(CFLAGS) $(SHARED) $^ -Iskynet-src -o $@ \n\n$(LUA_CLIB_PATH)\/clientwebsocket.so :lualib-src\/lua-clientwebsocket.c | $(LUA_CLIB_PATH)\n\t$(CC) $(CFLAGS) $(SHARED) $^ -o $@ -lpthread\n\n$(LUA_CLIB_PATH)\/clientsocket.so/' Makefile.1 > Makefile
