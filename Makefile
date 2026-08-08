CC ?= cc
# -Wextra/-Wpedantic dropped deliberately: this port leans on the same GNU
# extensions real PG's own build does (statement expressions, __builtin_*),
# and PG's vtable-shaped functions have the same "unused parameter" shape
# pedantic/-Wextra would flag in the original source too.
CFLAGS ?= -std=c11 -Wall -g -O2

PREFIX ?= /usr/local
LIBDIR = $(DESTDIR)$(PREFIX)/lib
INCLUDEDIR = $(DESTDIR)$(PREFIX)/include/mctx

OBJS = mcxt.o aset.o alignedalloc.o

# Headers needed to build the library itself.
HEADERS = pg_compat.h mctx_internal.h memutils_memorychunk.h memdebug.h memutils.h memnodes.h palloc.h mctx.h

# The subset of HEADERS that a consumer of the built library actually needs;
# this is what `make install` copies out. mctx_internal.h/
# memutils_memorychunk.h/memdebug.h are implementation-only -- no public
# header includes them, so they aren't part of the installed interface.
PUBLIC_HEADERS = pg_compat.h memnodes.h memutils.h palloc.h mctx.h

TEST_SRCS = test/test_mcxt.c test/test_palloc.c test/test_aligned.c test/test_abort_paths.c test/test_main.c
TEST_OBJS = $(TEST_SRCS:.c=.o)

all: libmctx.a test_runner

libmctx.a: $(OBJS)
	ar rcs $@ $^

mcxt.o: mcxt.c $(HEADERS)
	$(CC) $(CFLAGS) -c mcxt.c -o $@

aset.o: aset.c $(HEADERS)
	$(CC) $(CFLAGS) -c aset.c -o $@

alignedalloc.o: alignedalloc.c $(HEADERS)
	$(CC) $(CFLAGS) -c alignedalloc.c -o $@

test/%.o: test/%.c test/test.h $(HEADERS)
	$(CC) $(CFLAGS) -I. -c $< -o $@

test_runner: $(TEST_OBJS) libmctx.a
	$(CC) $(CFLAGS) -o $@ $(TEST_OBJS) libmctx.a

check: test_runner
	./test_runner

install: libmctx.a
	install -d $(LIBDIR) $(INCLUDEDIR)
	install -m 644 libmctx.a $(LIBDIR)/
	install -m 644 $(PUBLIC_HEADERS) $(INCLUDEDIR)/

uninstall:
	rm -f $(LIBDIR)/libmctx.a
	rm -rf $(INCLUDEDIR)

clean:
	rm -f *.o *.a test_runner test/*.o core-*

.PHONY: all check install uninstall clean
