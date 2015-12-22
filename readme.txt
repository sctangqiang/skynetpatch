对skynet增加websocket的协议处理

使用步骤:
在skynet下建立patch目录,然后将这里的所有文件copy到这个目录
cd patch
./dopatch.sh
cd ..

然后就可以按照skynet官网说明来make了.
websocket的例子, 使用了agent的例子
./skynet examples/wsconfig

另一终端运行 
./3rd/lua/lua examples/wsclient.lua
