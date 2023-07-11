SRC=stream_mux.cpp rtp.c ./mpeg/*.c
TARGET=stream2gb28181
LIB=-L./libs -lavcodec -lavfilter -lavformat -lavutil -lswresample -lswscale  -lx264
INCLUDE=-I./mpeg/include -I/usr/local/libffmpeg/include
all:${TARGET} 

${TARGET}: ${SRC}
	g++ $^ -std=c++11 -g  -fpermissive ${LIB} ${INCLUDE} -o $@ -pthread -DDEBUG   
