PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
ifneq ($(MAKECMDGOALS),)
ifeq ($(filter-out host test clean,$(MAKECMDGOALS)),)
HOST_ONLY := 1
endif
endif
ifndef HOST_ONLY
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
endif

CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Werror -pthread -Ivendor -Ivendor/zstd -Ibuild -Isrc
CXXFLAGS += -DCPPHTTPLIB_OPENSSL_SUPPORT -DCPPHTTPLIB_ZLIB_SUPPORT -DPSFOIL_PS5 -Ivendor/zlib
OPENSSL_ROOT ?= $(PS5_SYSROOT)/user/homebrew
CXXFLAGS += -I$(OPENSSL_ROOT)/include
LDLIBS := -L$(OPENSSL_ROOT)/lib -lssl -lcrypto -lSceSystemService -lSceUserService -lSceNetCtl -pthread

all: build/psfoil.elf build/manifest.json
ZLIB_SRCS := adler32 crc32 deflate inflate inffast inftrees trees zutil
ZLIB_OBJS := $(addprefix build/zlib/,$(addsuffix .o,$(ZLIB_SRCS)))
ZSTD_SRCS := $(wildcard vendor/zstd/common/*.c) $(wildcard vendor/zstd/decompress/*.c)
ZSTD_OBJS := $(patsubst vendor/zstd/%.c,build/zstd/%.o,$(ZSTD_SRCS))
HOST_ZSTD_OBJS := $(patsubst vendor/zstd/%.c,build/host-zstd/%.o,$(ZSTD_SRCS))
build/zstd/%.o: vendor/zstd/%.c
	mkdir -p $(dir $@)
	$(CC) -O2 -DZSTD_LEGACY_SUPPORT=0 -DZSTD_DISABLE_ASM=1 -Ivendor/zstd -c $< -o $@
build/host-zstd/%.o: vendor/zstd/%.c
	mkdir -p $(dir $@)
	cc -O2 -DZSTD_LEGACY_SUPPORT=0 -DZSTD_DISABLE_ASM=1 -Ivendor/zstd -c $< -o $@
build/zlib/%.o: vendor/zlib/%.c
	mkdir -p build/zlib
	$(CC) -O2 -Ivendor/zlib -c $< -o $@
build/assets.h: VERSION web/index.html web/app.js web/style.css web/uploads.js web/boot.css tools/embed.py assets/icon0.png web/icons $(wildcard web/icons/*.png) $(wildcard web/icons/*.svg)
	python3 tools/embed.py
APP_SRCS := src/main.cpp src/platform.cpp src/package.cpp src/download.cpp src/upload.cpp src/storage.cpp
build/psfoil.elf: $(APP_SRCS) $(wildcard src/*.h) build/assets.h vendor/httplib.h vendor/json.hpp $(ZLIB_OBJS) $(ZSTD_OBJS)
	$(CXX) $(CXXFLAGS) $(APP_SRCS) $(ZLIB_OBJS) $(ZSTD_OBJS) -o $@ $(LDLIBS)
build/manifest.json: build/psfoil.elf tools/manifest.py VERSION
	python3 tools/manifest.py
host: build/psfoil-host
build/psfoil-host: $(APP_SRCS) $(wildcard src/*.h) build/assets.h vendor/httplib.h vendor/json.hpp $(HOST_ZSTD_OBJS)
	c++ -std=c++17 -O1 -Wall -Wextra -Werror -pthread -DCPPHTTPLIB_OPENSSL_SUPPORT -DCPPHTTPLIB_ZLIB_SUPPORT -Ivendor -Ivendor/zstd -Ibuild -Isrc $(HOST_CPPFLAGS) $(APP_SRCS) $(HOST_ZSTD_OBJS) -o build/psfoil-host $(HOST_LDFLAGS) -lssl -lcrypto -lz
test: host
	python3 tests/startup.py
	python3 tests/integration.py
	python3 tests/migration.py
	python3 tests/local_conversions.py
	python3 tests/downloads.py
	python3 tests/uploads.py
	python3 tests/icons.py
clean:
	rm -f build/psfoil.elf build/psfoil-host build/assets.h build/manifest.json build/icon0.png
	rm -rf build/zlib build/zstd build/host-zstd
.PHONY: all host test clean
