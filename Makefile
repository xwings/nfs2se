# Browser build of Need for Speed II SE. The translated game is compiled from
# the NFSIISE submodule at build time and never vendored; src/platform is the
# browser platform layer (C, wasm) and src/web the page around it.
#
#   make            build dist/ (needs emcc/em++/em-config from emsdk on PATH)
#   make serve      serve dist/ with the headers SharedArrayBuffer needs
#   make test       C, JS unit and browser tests

EMCC ?= emcc
EMXX ?= em++
EMCONFIG ?= em-config
NODE ?= node
NPX ?= npx
PORT ?= 8080

BUILD := build
DIST := dist
UPSTREAM := third_party/NFSIISE
GAME_CPP := $(UPSTREAM)/src/Cpp/NFS2SE.cpp
# Files the game expects next to its data; shipped by the submodule. The page
# reads their names from files/manifest.txt.
UPSTREAM_FILES := $(UPSTREAM)/Need\ For\ Speed\ II\ SE
SUPPORT_FILES := install.win nfs2se.conf.template text.eng text.fre text.ger text.ita text.spa text.swe
# Licence notices of what dist/ contains, including the toolchain code in it.
NOTICE_FILES := $(addprefix $(DIST)/,LICENSE NOTICE LICENSE.emscripten LICENSE.musl LICENSE.compiler-rt)

PLATFORM_HDR := $(wildcard src/platform/*.h)
PLATFORM_OBJ := $(patsubst src/platform/%.c,$(BUILD)/obj/%.o,$(wildcard src/platform/*.c))
WEB_FILES := $(patsubst src/web/%,$(DIST)/%,$(wildcard src/web/*))

GAME_CXXFLAGS := -std=c++14 -O2 -DNDEBUG -fno-rtti -fno-exceptions -Wno-c++11-narrowing \
	-D__WORDSIZE=32 -include endian.h -pthread
CFLAGS := -std=gnu11 -O2 -pthread -Wall -Wextra -Werror
# --fatal-warnings: wasm-ld only warns about an import whose signature differs
# from its definition and links a trapping stub instead.
# PTHREAD_POOL_SIZE: workers loaded with the page, enough for the platform's
# four threads (game, timer, audio producer, exit watchdog) and four of the
# game's own; a further thread starts once a worker is created for it.
# -Wno-pthreads-mem-growth: growth slows Emscripten's own JS heap accesses,
# which re-check the buffer; the page's views of the shared regions are made
# once and stay valid, as growth never detaches a shared buffer.
LDFLAGS := -O2 -pthread -Wl,--fatal-warnings \
	-sPROXY_TO_PTHREAD -sPTHREAD_POOL_SIZE=8 -sINVOKE_RUN=0 -sEXIT_RUNTIME=1 \
	-sALLOW_MEMORY_GROWTH -sINITIAL_MEMORY=256MB -sMAXIMUM_MEMORY=2GB -Wno-pthreads-mem-growth \
	-sSTACK_SIZE=8MB -sDEFAULT_PTHREAD_STACK_SIZE=2MB \
	-sMODULARIZE -sEXPORT_ES6 -sEXPORT_NAME=createNfs2se -sENVIRONMENT=web,worker \
	-sEXPORTED_RUNTIME_METHODS=callMain,FS,HEAPU8,UTF8ToString -lidbfs.js

.PHONY: all clean serve test test-c test-unit test-e2e

all: $(DIST)/nfs2se.js $(addprefix $(DIST)/files/,$(SUPPORT_FILES) manifest.txt) $(WEB_FILES) $(NOTICE_FILES)

$(BUILD)/obj/NFS2SE.o: $(GAME_CPP)
	@mkdir -p $(@D)
	$(EMXX) $(GAME_CXXFLAGS) -c $< -o $@

$(BUILD)/obj/%.o: src/platform/%.c $(PLATFORM_HDR)
	@mkdir -p $(@D)
	$(EMCC) $(CFLAGS) -c $< -o $@

$(DIST)/nfs2se.js: $(BUILD)/obj/NFS2SE.o $(PLATFORM_OBJ)
	@mkdir -p $(@D)
	$(EMXX) $(LDFLAGS) $^ -o $@

$(DIST)/files/%: $(UPSTREAM_FILES)/%
	@mkdir -p $(@D)
	cp "$<" "$@"

$(DIST)/files/manifest.txt: Makefile
	@mkdir -p $(@D)
	printf '%s\n' $(SUPPORT_FILES) > $@

$(DIST)/LICENSE $(DIST)/NOTICE: $(DIST)/%: %
	@mkdir -p $(@D)
	cp $< $@

$(DIST)/LICENSE.emscripten: $(DIST)/nfs2se.js
	cp "$$($(EMCONFIG) EMSCRIPTEN_ROOT)/LICENSE" $@

$(DIST)/LICENSE.musl: $(DIST)/nfs2se.js
	cp "$$($(EMCONFIG) EMSCRIPTEN_ROOT)/system/lib/libc/musl/COPYRIGHT" $@

$(DIST)/LICENSE.compiler-rt: $(DIST)/nfs2se.js
	cp "$$($(EMCONFIG) EMSCRIPTEN_ROOT)/system/lib/compiler-rt/LICENSE.TXT" $@

$(DIST)/%: src/web/%
	@mkdir -p $(@D)
	cp $< $@

serve: all
	$(NODE) tools/serve.mjs $(DIST) $(PORT)

# C tests run in node as wasm32, so structures have the game's 32-bit layout.
# Each tests/c/<name>_test.c is linked with the sources in TEST_SRC_<name>
# and built with the extra flags in TEST_CFLAGS_<name>.
C_TESTS := $(patsubst tests/c/%_test.c,%,$(wildcard tests/c/*_test.c))
TEST_SRC_config := src/platform/config.c
TEST_SRC_dinput := src/platform/dinput.c src/platform/config.c
TEST_CFLAGS_dinput := -pthread -sPTHREAD_POOL_SIZE=2 -Wno-pthreads-mem-growth
TEST_SRC_fmt := src/platform/fmt_x86.c
TEST_SRC_glide := src/platform/glide.c
TEST_SRC_kernel32 := src/platform/kernel32.c src/platform/config.c
TEST_CFLAGS_kernel32 := -pthread -sPTHREAD_POOL_SIZE=2 -Wno-pthreads-mem-growth
TEST_SRC_records := src/platform/records.c src/platform/config.c
TEST_SRC_timer := src/platform/timer.c
TEST_SRC_user32 := src/platform/user32.c
TEST_SRC_wrapper := src/platform/wrapper.c src/platform/config.c
TEST_CFLAGS_wrapper := -pthread -sPTHREAD_POOL_SIZE=2 -Wno-pthreads-mem-growth

.SECONDEXPANSION:
$(BUILD)/tests/%.cjs: tests/c/%_test.c $$(TEST_SRC_$$*) $(PLATFORM_HDR) tests/c/check.h
	@mkdir -p $(@D)
	$(EMCC) -std=gnu11 -O1 -Wall -Wextra -Werror -Isrc/platform -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH -sENVIRONMENT=node \
		$(TEST_CFLAGS_$*) $(filter %.c,$^) -o $@ -lm

test: test-c test-unit test-e2e

test-c: $(patsubst %,$(BUILD)/tests/%.cjs,$(C_TESTS))
	@for t in $^; do echo "$$t"; $(NODE) $$t || exit 1; done

test-unit:
	$(NODE) --test tests/unit/*.test.mjs

test-e2e: all
	$(NPX) --no playwright test

clean:
	rm -rf $(BUILD) $(DIST)
