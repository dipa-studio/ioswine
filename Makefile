# Top-level Makefile for ioswine

ANDROID_NDK_HOME ?= $(shell ls -d $(HOME)/Android/ndk/* 2>/dev/null | sort -V | tail -n 1)
NDK_BIN          := $(ANDROID_NDK_HOME)/toolchains/llvm/prebuilt/linux-x86_64/bin
NDK_CLANG        := $(NDK_BIN)/clang
LD64             := $(NDK_BIN)/ld64.lld
HOST_CLANG       ?= clang
DSC              ?= $(if $(IOSWINE_DSC),$(IOSWINE_DSC),vendor-local/23G90__iPhone17,3/dyld_shared_cache_arm64e)

LOADER_CFLAGS := --target=aarch64-linux-android29 -ffreestanding -nostdlib -static -fno-pie -no-pie \
                 -fno-stack-protector -fno-builtin -fno-exceptions -fno-asynchronous-unwind-tables \
                 -O2 -g -Wall -Wextra -Wl,--image-base=0x7000000000 -Wl,--build-id=none -I. -Iloader

MACHO_CFLAGS := -target arm64-apple-ios17.0 -ffreestanding -nostdlib -fno-stack-protector -fblocks -O2 -Itests
LD64_FLAGS   := -arch arm64 -platform_version ios 17.0 17.0 -fixup_chains -e _main

LSK_CFLAGS   := -target arm64e-apple-ios17.0 -ffreestanding -nostdlib \
                -fno-stack-protector -fno-builtin -O2 -fptrauth-calls -fptrauth-returns \
                -Ilibsystem_kernel/src -Iandroid/surface/src/common
LSK_LD64_FLAGS := -arch arm64e -platform_version ios 17.0 17.0 -dylib \
                  -install_name /usr/lib/system/libsystem_kernel.dylib

LOADER_SRCS   := loader/start.S loader/rt.c $(filter-out loader/rt.c, $(wildcard loader/*.c))
SELFTEST_SRCS := loader/start.S loader/rt.c tests/host/rt_selftest.c
LSK_SRCS      := $(wildcard libsystem_kernel/src/*.c)
LSK_SRC_OBJS  := $(patsubst libsystem_kernel/src/%.c,build/lsk/src/%.o,$(LSK_SRCS))

LIBSYS_NAMES  := write_hello printf_hello malloc_test pthread_test cg_hello cg_present mach_test dispatch_test xpc_ping sqlite_test
LIBSYS_TESTS  := $(patsubst %,build/tests/libsys/%,$(LIBSYS_NAMES))

.PHONY: all loader selftest tests check clean loader_skip lsk tls-sites libsys-tests surface-test surface-apk xpcwire-test snapshot-test hostpresent host-tests

all: selftest tests loader lsk libsys-tests

xpcwire-test: build/tests/host/xpcwire_test
	build/tests/host/xpcwire_test

build/tests/host/xpcwire_test: tests/host/xpcwire_test.c libsystem_kernel/src/xpcwire.c
	@mkdir -p build/tests/host
	$(HOST_CLANG) -std=c99 -Wall -Wextra -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
		-Ilibsystem_kernel/src -o $@ $^

snapshot-test: build/tests/host/snapshot_test
	build/tests/host/snapshot_test

build/tests/host/snapshot_test: tests/host/snapshot_test.c libsystem_kernel/src/snapshot.c libsystem_kernel/src/snapshot.h android/surface/src/common/protocol.h
	@mkdir -p build/tests/host
	$(HOST_CLANG) -std=c11 -Wall -Wextra -O1 -g -fsanitize=address,undefined -fno-sanitize=function -fno-sanitize-recover=all \
		-DSNAPSHOT_HOST_TEST=1 -Ilibsystem_kernel/src -Iandroid/surface/src/common -o $@ tests/host/snapshot_test.c libsystem_kernel/src/snapshot.c

surface-test: build/surface/test_protocol build/surface/test_launcher_ui build/surface/test_server_resize
	build/surface/test_protocol
	build/surface/test_launcher_ui
	build/surface/test_server_resize

build/surface/test_protocol: android/surface/tests/host/test_protocol.c android/surface/src/common/protocol.c
	@mkdir -p build/surface
	$(CC) -std=c11 -Wall -Wextra -Iandroid/surface/src/common $^ -o $@ -lm

build/surface/test_launcher_ui: android/surface/tests/host/test_launcher_ui.c \
                                 android/surface/src/server/ui.c \
                                 android/surface/src/server/sysinfo.c
	@mkdir -p build/surface
	$(CC) -std=c11 -Wall -Wextra -Iandroid/surface/src/server $^ -o $@

# server.c resize wiring. The test #includes server.c
# (white-box access to static on_cmd/g_s), so server.c is a build
# dependency but NOT a link source; the Android APIs server.c calls
# are stubbed by tests/host/stubs (test-only, never in the APK).
build/surface/test_server_resize: android/surface/tests/host/test_server_resize.c \
                                 android/surface/src/server/server.c \
                                 android/surface/src/server/ui.c \
                                 android/surface/src/server/sysinfo.c \
                                 android/surface/src/common/protocol.c \
                                 android/surface/src/common/touch_input.c \
                                 android/surface/tests/host/stubs/test_server_stubs.c \
                                 android/surface/tests/host/stubs/android/log.h \
                                 android/surface/tests/host/stubs/android/native_window.h \
                                 android/surface/tests/host/stubs/android/configuration.h \
                                 android/surface/tests/host/stubs/android/native_activity.h \
                                 android/surface/tests/host/stubs/android/input.h \
                                 android/surface/tests/host/stubs/android/keycodes.h \
                                 android/surface/tests/host/stubs/android/looper.h \
                                 android/surface/tests/host/stubs/android/choreographer.h \
                                 android/surface/tests/host/stubs/android_native_app_glue.h
	@mkdir -p build/surface
	$(CC) -std=c11 -Wall -Wextra -Iandroid/surface/tests/host/stubs -Iandroid/surface/src/common -Iandroid/surface/src/server android/surface/tests/host/test_server_resize.c android/surface/src/server/ui.c android/surface/src/server/sysinfo.c android/surface/src/common/protocol.c android/surface/src/common/touch_input.c android/surface/tests/host/stubs/test_server_stubs.c -o $@ -lpthread

surface-apk: build/surface/ioswine-surface.apk

build/surface/ioswine-surface.apk: android/surface/src/server/server.c \
                                   android/surface/src/server/sysinfo.c \
                                   android/surface/src/server/sysinfo.h \
                                   android/surface/src/server/ui.c \
                                   android/surface/src/server/ui.h \
                                   android/surface/src/server/font8x16.h \
                                   android/surface/src/server/server_egl.c \
                                   android/surface/src/server/AndroidManifest.xml \
                                   android/surface/src/server/CMakeLists.txt \
                                   android/surface/src/server/package.sh \
                                   android/surface/src/common/protocol.c \
                                   android/surface/src/common/protocol.h \
                                   android/surface/src/common/touch_input.c \
                                   android/surface/src/common/touch_input.h
	@mkdir -p build/surface
	SURFACE_OUT_DIR="$(CURDIR)/build/surface" \
	SURFACE_KEYSTORE="$(CURDIR)/build/surface/debug.keystore" \
	ANDROID_NDK_HOME="$(ANDROID_NDK_HOME)" \
	android/surface/src/server/package.sh apk

selftest: build/rt-selftest

tests: build/tests/hello build/tests/fixups

ifeq ($(wildcard loader/main.c),)
LOADER_TARGET := loader_skip
else
LOADER_TARGET := build/ioswine-loader
endif

loader: $(LOADER_TARGET)

loader_skip:
	@echo "loader/main.c not found; skipping build/ioswine-loader"

build/rt-selftest: $(SELFTEST_SRCS)
	@mkdir -p build
	$(NDK_CLANG) $(LOADER_CFLAGS) -o $@ $^

build/ioswine-loader: $(LOADER_SRCS)
	@mkdir -p build
	$(NDK_CLANG) $(LOADER_CFLAGS) -o $@ $^

build/tests/%: tests/%.c
	@mkdir -p build/tests
	$(HOST_CLANG) $(MACHO_CFLAGS) -c $< -o build/tests/$*.o
	@if [ -f tests/$*.S ]; then \
		$(HOST_CLANG) $(MACHO_CFLAGS) -c tests/$*.S -o build/tests/$*_s.o && \
		$(LD64) $(LD64_FLAGS) -o $@ build/tests/$*.o build/tests/$*_s.o; \
	else \
		$(LD64) $(LD64_FLAGS) -o $@ build/tests/$*.o; \
	fi

libsys-tests: $(LIBSYS_TESTS)

build/tests/libsys/cg_hello: tests/libsys/cg_hello.c tests/libsys/libSystem.tbd \
                             tests/libsys/CoreGraphics.tbd tests/libsys/ImageIO.tbd \
                             tests/libsys/CoreFoundation.tbd
	@mkdir -p build/tests/libsys
	$(HOST_CLANG) $(MACHO_CFLAGS) -Itests/libsys -c $< -o build/tests/libsys/cg_hello.o
	$(LD64) -arch arm64 -platform_version ios 17.0 17.0 -fixup_chains -o $@ build/tests/libsys/cg_hello.o \
		tests/libsys/CoreGraphics.tbd tests/libsys/ImageIO.tbd tests/libsys/CoreFoundation.tbd tests/libsys/libSystem.tbd

build/tests/libsys/cg_present: tests/libsys/cg_present.c tests/libsys/libSystem.tbd \
                               tests/libsys/CoreGraphics.tbd
	@mkdir -p build/tests/libsys
	$(HOST_CLANG) $(MACHO_CFLAGS) -Itests/libsys -c $< -o build/tests/libsys/cg_present.o
	$(LD64) -arch arm64 -platform_version ios 17.0 17.0 -fixup_chains -o $@ build/tests/libsys/cg_present.o \
		tests/libsys/CoreGraphics.tbd tests/libsys/libSystem.tbd

build/tests/libsys/dispatch_test: tests/libsys/dispatch_test.c tests/libsys/libSystem.tbd tests/libsys/CoreFoundation.tbd
	@mkdir -p build/tests/libsys
	$(HOST_CLANG) $(MACHO_CFLAGS) -Itests/libsys -c $< -o build/tests/libsys/dispatch_test.o
	$(LD64) -arch arm64 -platform_version ios 17.0 17.0 -fixup_chains -o $@ build/tests/libsys/dispatch_test.o \
		tests/libsys/CoreFoundation.tbd tests/libsys/libSystem.tbd

build/tests/libsys/sqlite_test: tests/libsys/sqlite_test.c tests/libsys/libSystem.tbd tests/libsys/libsqlite3.tbd
	@mkdir -p build/tests/libsys
	$(HOST_CLANG) $(MACHO_CFLAGS) -Itests/libsys -c $< -o build/tests/libsys/sqlite_test.o
	$(LD64) -arch arm64 -platform_version ios 17.0 17.0 -fixup_chains -o $@ build/tests/libsys/sqlite_test.o \
		tests/libsys/libsqlite3.tbd tests/libsys/libSystem.tbd

build/tests/libsys/%: tests/libsys/%.c tests/libsys/libSystem.tbd
	@mkdir -p build/tests/libsys
	$(HOST_CLANG) $(MACHO_CFLAGS) -Itests/libsys -c $< -o build/tests/libsys/$*.o
	$(LD64) -arch arm64 -platform_version ios 17.0 17.0 -fixup_chains -o $@ build/tests/libsys/$*.o tests/libsys/libSystem.tbd

lsk: build/libsystem_kernel.dylib

build/lsk/src/%.o: libsystem_kernel/src/%.c
	@mkdir -p build/lsk/src
	$(HOST_CLANG) $(LSK_CFLAGS) -c $< -o $@

build/lsk/gen/stubs.S: gen/data/libsystem_kernel.exports.tsv $(LSK_SRC_OBJS)
	@mkdir -p build/lsk/gen
	python3 gen/stubgen.py --tsv $< --out $@ --nm $(NDK_BIN)/llvm-nm $(LSK_SRC_OBJS)

build/lsk/gen/stubs.o: build/lsk/gen/stubs.S
	$(HOST_CLANG) -target arm64e-apple-ios17.0 -c $< -o $@

build/libsystem_kernel.dylib: $(LSK_SRC_OBJS) build/lsk/gen/stubs.o
	@mkdir -p build
	$(LD64) $(LSK_LD64_FLAGS) -o $@ $^
	@python3 -c 'import struct; f=open("$@", "r+b"); f.seek(8); f.write(struct.pack("<I", 0x80000002)); f.close()'

tls-sites: build/dsc/tls_sites.bin

build/dsc/tls_sites.bin: gen/tls_sites.py $(DSC)
	@mkdir -p build/dsc
	python3 gen/tls_sites.py $(DSC) $@

check: all
	@tools/check.sh

clean:
	rm -rf build/ioswine-loader build/rt-selftest build/tests build/lsk build/libsystem_kernel.dylib build/hostpresent
	@mkdir -p build/tmp

hostpresent: build/hostpresent

build/hostpresent: tools/hostpresent/hostpresent.c \
                   tools/hostpresent/hostpresent.h \
                   android/surface/src/common/protocol.c \
                   android/surface/src/common/protocol.h \
                   android/surface/src/common/touch_input.c \
                   android/surface/src/common/touch_input.h
	@mkdir -p build
	$(CC) -std=c11 -Wall -Wextra -O2 -g -Iandroid/surface/src/common \
		tools/hostpresent/hostpresent.c \
		android/surface/src/common/protocol.c \
		android/surface/src/common/touch_input.c \
		-o $@ -lpthread

build/lsk/src/snapshot.o: libsystem_kernel/src/snapshot.c libsystem_kernel/src/snapshot.h libsystem_kernel/src/linux.h
build/lsk/src/init.o: libsystem_kernel/src/init.c libsystem_kernel/src/snapshot.h libsystem_kernel/src/linux.h
build/lsk/src/pathmap.o: libsystem_kernel/src/pathmap.c libsystem_kernel/src/pathmap.h libsystem_kernel/src/linux.h
build/lsk/src/bsd.o: libsystem_kernel/src/bsd.c libsystem_kernel/src/pathmap.h libsystem_kernel/src/linux.h libsystem_kernel/src/errno.h
build/lsk/src/hidsys.o: libsystem_kernel/src/hidsys.c libsystem_kernel/src/hidsys.h libsystem_kernel/src/ipc.h libsystem_kernel/src/linux.h
build/lsk/src/bkshid.o: libsystem_kernel/src/bkshid.c libsystem_kernel/src/bkshid.h libsystem_kernel/src/svchost.h libsystem_kernel/src/bsxpc.h libsystem_kernel/src/xpcwire.h libsystem_kernel/src/ipc.h libsystem_kernel/src/linux.h
build/lsk/src/keyboard.o: libsystem_kernel/src/keyboard.c libsystem_kernel/src/keyboard.h libsystem_kernel/src/present.h android/surface/src/common/protocol.h libsystem_kernel/src/linux.h
build/lsk/src/carender.o: libsystem_kernel/src/carender.c libsystem_kernel/src/carender.h libsystem_kernel/src/cadamage.h libsystem_kernel/src/present.h libsystem_kernel/src/cadisplay.h libsystem_kernel/src/snapshot.h libsystem_kernel/src/linux.h
build/lsk/src/cadamage.o: libsystem_kernel/src/cadamage.c libsystem_kernel/src/cadamage.h
build/lsk/src/touch.o: libsystem_kernel/src/touch.c libsystem_kernel/src/touch.h libsystem_kernel/src/present.h libsystem_kernel/src/keyboard.h libsystem_kernel/src/linux.h
build/lsk/src/present.o: libsystem_kernel/src/present.c libsystem_kernel/src/present.h android/surface/src/common/protocol.h libsystem_kernel/src/linux.h
build/lsk/src/present_api.o: libsystem_kernel/src/present_api.c libsystem_kernel/src/present.h android/surface/src/common/protocol.h
build/lsk/src/cadisplay.o: libsystem_kernel/src/cadisplay.c libsystem_kernel/src/cadisplay.h libsystem_kernel/src/present.h libsystem_kernel/src/iosurface.h libsystem_kernel/src/iokit.h
build/lsk/src/ipc.o: libsystem_kernel/src/ipc.c libsystem_kernel/src/hidsys.h
build/lsk/src/vm.o: libsystem_kernel/src/vm.c libsystem_kernel/src/ipc.h libsystem_kernel/src/linux.h libsystem_kernel/src/errno.h
build/lsk/src/sem.o: libsystem_kernel/src/sem.c libsystem_kernel/src/linux.h libsystem_kernel/src/errno.h

host-tests: build/tests/host/snapshot_test
	tools/host_tests.sh
	build/tests/host/snapshot_test
