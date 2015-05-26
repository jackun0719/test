NATIVE_CC ?= gcc
INTERNALDIR ?= ..

_all: all

all:
	$(NATIVE_CC) -g -m32 -Wall \
	-I$(INTERNALDIR)/include \
	-I$(WLANCMN_DIR)/include \
	-o mkdsetimg mkdsetimg.c lz.c

clean:
	rm -f mkdsetimg

clobber: clean
