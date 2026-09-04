CC ?= cc
CFLAGS = -Wall -Wextra -O2 -I. -Ivendor/cjson
LDFLAGS = -lm

# raylib: prefer pkg-config, else set RAYLIB_DIR to a raylib release dir
# (containing include/ and lib/), e.g. make RAYLIB_DIR=~/raylib-5.5_linux_amd64
ifeq ($(shell pkg-config --exists raylib && echo yes),yes)
    RAYLIB_CFLAGS = $(shell pkg-config --cflags raylib)
    RAYLIB_LIBS = $(shell pkg-config --libs raylib)
else ifdef RAYLIB_DIR
    RAYLIB_CFLAGS = -I$(RAYLIB_DIR)/include
    RAYLIB_LIBS = $(RAYLIB_DIR)/lib/libraylib.a -lm -lpthread -ldl
endif

COMMON = ws.c net.c protocol.c vendor/cjson/cJSON.c

client: main.c $(COMMON)
	$(CC) $(CFLAGS) $(RAYLIB_CFLAGS) -o $@ $^ $(RAYLIB_LIBS) $(LDFLAGS)

test_cli: test_cli.c $(COMMON)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# cross-build a self-contained windows exe.
# needs a mingw-w64 toolchain (WINCC) and a raylib win64_mingw-w64 release dir:
#   make windows WINCC=x86_64-w64-mingw32-gcc RAYLIB_WIN_DIR=~/raylib-5.5_win64_mingw-w64
WINCC ?= x86_64-w64-mingw32-gcc
WINDRES ?= $(dir $(WINCC))x86_64-w64-mingw32-windres

windows: main.c $(COMMON) gridmmo.rc
	$(WINDRES) gridmmo.rc gridmmo_rc.o
	$(WINCC) $(CFLAGS) -I$(RAYLIB_WIN_DIR)/include -o gridmmo.exe \
	    main.c $(COMMON) gridmmo_rc.o \
	    $(RAYLIB_WIN_DIR)/lib/libraylib.a \
	    -lws2_32 -lwinmm -lgdi32 -lopengl32 -mwindows

clean:
	rm -f client test_cli gridmmo.exe gridmmo_rc.o

.PHONY: windows clean
