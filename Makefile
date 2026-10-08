# geist-runtime — embeddable model runner on geistlib.
#
#   make             stub library, conformance tests, example (no engine needed)
#   make core        build/libgeistr-core.a: catalog, fit, templates, text (no engine)
#   make test        run them
#   make sanitize    the same under ASan + UBSan
#   make runtime     the real libgeistr.a on the pinned geistlib engine
#   make test-real   the real runtime against the reference model (GEIST_TEST_MODEL,
#                    default: the engine's fetched SmolLM2; make fetch-model)
#   make chat-real   build/chat-real: the example chat on the real runtime
#   make geistr      build/geistr: the CLI (PULL=0: without the download module)
#   make test-geistr the CLI against the reference model (ONLY=serve,bench: some sections)
#   make wheel       build/wheel/geistr-*.whl: the Python package (#9)
#   make test-python pip install it into a venv; example and tests
#
# src/template.c and src/stream.c are the runtime's text side; src/runtime.c
# binds them to geistlib (#4). src/stub.c implements include/geistr.h without
# an engine, for the API conformance tests; src/common.c is what both share.

CC       ?= cc
CXX      ?= c++
BUILD    ?= build
WARN     := -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wno-sign-conversion
CFLAGS   ?= -O2 -g
CXXFLAGS ?= -O2 -g
override CFLAGS   += -std=c23 $(WARN) -fPIC -Iinclude -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE
override CXXFLAGS += -std=c++20 $(WARN) -Iinclude
LDLIBS   += -lpthread

LIB  := $(BUILD)/libgeistr-stub.a
CORE := $(BUILD)/libgeistr-core.a
TEXT := $(BUILD)/template.o $(BUILD)/stream.o $(BUILD)/catalog.o $(BUILD)/fit.o $(BUILD)/decision_config.o $(BUILD)/decision_profile.o

all: $(LIB) $(CORE) $(BUILD)/test_api $(BUILD)/test_cxx $(BUILD)/test_template $(BUILD)/test_stream $(BUILD)/test_window $(BUILD)/test_catalog $(BUILD)/test_fit $(BUILD)/test_decision_config $(BUILD)/test_decision_profile $(BUILD)/test_render $(BUILD)/test_lineedit $(BUILD)/test_chat $(BUILD)/chat

$(BUILD)/%.o: src/%.c src/*.h include/*.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB): $(BUILD)/stub.o $(BUILD)/common.o $(TEXT)
	ar rcs $@ $^

# Everything that needs no engine: templates, text stages, catalog, fit. For
# processes that never load a model (geist-app): link with this alone.
$(CORE): $(TEXT)
	ar rcs $@ $^

core: $(CORE)

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

$(BUILD)/test_fit: tests/test_fit.c $(TEXT)
	$(CC) $(CFLAGS) $< $(TEXT) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/test_decision_config: tests/test_decision_config.c $(LIB)
	$(CC) $(CFLAGS) $< $(LIB) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/test_decision_profile: tests/test_decision_profile.c tests/native_cases.h tests/fixtures/decisions/*.h $(TEXT)
	$(CC) $(CFLAGS) -Isrc $< $(TEXT) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/test_render: tests/test_render.c tools/geistr/render.c tools/geistr/render.h | $(BUILD)
	$(CC) $(CFLAGS) -Itools/geistr tests/test_render.c tools/geistr/render.c $(LDFLAGS) -o $@

CHAT_PARTS := tools/geistr/conversation.c tools/geistr/json.c tools/geistr/config.c
$(BUILD)/test_chat: tests/test_chat.c $(CHAT_PARTS) tools/geistr/cli.h tools/geistr/json.h $(CORE) | $(BUILD)
	$(CC) $(CFLAGS) -Itools/geistr -Isrc tests/test_chat.c $(CHAT_PARTS) $(CORE) $(LDFLAGS) -o $@

$(BUILD)/test_lineedit: tests/test_lineedit.c tools/geistr/lineedit.c tools/geistr/lineedit.h | $(BUILD)
	$(CC) $(CFLAGS) -Itools/geistr tests/test_lineedit.c tools/geistr/lineedit.c $(LDFLAGS) -o $@

$(BUILD)/chat: examples/chat.c $(LIB)
	$(CC) $(CFLAGS) $< $(LIB) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD):
	mkdir -p $@

test: all $(BUILD)/test_decide_driver
	$(BUILD)/test_api
	$(BUILD)/test_cxx
	$(BUILD)/test_template
	$(BUILD)/test_stream
	$(BUILD)/test_window
	$(BUILD)/test_catalog
	$(BUILD)/test_decision_config
	$(BUILD)/test_decision_profile
	python3 tests/test_decide_cli.py $(BUILD)/test_decide_driver
	python3 tests/test_decision_evidence.py
	python3 tests/test_catalog.py $(BUILD)/test_catalog models/catalog.json
	$(BUILD)/test_fit
	$(BUILD)/test_render
	$(BUILD)/test_lineedit
	$(BUILD)/test_chat
	@out=$$(printf 'Hallo Welt\nnoch einmal\n' | $(BUILD)/chat stub:echo) && \
	  echo "$$out" | grep -q 'Echo: noch einmal' && echo "example chat: two turns passed" || \
	  { echo "example chat failed: $$out"; exit 1; }

# ---- the real runtime on geistlib (#4) ---------------------------------------
ENGINE_GOALS := runtime test-real chat-real fetch-model geistr test-geistr shared wheel test-python test-decision-real test-decision-cli test-decision-python $(BUILD)/test_decision_real
ifneq (,$(filter $(ENGINE_GOALS),$(MAKECMDGOALS)))
GEIST_REPO ?= https://github.com/geisten/geistlib.git
GEIST_REF  ?= 0707c3b1c9e547c909d200331a0750b4ff8e8cbe
GEISTLIB   ?= geistlib
DECISION   ?= 0
ENGINE := $(shell GEIST_REPO='$(GEIST_REPO)' GEIST_REF='$(GEIST_REF)' \
                  GEISTLIB='$(GEISTLIB)' sh scripts/sync-engine.sh >&2 && echo ok)
ifneq ($(ENGINE),ok)
$(error engine sync failed — see the messages above)
endif
GEMM_PROVIDER ?= native
# Self-contained binaries and libraries on macOS: libomp linked statically.
GEIST_STATIC_OMP ?= 1
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
ENGINE_CFLAGS = -DGEISTR_ENGINE_REVISION=\"$(GEIST_REF)\"

# Always delegate: the engine's own make is incremental, and a plain file
# target would go stale on a GEIST_REF bump. -fPIC rides on CC so it reaches
# every engine object (stb has its own rule without EXTRA_CFLAGS): the engine
# goes into libgeistr.so too.
$(ENGINE_LIB): FORCE
	$(MAKE) -C $(GEISTLIB) lib TARGET=$(TARGET) MODE=$(ENGINE_MODE) \
		GEMM_PROVIDER=$(GEMM_PROVIDER) BACKENDS="$(BACKENDS)" CC="$(CC) -fPIC" DECISION=$(DECISION)

$(BUILD)/runtime.o: src/runtime.c src/*.h include/*.h $(ENGINE_LIB) | $(BUILD)
	$(CC) $(CFLAGS) $(ENGINE_CFLAGS) -isystem $(GEISTLIB)/include -c $< -o $@

$(RUNTIME): $(BUILD)/runtime.o $(BUILD)/common.o $(TEXT)
	ar rcs $@ $^

runtime: $(RUNTIME)

$(BUILD)/test_real: tests/test_real.c $(RUNTIME) $(ENGINE_LIB)
	$(CC) $(CFLAGS) -isystem $(GEISTLIB)/include $< $(RUNTIME) $(ENGINE_LINK) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/chat-real: examples/chat.c $(RUNTIME) $(ENGINE_LIB)
	$(CC) $(CFLAGS) -isystem $(GEISTLIB)/include $< $(RUNTIME) $(ENGINE_LINK) $(LDFLAGS) $(LDLIBS) -o $@

chat-real: $(BUILD)/chat-real

# ---- shared library and Python wheel (#9) ---------------------------------------
# libgeistr.{dylib,so}: runtime and engine in one file that exports only geistr_*.
ifeq ($(shell uname -s),Darwin)
SHLIB      := $(BUILD)/libgeistr.dylib
SHLIB_LINK  = -dynamiclib -install_name @rpath/libgeistr.dylib -Wl,-exported_symbol,_geistr_* \
              -Wl,-force_load,$(RUNTIME)
else
SHLIB      := $(BUILD)/libgeistr.so
SHLIB_LINK  = -shared -Wl,--version-script=python/exports.map -Wl,--whole-archive $(RUNTIME) -Wl,--no-whole-archive
endif

$(SHLIB): $(RUNTIME) $(ENGINE_LIB) python/exports.map
	$(CC) $(SHLIB_LINK) $(ENGINE_LINK) $(LDFLAGS) $(LDLIBS) -o $@

shared: $(SHLIB)

# A platform wheel (ctypes, no extension module) in build/wheel/.
wheel: $(SHLIB)
	python3 scripts/build-wheel.py $(SHLIB) $(BUILD)/wheel

$(BUILD)/abi_sizes: tests/abi_sizes.c include/*.h | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

# pip install the wheel into a fresh venv; run the example and the tests.
test-python: wheel $(BUILD)/abi_sizes
	@test -f "$(GEIST_TEST_MODEL)" || { echo "no reference model at $(GEIST_TEST_MODEL): make fetch-model"; exit 1; }
	rm -rf $(BUILD)/venv && python3 -m venv $(BUILD)/venv
	$(BUILD)/venv/bin/pip install -q $(BUILD)/wheel/geistr-*.whl
	cd $(BUILD) && venv/bin/python ../examples/chat.py "$(abspath $(GEIST_TEST_MODEL))" < /dev/null
	$(BUILD)/venv/bin/python tests/test_python.py "$(abspath $(GEIST_TEST_MODEL))" $(BUILD)/abi_sizes

# ---- the geistr CLI (#11) -------------------------------------------------------
# PULL=1 adds the download module (libcurl); PULL=0 builds without network code.
PULL ?= $(shell curl-config --libs >/dev/null 2>&1 && echo 1 || echo 0)
ifeq ($(PULL),1)
GEISTR_PULL := tools/geistr/pull.c
GEISTR_LIBS := $(shell curl-config --libs)
else
GEISTR_PULL := tools/geistr/nopull.c
endif

$(BUILD)/catalog_json.h: models/catalog.json | $(BUILD)
	python3 -c 'import sys; d = open(sys.argv[1], "rb").read(); print("static const unsigned char embedded_catalog[] = {" + ",".join(map(str, d)) + "};")' $< > $@

$(BUILD)/geistr: tools/geistr/geistr.c tools/geistr/decide.c tools/geistr/decide.h tools/geistr/render.c tools/geistr/render.h tools/geistr/lineedit.c tools/geistr/lineedit.h tools/geistr/service.c tools/geistr/service.h tools/geistr/json.c tools/geistr/json.h tools/geistr/config.c tools/geistr/speed.c tools/geistr/conversation.c tools/geistr/chat.c tools/geistr/http.c tools/geistr/cli.h $(GEISTR_PULL) tools/geistr/pull.h $(BUILD)/catalog_json.h $(RUNTIME) $(ENGINE_LIB)
	$(CC) $(CFLAGS) $(GEISTR_CFLAGS) -DGEISTR_ENGINE='"$(GEIST_REF)"' -I$(BUILD) -Itools/geistr -Isrc tools/geistr/geistr.c tools/geistr/decide.c tools/geistr/render.c tools/geistr/lineedit.c tools/geistr/service.c tools/geistr/json.c tools/geistr/config.c tools/geistr/speed.c tools/geistr/conversation.c tools/geistr/chat.c tools/geistr/http.c $(GEISTR_PULL) $(RUNTIME) \
		$(ENGINE_LINK) $(GEISTR_LIBS) $(LDFLAGS) $(LDLIBS) -o $@

geistr: $(BUILD)/geistr

# ---- make install: the built geistr into $(DESTDIR)$(PREFIX)/bin ------------------
# Not an engine goal: `sudo make install` copies, it never builds (as root) in
# the engine checkout. geistlib and (on macOS) libomp are linked in statically.
PREFIX ?= /usr/local
install:
	@test -x $(BUILD)/geistr || { echo "build it first: make geistr"; exit 1; }
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BUILD)/geistr $(DESTDIR)$(PREFIX)/bin/geistr

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/geistr

# The CLI against the reference model: run, chat, cancellation, catalog
# (--json schema), pull from a local server (a GEISTR_TESTING build).
test-geistr:
	@test -f "$(GEIST_TEST_MODEL)" || { echo "no reference model at $(GEIST_TEST_MODEL): make fetch-model"; exit 1; }
	$(MAKE) BUILD=$(BUILD)/geistr-test GEISTR_CFLAGS=-DGEISTR_TESTING geistr
	$(MAKE) BUILD=$(BUILD)/geistr-nonet PULL=0 geistr
	GEISTR_TEST_ONLY=$(ONLY) python3 -u tests/test_geistr.py $(BUILD)/geistr-test/geistr $(BUILD)/geistr-nonet/geistr "$(GEIST_TEST_MODEL)" $(PULL)

# An explicit target never skips: a missing model is an error, not a pass.
test-real: $(BUILD)/test_real
	@test -f "$(GEIST_TEST_MODEL)" || { echo "no reference model at $(GEIST_TEST_MODEL): make fetch-model"; exit 1; }
	GEIST_TEST_MODEL="$(GEIST_TEST_MODEL)" $(BUILD)/test_real

$(BUILD)/test_decision_real: tests/test_decision_real.c tests/native_cases.h tests/fixtures/decisions/*.h $(RUNTIME) $(ENGINE_LIB)
	$(CC) $(CFLAGS) $(ENGINE_CFLAGS) -Isrc -isystem $(GEISTLIB)/include $< $(BUILD)/common.o $(TEXT) $(ENGINE_LINK) $(LDFLAGS) $(LDLIBS) -o $@

test-decision-real: $(BUILD)/test_decision_real
	$(BUILD)/test_decision_real "$(DECISION_MODEL)" "$(DECISION_PROFILE)" "$(DECISION_PROCESSOR)"

$(BUILD)/test_decide_driver: tests/geistr_decide_driver.c tools/geistr/decide.c tools/geistr/decide.h $(LIB)
	$(CC) $(CFLAGS) tests/geistr_decide_driver.c tools/geistr/decide.c $(LIB) $(LDFLAGS) $(LDLIBS) -o $@

test-decision-cli: $(BUILD)/geistr
	python3 tests/test_decision_product_cli.py $(BUILD)/geistr "$(DECISION_MODEL)" "$(DECISION_PROFILE)" "$(DECISION_PROCESSOR)"

test-decision-python: wheel $(BUILD)/abi_sizes
	python3 -m venv $(BUILD)/decision-venv
	$(BUILD)/decision-venv/bin/pip install --no-index --force-reinstall $(BUILD)/wheel/geistr-*.whl
	$(BUILD)/decision-venv/bin/python tests/test_decision_python.py "$(DECISION_MODEL)" "$(DECISION_PROFILE)" "$(DECISION_PROCESSOR)" $(BUILD)/abi_sizes

# The reference model of the real tests: SmolLM2 360M from the catalog, verified.
fetch-model:
	sh scripts/fetch-model.sh models/catalog.json smollm2-360m $(dir $(GEIST_TEST_MODEL))

FORCE:

SAN := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
sanitize:
	$(MAKE) BUILD=$(BUILD)/san CFLAGS="-O1 -g $(SAN)" CXXFLAGS="-O1 -g $(SAN)" LDFLAGS="$(SAN)" test

clean:
	rm -rf $(BUILD)

.PHONY: install uninstall core all test sanitize clean runtime test-real chat-real fetch-model geistr test-geistr shared wheel test-python test-decision-real test-decision-cli test-decision-python FORCE
