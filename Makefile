# Sentinel build system (GNU make). Pure C/C++ toolchain: g++/gcc + make.
#
#   make                 optimized build      -> build/release/{sentineld,sentinel-cli,sentinel_read}
#   make test            build + run tests
#   make check           tests under AddressSanitizer+UBSan, then ThreadSanitizer
#   make MODE=debug      -O0 -g3 build        (MODE: release | debug | asan | tsan)
#   make driver          build the kernel module (needs kernel headers)
#   make install         install to $(PREFIX)   (DESTDIR supported)
#   make lint            cppcheck static analysis (if installed)
#   make help

CXX      ?= g++
CC       ?= gcc
AR       ?= ar
MODE     ?= release
PREFIX   ?= /usr/local
BUILD    := build
OUT      := $(BUILD)/$(MODE)

WARN     := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wformat=2
CXXWARN  := -Wnon-virtual-dtor -Woverloaded-virtual
INCLUDES := -Isrc -Iinclude
DEPFLAGS := -MMD -MP

ifeq ($(MODE),release)
  OPT      := -O2 -DNDEBUG -D_FORTIFY_SOURCE=2 -fstack-protector-strong -fPIE
  LDMODE   := -pie -Wl,-z,relro,-z,now
else ifeq ($(MODE),debug)
  OPT      := -O0 -g3 -fno-omit-frame-pointer
  LDMODE   :=
else ifeq ($(MODE),asan)
  OPT      := -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined
  LDMODE   := -fsanitize=address,undefined
else ifeq ($(MODE),tsan)
  OPT      := -O1 -g -fno-omit-frame-pointer -fsanitize=thread
  LDMODE   := -fsanitize=thread
else
  $(error Unknown MODE '$(MODE)' (use release, debug, asan or tsan))
endif

CXXFLAGS += -std=c++17 $(WARN) $(CXXWARN) $(OPT) $(INCLUDES) $(DEPFLAGS) -pthread
CFLAGS   += -std=gnu11 $(WARN) $(OPT) $(INCLUDES) $(DEPFLAGS)
LDFLAGS  += $(LDMODE) -pthread
LDLIBS   += -lrt

LIB_SRC  := log sample source protocol server shm daemonize
LIB_OBJ  := $(LIB_SRC:%=$(OUT)/%.o)
LIB      := $(OUT)/libsentinel.a

BINS     := $(OUT)/sentineld $(OUT)/sentinel-cli $(OUT)/sentinel_read
TEST_BIN := $(OUT)/sentinel_tests

.PHONY: all test check driver driver-clean install uninstall install-service lint clean distclean help
all: $(BINS)

$(OUT)/%.o: src/%.cpp | $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/%.o: tests/%.cpp | $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/sentinel_read.o: tools/sentinel_read.c | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB): $(LIB_OBJ)
	$(AR) rcs $@ $^

$(OUT)/sentineld: $(OUT)/sentineld.o $(LIB)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/sentinel-cli: $(OUT)/sentinel_cli.o $(LIB)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/sentinel_read: $(OUT)/sentinel_read.o
	$(CC) $(LDFLAGS) -o $@ $^

$(TEST_BIN): $(OUT)/test_main.o $(LIB)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OUT):
	mkdir -p $@

test: $(TEST_BIN)
	$(TEST_BIN)

check:
	$(MAKE) MODE=asan test
	$(MAKE) MODE=tsan test

driver:
	$(MAKE) -C driver

driver-clean:
	$(MAKE) -C driver clean

install: $(OUT)/sentineld $(OUT)/sentinel-cli $(OUT)/sentinel_read
	install -D -m 0755 $(OUT)/sentineld     $(DESTDIR)$(PREFIX)/sbin/sentineld
	install -D -m 0755 $(OUT)/sentinel-cli  $(DESTDIR)$(PREFIX)/bin/sentinel-cli
	install -D -m 0755 $(OUT)/sentinel_read $(DESTDIR)$(PREFIX)/bin/sentinel_read

install-service:
	install -D -m 0644 packaging/sentineld.service $(DESTDIR)/etc/systemd/system/sentineld.service

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/sbin/sentineld $(DESTDIR)$(PREFIX)/bin/sentinel-cli $(DESTDIR)$(PREFIX)/bin/sentinel_read

lint:
	@command -v cppcheck >/dev/null 2>&1 || { echo "cppcheck not installed (apt install cppcheck)"; exit 1; }
	cppcheck --std=c++17 --enable=warning,performance,portability --inline-suppr --error-exitcode=1 \
	  --suppress=missingIncludeSystem -Isrc -Iinclude src tests tools

clean:
	rm -rf $(BUILD)

distclean: clean driver-clean

help:
	@sed -n '2,11p' Makefile

-include $(wildcard $(OUT)/*.d)
