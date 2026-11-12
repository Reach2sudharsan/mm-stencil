/*
 * test_buddy.c --  functional tests for the buddy allocator.
 *
 * Build/run:   make test_buddy        (from the project root)
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

#include "include/buddy_allocator.h"   /* free_list, chunk_t, buddy_obj_*  */
#include "sys/mem/mem.h"               /* dram, mem_size                   */

/* buddy_allocator.c uses NUM_LISTS = 5 internally; mirror it for inspection. */
#define NUM_LISTS      5
#define MAX_LIST_IDX   (NUM_LISTS - 1)          /* the MAX_CHUNK_SIZE class  */
#define TEST_MEM_SIZE  (262144L)                /* 256 KB: upper half = 2 x 64 KB */

/* --- no-op spinlock so buddy_obj_* don't deadlock (see header comment) --- */
void spinlock_init(spinlock_t *lock)   { (void)lock; }
void spinlock_lock(spinlock_t *lock)   { (void)lock; }
void spinlock_unlock(spinlock_t *lock) { (void)lock; }

/* ----------------------------- test plumbing ----------------------------- */
static int checks_run = 0;
static int checks_failed = 0;

static void check(int condition, const char *format, ...) {
    checks_run++;
    if (condition) {
        printf("    [ ok ] ");
    } else {
        checks_failed++;
        printf("    [FAIL] ");
    }

    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}

/* ------------------------- free-list inspection -------------------------- */

// Count the free chunks currently parked in size class `i`.
static int count_chunks(int i) {
    int n = 0;
    for (chunk_t *c = free_list->lists[i]; c != NULL; c = c->next) {
        n++;
    }
    return n;
}

// Total free bytes = sum over classes of (#chunks * class size in bytes).
// Class i holds 2^i pages, i.e. (PAGE_SIZE << i) bytes.
static long total_free_bytes(void) {
    long total = 0;
    for (int i = 0; i < NUM_LISTS; i++) {
        total += (long)count_chunks(i) * ((long)PAGE_SIZE << i);
    }
    return total;
}

static void dump_free_list(const char *label) {
    printf("    free_list after %s:", label);
    for (int i = 0; i < NUM_LISTS; i++) {
        printf("  [%d:%dp]=%d", i, 1 << i, count_chunks(i));
    }
    printf("   (%ld bytes free)\n", total_free_bytes());
}

// Do byte ranges [a, a+na) and [b, b+nb) overlap?
static int overlaps(void *a, long na, void *b, long nb) {
    char *a0 = a, *a1 = a0 + na;
    char *b0 = b, *b1 = b0 + nb;
    return (a0 < b1) && (b0 < a1);
}

// Fresh arena + allocator state before each test, so tests are independent.
static void setup(void) {
    if (dram != NULL) {
        free(dram);
    }
    mem_size = TEST_MEM_SIZE;
    /* aligned_alloc: size must be a multiple of alignment. 262144 = 4*65536. */
    dram = aligned_alloc(MAX_CHUNK_SIZE, (size_t)mem_size);
    if (dram == NULL) {
        fprintf(stderr, "aligned_alloc failed\n");
        exit(2);
    }
    slab_init();     /* free_ptr = dram  (slab metadata lives in lower half) */
    buddy_init();    /* seeds the upper half into the MAX_CHUNK_SIZE class   */
}

// The upper half is the buddy arena; it starts life as whole 64 KB chunks.
static long arena_bytes(void)      { return mem_size / 2; }
static int  initial_top_chunks(void) { return (int)(arena_bytes() / MAX_CHUNK_SIZE); }

// ============================= TEST 1 ===================================
// Process lifecycle: allocate a mock address space (code/data/stack) out of
// the arena, then "terminate the process" by freeing it all back. Verifies
// splitting on the way in and coalescing on the way out.
static void test_process_lifecycle(void) {
    printf("\nTest 1: process address-space alloc then reclaim\n");
    setup();

    long start_free = total_free_bytes();
    check(start_free == arena_bytes(),
          "arena starts fully free: %ld == %ld\n", start_free, arena_bytes());
    check(count_chunks(MAX_LIST_IDX) == initial_top_chunks(),
          "top class seeded with %d chunks\n", initial_top_chunks());

    /* Carve an address space out of a single 64 KB chunk (forces splits). */
    void *code  = buddy_obj_alloc(4096);    /* 1 page  -> class 0 */
    void *data  = buddy_obj_alloc(8192);    /* 2 pages -> class 1 */
    void *stack = buddy_obj_alloc(16384);   /* 4 pages -> class 2 */

    check(code && data && stack, "all three regions allocated (non-NULL)\n");
    check(code != data && data != stack && code != stack,
          "regions have distinct addresses\n");
    check(!overlaps(code, 4096, data, 8192) &&
          !overlaps(data, 8192, stack, 16384) &&
          !overlaps(code, 4096, stack, 16384),
          "regions do not overlap\n");
    check(total_free_bytes() == start_free - (4096 + 8192 + 16384),
          "free bytes dropped by exactly the amount handed out\n");
    dump_free_list("3 allocations");

    /* Process terminates: reclaim everything. */
    buddy_obj_free(16384, stack);
    buddy_obj_free(8192, data);
    buddy_obj_free(4096, code);

    check(total_free_bytes() == start_free,
          "all bytes reclaimed: %ld == %ld\n", total_free_bytes(), start_free);
    check(count_chunks(MAX_LIST_IDX) == initial_top_chunks(),
          "freed blocks coalesced back into %d top-class chunks\n",
          initial_top_chunks());
    dump_free_list("reclaim");
}

// ============================= TEST 2 ===================================
// Exhaust all memory, confirm the next allocation fails, then free one block
// and confirm allocation can proceed again.
static void test_exhaustion_then_free(void) {
    printf("\nTest 2: full exhaustion, then a free re-enables allocation\n");
    setup();

    int n = initial_top_chunks();           /* how many 64 KB blocks exist */
    void *blocks[16];
    check(n <= 16, "test config has a manageable number of top chunks (%d)\n", n);

    for (int i = 0; i < n; i++) {
        blocks[i] = buddy_obj_alloc(MAX_CHUNK_SIZE);
        check(blocks[i] != NULL, "max-size allocation #%d succeeds\n", i + 1);
    }

    check(total_free_bytes() == 0, "every byte is now allocated\n");
    for (int i = 0; i < NUM_LISTS; i++) {
        check(count_chunks(i) == 0, "class %d is empty at full exhaustion\n", i);
    }

    void *fail = buddy_obj_alloc(MAX_CHUNK_SIZE);
    check(fail == NULL, "allocation fails when memory is exhausted\n");
    dump_free_list("exhaustion");

    /* Return one block; allocation should work again. */
    buddy_obj_free(MAX_CHUNK_SIZE, blocks[0]);
    check(total_free_bytes() == MAX_CHUNK_SIZE, "one block worth of bytes freed\n");

    void *again = buddy_obj_alloc(MAX_CHUNK_SIZE);
    check(again != NULL, "allocation succeeds after a free\n");
    check(again == blocks[0], "reclaimed block is handed back out\n");
    dump_free_list("free + realloc");
}

// ============================= TEST 3 ===================================
// Request a size larger than the largest supported chunk. Must fail cleanly
// and must not disturb the free list.
static void test_oversized_request(void) {
    printf("\nTest 3: allocation larger than the largest chunk class\n");
    setup();

    long before = total_free_bytes();

    void *too_big_1 = buddy_obj_alloc(MAX_CHUNK_SIZE + 1);   /* just over */
    check(too_big_1 == NULL, "buddy_obj_alloc(MAX_CHUNK_SIZE + 1) returns NULL\n");

    void *too_big_2 = buddy_obj_alloc(MAX_CHUNK_SIZE * 2);   /* far over */
    check(too_big_2 == NULL, "buddy_obj_alloc(2 * MAX_CHUNK_SIZE) returns NULL\n");

    /* page_alloc_n guards the same case in page units. */
    void *too_big_3 = page_alloc_n((MAX_CHUNK_SIZE / PAGE_SIZE) + 1);
    check(too_big_3 == NULL, "page_alloc_n beyond MAX_CHUNK_SIZE returns NULL\n");

    check(total_free_bytes() == before,
          "free list is untouched by failed oversized requests\n");
    check(count_chunks(MAX_LIST_IDX) == initial_top_chunks(),
          "top class still holds all its chunks\n");
    dump_free_list("oversized attempts");
}

// ============================= TEST 4 ===================================
// Alloc / free / alloc determinism: freeing a block and requesting the same
// size again should hand the block straight back (proves the freed node is
// returned to the free list, not leaked).
static void test_alloc_free_alloc_determinism(void) {
    printf("\nTest 4: alloc/free/alloc returns the same address (no leak)\n");
    setup();

    /* Try it across several size classes. */
    long sizes[] = { 4096, 8192, 16384, 32768, 65536 };
    for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        long sz = sizes[s];

        void *first = buddy_obj_alloc(sz);
        check(first != NULL, "size %ld: first allocation succeeds\n", sz);

        buddy_obj_free(sz, first);

        void *second = buddy_obj_alloc(sz);
        check(second == first,
              "size %ld: realloc returns the just-freed address\n", sz);

        /* Leave the arena clean for the next size. */
        buddy_obj_free(sz, second);
        check(total_free_bytes() == arena_bytes(),
              "size %ld: arena fully restored after the cycle\n", sz);
    }
    dump_free_list("determinism cycles");
}

int main(void) {
    printf("=== buddy allocator functional tests ===\n");
    printf("(arena = %ld bytes, upper half handed out as %d x %d-byte chunks)\n",
           TEST_MEM_SIZE, (int)((TEST_MEM_SIZE / 2) / MAX_CHUNK_SIZE), MAX_CHUNK_SIZE);

    test_process_lifecycle();
    test_exhaustion_then_free();
    test_oversized_request();
    test_alloc_free_alloc_determinism();

    printf("\n=== summary: %d checks, %d failed ===\n",
           checks_run, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
