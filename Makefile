# geist-runtime — embeddable model runner on geistlib.
#
#   make             stub library, conformance tests, example (no engine needed)
#   make test        run them
#   make sanitize    the same under ASan + UBSan
#   make runtime     the real libgeistr.a on the pinned geistlib engine
#   make test-real   the real runtime against the reference model (GEIST_TEST_MODEL,
#                    default: the engine's fetched SmolLM2; make fetch-model)
#   make chat-real   build/chat-real: the example chat on the real runtime
#   make parity SERVE_DIR=../geist-serve   templates byte-identical to geist-serve,
#                    and the same catalog
#
# src/template.c and src/stream.c are the runtime's text side; src/runtime.c
# binds them to geistlib (#4). src/stub.c implements include/geistr.h without
# an engine, for the API conformance tests.

CC       ?= cc
CXX      ?= c++
BUILD    ?= build
WARN     := -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wno-sign-conversion
CFLAGS   ?= -O2 -g
CXXFLAGS ?= -O2 -g
override CFLAGS   += -std=c23 $(WARN) -Iinclude -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE
override CXXFLAGS += -std=c++20 $(WARN) -Iinclude
LDLIBS   += -lpthread

LIB  := $(BUILD)/libgeistr-stub.a
TEXT := $(BUILD)/template.o $(BUILD)/stream.o $(BUILD)/catalog.o

all: $(LIB) $(BUILD)/test_api $(BUILD)/test_cxx $(BUILD)/test_template $(BUILD)/test_stream $(BUILD)/test_window $(BUILD)/test_catalog $(BUILD)/chat

$(BUILD)/%.o: src/%.c src/*.h include/*.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB): $(BUILD)/stub.o $(TEXT)
	ar rcs $@ $^

$(BUILD)/test_api: tests/test_api.c $(LIB)
	$(CC) $(CFLAGS) $< $(LIB) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/test_cxx: tests/test_cxx.cpp $(LIB) include/geistr.h
	$(CXX) $(CXXFLAGS) $< $(LIB) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/test_template: tests/test_template.c $(TEXT)
	$(CC) $(CFLAGS) -Isrc $< $(TEXT) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/test_stream: tests/test_stream.c $(TEXT)
	$(CC) $(CFLAGS) -Isrc $< $(TEXT) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/test_window: tests/test_window.c src/window.h | $(BUILD)
	$(CC) $(CFLAGS) -Isrc $< $(LDFLAGS) -o $@

$(BUILD)/test_catalog: tests/test_catalog.c $(TEXT)
	$(CC) $(CFLAGS) $< $(TEXT) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/chat: examples/chat.c $(LIB)
	$(CC) $(CFLAGS) $< $(LIB) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD):
	mkdir -p $@

test: all
	$(BUILD)/test_api
	$(BUILD)/test_cxx
	$(BUILD)/test_template
	$(BUILD)/test_stream
	$(BUILD)/test_window
	$(BUILD)/test_catalog
	python3 tests/test_catalog.py $(BUILD)/test_catalog models/catalog.json
	@out=$$(printf 'Hallo Welt\nnoch einmal\n' | $(BUILD)/chat stub:echo) && \
	  echo "$$out" | grep -q 'Echo: noch einmal' && echo "example chat: two turns passed" || \
	  { echo "example chat failed: $$out"; exit 1; }

# ---- the real runtime on geistlib (#4) ---------------------------------------
ENGINE_GOALS := runtime test-real chat-real fetch-model
ifneq (,$(filter $(ENGINE_GOALS),$(MAKECMDGOALS)))
GEIST_REPO ?= https://github.com/geisten/geistlib.git
GEIST_REF  ?= 5dd7e1747df86092a320e638c66993afd409e3b6
GEISTLIB   ?= geistlib
ENGINE := $(shell GEIST_REPO='$(GEIST_REPO)' GEIST_REF='$(GEIST_REF)' \
                  GEISTLIB='$(GEISTLIB)' sh scripts/sync-engine.sh >&2 && echo ok)
ifneq ($(ENGINE),ok)
$(error engine sync failed — see the messages above)
endif
GEMM_PROVIDER ?= native
ifeq ($(shell uname -s)-$(shell uname -m),Darwin-arm64)
BACKENDS ?= cpu_neon cpu_scalar metal
endif
TARGET ?= $(shell $(GEISTLIB)/mk/detect-target.sh)
include $(GEISTLIB)/mk/target-$(TARGET).mk
include $(GEISTLIB)/mk/gemm-$(GEMM_PROVIDER).mk
ENGINE_MODE ?= release
ENGINE_LIB  := $(GEISTLIB)/lib/$(TARGET)/$(ENGINE_MODE)/libgeist.a
ENGINE_LINK := $(ENGINE_LIB) $(LDFLAGS_TARGET) $(LDLIBS_TARGET) $(GEMM_LDLIBS)
GEIST_TEST_MODEL ?= $(GEISTLIB)/gguf_artifacts/smollm2-360m-instruct-q8_0.gguf
endif

RUNTIME := $(BUILD)/libgeistr.a

# Always delegate: the engine's own make is incremental, and a plain file
# target would go stale on a GEIST_REF bump.
$(ENGINE_LIB): FORCE
	$(MAKE) -C $(GEISTLIB) lib TARGET=$(TARGET) MODE=$(ENGINE_MODE) \
		GEMM_PROVIDER=$(GEMM_PROVIDER) BACKENDS="$(BACKENDS)"

$(BUILD)/runtime.o: src/runtime.c src/*.h include/geistr.h $(ENGINE_LIB) | $(BUILD)
	$(CC) $(CFLAGS) -isystem $(GEISTLIB)/include -c $< -o $@

$(RUNTIME): $(BUILD)/runtime.o $(TEXT)
	ar rcs $@ $^

runtime: $(RUNTIME)

$(BUILD)/test_real: tests/test_real.c $(RUNTIME) $(ENGINE_LIB)
	$(CC) $(CFLAGS) -isystem $(GEISTLIB)/include $< $(RUNTIME) $(ENGINE_LINK) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/chat-real: examples/chat.c $(RUNTIME) $(ENGINE_LIB)
	$(CC) $(CFLAGS) -isystem $(GEISTLIB)/include $< $(RUNTIME) $(ENGINE_LINK) $(LDFLAGS) $(LDLIBS) -o $@

chat-real: $(BUILD)/chat-real

test-real: $(BUILD)/test_real
	GEIST_TEST_MODEL="$(GEIST_TEST_MODEL)" $(BUILD)/test_real

fetch-model:
	$(MAKE) -C $(GEISTLIB) fetch-model

FORCE:

# Byte parity with geist-serve's renderer (#2): make parity SERVE_DIR=../geist-serve
parity: $(TEXT)
	@test -f "$(SERVE_DIR)/src/template.c" || { echo "set SERVE_DIR to a geist-serve checkout"; exit 1; }
	$(CC) $(CFLAGS) -Wno-conversion -I$(SERVE_DIR)/src -Itools/parity tools/parity/serve.c $(SERVE_DIR)/src/template.c -o $(BUILD)/parity_serve
	$(CC) $(CFLAGS) -Isrc -Itools/parity tools/parity/geistr.c $(TEXT) -o $(BUILD)/parity_geistr
	$(BUILD)/parity_serve > $(BUILD)/parity_serve.txt
	$(BUILD)/parity_geistr > $(BUILD)/parity_geistr.txt
	cmp $(BUILD)/parity_serve.txt $(BUILD)/parity_geistr.txt
	@echo "parity with geist-serve: $$(grep -c '^== ' $(BUILD)/parity_serve.txt) renders identical"
	cmp models/catalog.json $(SERVE_DIR)/models/catalog.json
	$(BUILD)/test_catalog --validate < $(SERVE_DIR)/models/catalog.json

SAN := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
sanitize:
	$(MAKE) BUILD=$(BUILD)/san CFLAGS="-O1 -g $(SAN)" CXXFLAGS="-O1 -g $(SAN)" LDFLAGS="$(SAN)" test

clean:
	rm -rf $(BUILD)

.PHONY: all test sanitize parity clean runtime test-real chat-real fetch-model FORCE
