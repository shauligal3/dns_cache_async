# libdnscache - asynchronous DNS cache shared library
#
#   make              build build/libdnscache.so (+ static lib, example)
#   make test         build and run the test suite (needs working DNS)
#   make asan         run the tests under AddressSanitizer + UBSan
#   make memcheck     run the tests under valgrind memcheck
#   make install      install to $(PREFIX) (default /usr/local)

NAME      := dnscache
VERSION   := 1.0.0
SOVERSION := 1

CC        ?= cc
PREFIX    ?= /usr/local
LIBDIR    ?= $(PREFIX)/lib
INCDIR    ?= $(PREFIX)/include
BUILD     ?= build

CFLAGS    ?= -O2 -g
override CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wshadow \
             -fPIC -fvisibility=hidden -Iinclude
LDLIBS    := -lanl -lpthread

SRCS      := src/dns_cache.c src/timer_heap.c src/job_queue.c
OBJS      := $(SRCS:src/%.c=$(BUILD)/obj/%.o)

SONAME    := lib$(NAME).so.$(SOVERSION)
SHLIB     := $(BUILD)/lib$(NAME).so.$(VERSION)
STLIB     := $(BUILD)/lib$(NAME).a

.PHONY: all lib test asan memcheck install uninstall clean

all: lib $(BUILD)/dnsc_resolve

lib: $(SHLIB) $(STLIB)

$(BUILD)/obj/%.o: src/%.c include/dns_cache.h src/*.h | $(BUILD)/obj
	$(CC) $(CFLAGS) -c $< -o $@

$(SHLIB): $(OBJS)
	$(CC) -shared -Wl,-soname,$(SONAME) -Wl,--no-undefined -o $@ $^ $(LDFLAGS) $(LDLIBS)
	ln -sf lib$(NAME).so.$(VERSION) $(BUILD)/$(SONAME)
	ln -sf $(SONAME) $(BUILD)/lib$(NAME).so

$(STLIB): $(OBJS)
	$(AR) rcs $@ $^

$(BUILD)/dnsc_resolve: examples/dnsc_resolve.c $(SHLIB)
	$(CC) $(CFLAGS) -o $@ $< -L$(BUILD) -l$(NAME) -Wl,-rpath,'$$ORIGIN' $(LDLIBS)

$(BUILD)/test_dns_cache: tests/test_dns_cache.c $(SHLIB)
	$(CC) $(CFLAGS) -o $@ $< -L$(BUILD) -l$(NAME) -Wl,-rpath,'$$ORIGIN' $(LDLIBS)

test: $(BUILD)/test_dns_cache
	$(BUILD)/test_dns_cache

asan:
	$(MAKE) BUILD=build-asan CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
	        LDFLAGS="-fsanitize=address,undefined" test

# ThreadSanitizer is not usable here: since glibc 2.34 getaddrinfo_a() starts
# its worker threads through an internal pthread_create() that TSan does not
# intercept, and TSan crashes inside them.
memcheck: $(BUILD)/test_dns_cache
	valgrind -q --leak-check=full --errors-for-leak-kinds=definite \
	         --suppressions=valgrind.supp --error-exitcode=9 $(BUILD)/test_dns_cache

$(BUILD)/obj:
	mkdir -p $@

install: lib
	install -d $(DESTDIR)$(LIBDIR) $(DESTDIR)$(INCDIR)
	install -m 0755 $(SHLIB) $(DESTDIR)$(LIBDIR)/
	ln -sf lib$(NAME).so.$(VERSION) $(DESTDIR)$(LIBDIR)/$(SONAME)
	ln -sf $(SONAME) $(DESTDIR)$(LIBDIR)/lib$(NAME).so
	install -m 0644 $(STLIB) $(DESTDIR)$(LIBDIR)/
	install -m 0644 include/dns_cache.h $(DESTDIR)$(INCDIR)/

uninstall:
	rm -f $(DESTDIR)$(LIBDIR)/lib$(NAME).so* $(DESTDIR)$(LIBDIR)/lib$(NAME).a \
	      $(DESTDIR)$(INCDIR)/dns_cache.h

clean:
	rm -rf build build-asan
