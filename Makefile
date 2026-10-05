# geist-runtime — embeddable model runner on geistlib.
#
#   make            the stub library, the tests and the example
#   make test       run the conformance tests (C and C++) and the example
#   make sanitize   the same with AddressSanitizer and UBSan
#
# Until the extraction (#2–#6) there is only src/stub.c: a test double of
# include/geistr.h without geistlib, so the API can be reviewed and tested.

CC       ?= cc
CXX      ?= c++
BUILD    ?= build
WARN     := -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wno-sign-conversion
CFLAGS   ?= -O2 -g
CXXFLAGS ?= -O2 -g
override CFLAGS   += -std=c23 $(WARN) -Iinclude -D_POSIX_C_SOURCE=200809L
override CXXFLAGS += -std=c++20 $(WARN) -Iinclude
LDLIBS   += -lpthread

LIB := $(BUILD)/libgeistr-stub.a

all: $(LIB) $(BUILD)/test_api $(BUILD)/test_cxx $(BUILD)/chat

$(BUILD)/%.o: src/%.c include/geistr.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB): $(BUILD)/stub.o
	ar rcs $@ $^

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
	@out=$$(printf 'Hallo Welt\nnoch einmal\n' | $(BUILD)/chat stub:echo) && \
	  echo "$$out" | grep -q 'Echo: noch einmal' && echo "example chat: two turns passed" || \
	  { echo "example chat failed: $$out"; exit 1; }

SAN := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
sanitize:
	$(MAKE) BUILD=$(BUILD)/san CFLAGS="-O1 -g $(SAN)" CXXFLAGS="-O1 -g $(SAN)" LDFLAGS="$(SAN)" test

clean:
	rm -rf $(BUILD)

.PHONY: all test sanitize clean
