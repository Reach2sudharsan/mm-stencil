#include "buddy_allocator.h"

#include "sys/mem/mem.h"

#include "util/ktypes.h"
#include "util/string.h"

/* mem_size is the global from sys/mem/mem.h; we hand out only its upper half. */
#define NUM_LISTS 5
#define MAX_LIST_IDX (NUM_LISTS - 1)   /* index of the MAX_CHUNK_SIZE class */

void buddy_init() {
    spinlock_init(&mm_buddy_mutex);
    slab_allocator_init(&buddy_slab_allocator, sizeof(free_list_t));
    slab_allocator_init(&chunk_slab_allocator, sizeof(chunk_t));
    free_list = slab_obj_alloc(buddy_slab_allocator);

    slab_allocator_t lists_allocator;
    slab_allocator_init(&lists_allocator, NUM_LISTS * sizeof(chunk_t *));
    free_list->lists = slab_obj_alloc(lists_allocator);

    for (int i = 0; i < NUM_LISTS; ++i) {
        free_list->lists[i] = NULL;
    }

    // initialize the largest chunk with 2 chunks and leave the rest

    void *base = (char *)dram + (mem_size / 2);
    long n_chunks = (mem_size / 2) / MAX_CHUNK_SIZE;

    /* Seed the largest size class with one chunk per MAX_CHUNK_SIZE region.
     * Splitting into smaller classes happens on demand in buddy_obj_alloc. */
    for (long i = 0; i < n_chunks; ++i) {
        chunk_t *c = slab_obj_alloc(chunk_slab_allocator);
        c->addr = (char *)base + i * MAX_CHUNK_SIZE;
        c->next = free_list->lists[MAX_LIST_IDX];   /* push-front */
        free_list->lists[MAX_LIST_IDX] = c;
    }
}

/*
 * TODO: implement me!
 *
 * Hints:
 *   - given a size, how do we know where to allocate from?
 *   - if we don't have any available chunks, how do we find another one?
 */
int log_2(int n) {
    int flag = 0;
    int res = 0;
    
    while (n > 1) {
        if (n & 0x01){     // a low bit gets dropped -> remainder -> round up
            flag = 1;
        }
        n >>= 1;
        res++;
    }
    return res + flag;
}
 
void *buddy_obj_alloc(long size) {
    //lock 
    spinlock_lock(&mm_buddy_mutex);
    int first_ind = log_2((size + PAGE_SIZE - 1) / PAGE_SIZE);
    int cur_ind=first_ind;
    int added = 0;
    void *result = NULL;
    while (!added && cur_ind <= MAX_LIST_IDX) {
        if (free_list->lists[cur_ind] != NULL){
            chunk_t *c = free_list->lists[cur_ind];
            free_list->lists[cur_ind] = c->next;

            while (cur_ind > first_ind) {
                cur_ind--;
                chunk_t *new_chunk = slab_obj_alloc(chunk_slab_allocator);
                new_chunk->addr = (char *)c->addr + (1 << cur_ind) * PAGE_SIZE;
                new_chunk->next = free_list->lists[cur_ind];
                free_list->lists[cur_ind] = new_chunk;
            }
            
            result = c->addr;
            slab_obj_free(chunk_slab_allocator, c);   // node no longer tracks a free block
            added = 1;
        }
        cur_ind += 1;
    }
 
    //unlock 
    spinlock_unlock(&mm_buddy_mutex);
    return result;
}

/*
 *
 * Hint: this function should handle the inverse cases of the "alloc" function!
 *       if there are cases that need to be handled in that function, what
 *       would the inverse cases look like?
 */
void buddy_obj_free(long size, void *obj) {
    // lock 
    spinlock_lock(&mm_buddy_mutex);
    // find the index of the list to add the chunk to
    int ind = log_2((size + PAGE_SIZE - 1) / PAGE_SIZE);
    chunk_t *c = slab_obj_alloc(chunk_slab_allocator);
    c->addr = obj;

    // merge upward while this block's buddy is free
    while (ind < MAX_LIST_IDX) {
        // buddy = this block's address with the current chunk-size bit flipped
        unsigned long chunk_bytes = (unsigned long)PAGE_SIZE << ind;
        void *buddy_addr = (void *)((unsigned long)c->addr ^ chunk_bytes);
        chunk_t *prev = NULL;
        chunk_t *curr = free_list->lists[ind];
        while (curr != NULL && curr->addr != buddy_addr) {
            prev = curr;
            curr = curr->next;
        }

        if (curr == NULL) {
            break;   // buddy not free -> stop merging
        }

        // unlink the buddy from this list
        if (prev == NULL) {
            free_list->lists[ind] = curr->next;
        } else {
            prev->next = curr->next;
        }

        // merged block starts at the lower address
        if (buddy_addr < c->addr) {
            c->addr = buddy_addr;
        }
        slab_obj_free(chunk_slab_allocator, curr);   // free the buddy's node

        ind++;
    }

    // insert the (possibly merged) chunk once, at the head of its list
    c->next = free_list->lists[ind];
    free_list->lists[ind] = c;

    //unlock
    spinlock_unlock(&mm_buddy_mutex);
}

/*
 * TODO: implement me!
 *
 * Hint: this function behaves similar to "obj_alloc" but what happens if
 *       npages is larger than MAX_CHUNK_SIZE?
 */
void *page_alloc_n(int npages) {
    if (npages <= 0) {
        return NULL;
    }
    if (npages * PAGE_SIZE > MAX_CHUNK_SIZE) {
        return NULL;
    }
    return buddy_obj_alloc(npages * PAGE_SIZE);
}

/*
 * TODO: implement me!
 *
 * Hint: this function should handle the inverse case of the "alloc" function!
 */
void page_free_n(void *ptr, int npages) {
    if (ptr == NULL || npages <= 0) {
        return;
    }
    if (npages * PAGE_SIZE > MAX_CHUNK_SIZE) {
        return;   // page_alloc_n never hands out more than MAX_CHUNK_SIZE
    }
    buddy_obj_free(npages * PAGE_SIZE, ptr);
}
