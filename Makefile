CC=gcc
UV_PATH=libuv
UV_CONFIG=--disable-dtrace --enable-shared --prefix="$(shell pwd)"
CFLAGS=-g -Wall -I$(UV_PATH)/include
LIBS=

uname_S=$(shell uname -s)

ifeq (Darwin, $(uname_S))
CFLAGS+=-framework CoreServices
UV_EXT=11.dylib
endif

ifeq (Linux, $(uname_S))
LIBS=-lrt -ldl -lm -pthread -lcurl -Wl,-rpath -Wl,lib
UV_EXT=so.11 
endif

UV_LIB=lib/libuv.$(UV_EXT)

all: yamhd-c-libuv

yamhd-c-libuv: $(UV_LIB)
	$(CC) $(CFLAGS) $(LIBS) -o yamhd-c-libuv yamhd-c-libuv.c $(UV_LIB)
	
$(UV_LIB):
	mkdir -p lib
	cd $(UV_PATH); sh autogen.sh;
	cd $(UV_PATH); ./configure $(UV_CONFIG);
	make -C $(UV_PATH)
	cp $(UV_PATH)/.libs/libuv.$(UV_EXT) lib

clean:
	rm -rf yamhd-c-libuv yamhd-c-libuv.dSYM 

clean-doc:
	rm -rf docs

clean-libuv:
	make -C libuv clean
	rm -rf lib

clean-all: clean clean-doc clean-libuv

doc:
	docco -L res/docco-lang.json *.c
	mv docs/yamhd-c-libuv.html docs/index.html

.PHONY: all clean clean-all clean-doc clean-libuv doc

# linux static
# gcc -g -Wall -Ilibuv/include yamhd-c-libuv.c -static libuv/.libs/libuv.a -lrt -ldl -lm -pthread -lcurl -o yamhd-c-libuv
