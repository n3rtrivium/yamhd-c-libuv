CC=gcc
UV_PATH=libuv/include
CFLAGS=-g -Wall -I$(UV_PATH)
LIBS=

uname_S=$(shell uname -s)

ifeq (Darwin, $(uname_S))
CFLAGS+=-framework CoreServices
UV_LIB=libuv/.libs/libuv.dylib
endif

ifeq (Linux, $(uname_S))
LIBS=-lrt -ldl -lm -pthread -lcurl
UV_LIB=libuv/libuv.so
endif

all: yamhd-c-libuv

yamhd-c-libuv: $(UV_LIB)
	$(CC) $(CFLAGS) $(LIBS) -o yamhd-c-libuv yamhd-c-libuv.c $(UV_LIB)
	
$(UV_LIB):
	cd libuv; sh autogen.sh; ./configure;
	make -C libuv

clean:
	rm -rf yamhd-c-libuv yamhd-c-libuv.dSYM 

clean-all: clean
	make -C libuv clean

doc:
	docco -L res/docco-lang.json *.c

.PHONY: all clean clean-all doc
