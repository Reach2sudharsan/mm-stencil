CC = gcc
FLAGS = -Og -g -Wall
LIBFLAGS = -c
INCLUDE = -I. -Iinclude

UTILS = spinlock list string
ALLOCS = slaballocator
ALL = $(UTILS) $(ALLOCS) $(LIBS)

ALLOCOBJS = bin/slab_allocator.o
ALLOBJS = $(UTILOBJS) $(ALLOCOBJS) $(LIBOBJS)

slaballocator:
	$(CC) $(FLAGS) $(LIBFLAGS) $(INCLUDE) src/slab_allocator.c -o bin/slab_allocator.o

buddyallocator:
	$(CC) $(FLAGS) $(LIBFLAGS) $(INCLUDE) src/buddy_allocator.c -o bin/buddy_allocator.o

mm: $(UTILS) $(ALLOCS)
	$(CC) $(FLAGS) $(LIBFLAGS) $(INCLUDE) $(ALLOCOBJS) src/mm.c -o bin/mm.o

# Functional tests for the buddy allocator.
#   -fcommon         : tolerate the tentative global defs in the headers
#   -Dpage_alloc_n=..: rename the slab's duplicate page_* funcs so they don't
#                      collide with the buddy's (compiled in a separate step).
# The harness supplies its own no-op spinlocks and arena, so util/spinlock.c
# and sys/mem/mem.c are intentionally NOT linked (see test/test_buddy.c).
TESTFLAGS = -Og -g -Wall -fcommon $(INCLUDE)

test_buddy:
	$(CC) $(TESTFLAGS) -Dpage_alloc_n=slab_page_alloc_n -Dpage_free_n=slab_page_free_n $(LIBFLAGS) src/slab_allocator.c -o bin/slab_renamed.o
	$(CC) $(TESTFLAGS) test/test_buddy.c src/buddy_allocator.c bin/slab_renamed.o -o bin/test_buddy
	./bin/test_buddy

clean:
	rm -rf bin/*

# Separate build: test the REAL spinlock instead of the test harness stubs.
test_buddy_real_spinlock:
	$(CC) $(TESTFLAGS) -Dpage_alloc_n=slab_page_alloc_n -Dpage_free_n=slab_page_free_n $(LIBFLAGS) src/slab_allocator.c -o bin/slab_renamed.o
	$(CC) $(TESTFLAGS) -DTEST_REAL_SPINLOCK test/test_buddy.c src/buddy_allocator.c util/spinlock.c bin/slab_renamed.o -o bin/test_buddy_real_spinlock
	./bin/test_buddy_real_spinlock
