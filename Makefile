# gridrouter: the Python module builds the library by itself on first use;
# this Makefile is only a convenience (macOS and Linux; on Windows the module
# builds gridrouter.dll itself).  Needs nothing but the system cc.
CC      ?= cc
CFLAGS  ?= -O2 -std=c99 -Wall -Wextra
PYTHON  ?= python3
ifeq ($(shell uname -s),Darwin)
ARCHS   ?= -arch arm64 -arch x86_64
KPY     ?= /Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3
LIB      = gridrouter/libgridrouter.dylib
SHARED   = -dynamiclib $(ARCHS)
else
KPY     ?= python3
LIB      = gridrouter/libgridrouter.so
SHARED   = -shared -fPIC
endif
SRC      = gridrouter/gridrouter.c gridrouter/gridrouter.h

.PHONY: all test test-quick test-kicad bench clean

all: $(LIB)

$(LIB): $(SRC)
	$(CC) $(CFLAGS) $(SHARED) -o $@ gridrouter/gridrouter.c

test: $(LIB)
	$(PYTHON) tests/run_all.py

test-quick: $(LIB)
	GRIDROUTER_TEST_SCALE=0.2 $(PYTHON) tests/run_all.py

test-kicad: $(LIB)
	$(KPY) tests/run_all.py

bench: $(LIB)
	$(PYTHON) tests/bench.py

clean:
	rm -f $(LIB)
