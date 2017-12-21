#!/bin/sh
PATCH_PATH=$(pwd)/`dirname $0`
cd ${PATCH_PATH}

cp ./lualib-src/*.c ../skynet/lualib-src
cp ./lualib/snax/*.lua ../skynet/lualib/snax
cp ./service/*.lua ../skynet/service
cp ./examples/* ../skynet/examples
