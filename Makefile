# geist-runtime — embeddable model runner on geistlib.
#
#   make            the stub library, the tests and the example
#   make test       run the conformance tests (C and C++) and the example
#   make sanitize   the same with AddressSanitizer and UBSan
#
# libgeistr.a collects the real runtime as it moves from geist-serve (#2–#6):
# so far the chat templates (src/template.c). libgeistr-stub.a is a test
# double of include/geistr.h without geistlib, for the API conformance tests.

CC       ?= cc
CXX      ?= c++
BUILD    ?= build
WARN     := -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wno-sign-conversion
CFLAGS   ?= -O2 -g
CXXFLAGS ?= -O2 -g
override CFLAGS   += -std=c23 $(WARN) -Iinclude -D_POSIX_C_SOURCE=200809L
override CXXFLAGS += -std=c++20 $(WARN) -Iinclude
LDLIBS   += -lpthread

LIB     := $(BUILD)/libgeistr-stub.a
RUNTIME := $(BUILD)/libgeistr.a

all: $(LIB) $(RUNTIME) $(BUILD)/test_api $(BUILD)/test_cxx $(BUILD)/test_template $(BUILD)/chat

$(BUILD)/%.o: src/%.c src/*.h include/geistr.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB): $(BUILD)/stub.o $(BUILD)/template.o
	ar rcs $@ $^

$(RUNTIME): $(BUILD)/template.o
	ar rcs $@ $^

$(BUILD)/test_template: tests/test_template.c $(RUNTIME)
	$(CC) $(CFLAGS) -Isrc $< $(RUNTIME) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/test_api: tests/test_api.c $(LIB)
	$(CC) $(CFLAGS) $< $(LIB) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/test_cxx: tests/test_cxx.cpp $(LIB) include/geistr.h
	$(CXX) $(CXXFLAGS) $< $(LIB) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/chat: examples/chat.c $(LIB)
	$(CC) $(CFLAGS) $< $(LIB) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD):
	mkdir -p $@

test: all
	$(BUILD)/test_api
	$(BUILD)/test_cxx
	$(BUILD)/test_template
	@out=$$(printf 'Hallo Welt\nnoch einmal\n' | $(BUILD)/chat stub:echo) && \
	  echo "$$out" | grep -q 'Echo: noch einmal' && echo "example chat: two turns passed" || \
	  { echo "example chat failed: $$out"; exit 1; }

# Byte parity with geist-serve's renderer (#2): make parity SERVE_DIR=../geist-serve
parity: $(RUNTIME)
	@test -f "$(SERVE_DIR)/src/template.c" || { echo "set SERVE_DIR to a geist-serve checkout"; exit 1; }
	$(CC) $(CFLAGS) -Wno-conversion -I$(SERVE_DIR)/src -Itools/parity tools/parity/serve.c $(SERVE_DIR)/src/template.c -o $(BUILD)/parity_serve
	$(CC) $(CFLAGS) -Isrc -Itools/parity tools/parity/geistr.c $(RUNTIME) -o $(BUILD)/parity_geistr
	$(BUILD)/parity_serve > $(BUILD)/parity_serve.txt
	$(BUILD)/parity_geistr > $(BUILD)/parity_geistr.txt
	cmp $(BUILD)/parity_serve.txt $(BUILD)/parity_geistr.txt
	@echo "parity with geist-serve: $$(grep -c '^== ' $(BUILD)/parity_serve.txt) renders identical"

SAN := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
sanitize:
	$(MAKE) BUILD=$(BUILD)/san CFLAGS="-O1 -g $(SAN)" CXXFLAGS="-O1 -g $(SAN)" LDFLAGS="$(SAN)" test

clean:
	rm -rf $(BUILD)

.PHONY: all test sanitize parity clean
