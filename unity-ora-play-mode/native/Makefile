# Builds the Linux link for Ora 0.2: libora-audio.so (Unity) and ora-link.node (Electron). See oralink.h.
NODE_INCLUDE ?= $(shell node -p "require('path').join(process.execPath, '..', '..', 'include', 'node')" 2>/dev/null)
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
CFLAGS += -std=gnu11 -fPIC -fvisibility=hidden -D_GNU_SOURCE

all: build/libora-audio.so build/ora-link.node

build/libora-audio.so: oralink.c ora_audio_unity.c oralink.h
	@mkdir -p build
	$(CC) $(CFLAGS) -shared -o $@ oralink.c ora_audio_unity.c -lpthread

# Node-API symbols come from the Electron process that loads the addon.
build/ora-link.node: oralink.c ora_link_node.c oralink.h
	@mkdir -p build
	$(CC) $(CFLAGS) -I$(NODE_INCLUDE) -I/usr/include/node -DNODE_GYP_MODULE_NAME=ora_link -shared -o $@ oralink.c ora_link_node.c -lpthread -Wl,--unresolved-symbols=ignore-all

clean:
	rm -rf build

.PHONY: all clean
