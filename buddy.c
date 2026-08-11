#include "buddy.h"
#include <stdlib.h>
#include <string.h>

#define PAGE_SIZE    4096
#define MAX_RANK     16

/* ---- internal state ----
 * page_rank[i] is the rank of the block containing page i (set for ALL pages).
 * page_allocated[i] is 1 if page i is allocated, 0 if free.
 */
static char *page_allocated = NULL;
static int  *page_rank      = NULL;
static void *pool           = NULL;
static int   total_pages    = 0;

/* Doubly-linked free lists: each free block stores [next, prev] at offset 0 */
static void *free_lists[MAX_RANK + 1];
static int   free_count[MAX_RANK + 1];

/* ---- helpers ---- */

static inline int block_pages(int rank) {
    return 1 << (rank - 1);
}

static inline int buddy_offset(int offset, int rank) {
    return offset ^ (1 << (rank - 1));
}

/* Store free list pointers at the END of the block to avoid conflicts
 * with any writes to the beginning of allocated blocks. */
static inline void **get_next_ptr(void *addr) {
    return (void **)((char *)addr + PAGE_SIZE - 16);
}

static inline void **get_prev_ptr(void *addr) {
    return (void **)((char *)addr + PAGE_SIZE - 8);
}

static void remove_from_free_list(void *addr, int rank) {
    void *next = *get_next_ptr(addr);
    void *prev = *get_prev_ptr(addr);
    if (prev) {
        *get_next_ptr(prev) = next;
    } else {
        free_lists[rank] = next;
    }
    if (next) {
        *get_prev_ptr(next) = prev;
    }
}

static void add_to_free_list(void *addr, int rank) {
    *get_next_ptr(addr) = free_lists[rank];
    *get_prev_ptr(addr) = NULL;
    if (free_lists[rank]) {
        *get_prev_ptr(free_lists[rank]) = addr;
    }
    free_lists[rank] = addr;
}

/* ---- public API ---- */

int init_page(void *p, int pgcount) {
    if (page_rank)      { free(page_rank);      page_rank      = NULL; }
    if (page_allocated) { free(page_allocated); page_allocated = NULL; }

    pool        = p;
    total_pages = pgcount;

    int i;
    for (i = 1; i <= MAX_RANK; i++) {
        free_lists[i] = NULL;
        free_count[i] = 0;
    }

    if (pgcount <= 0) return OK;

    page_rank      = (int  *)calloc((size_t)pgcount, sizeof(int));
    page_allocated = (char *)calloc((size_t)pgcount, sizeof(char));
    if (!page_rank || !page_allocated) {
        if (page_rank)      { free(page_rank);      page_rank      = NULL; }
        if (page_allocated) { free(page_allocated); page_allocated = NULL; }
        return -ENOSPC;
    }

    /* Split into maximal power-of-2 blocks */
    int remaining = pgcount;
    int offset    = 0;

    for (int rank = MAX_RANK; rank >= 1 && remaining > 0; rank--) {
        int bp = block_pages(rank);
        while (remaining >= bp) {
            int j;
            for (j = offset; j < offset + bp; j++) {
                page_rank[j] = rank;
            }
            /* page_allocated already 0 from calloc */

            void *addr = (char *)pool + (unsigned long)offset * PAGE_SIZE;
            add_to_free_list(addr, rank);
            free_count[rank]++;

            offset    += bp;
            remaining -= bp;
        }
    }

    return OK;
}

void *alloc_pages(int rank) {
    if (rank < 1 || rank > MAX_RANK) return ERR_PTR(-EINVAL);

    int r;
    for (r = rank; r <= MAX_RANK; r++) {
        if (free_lists[r] != NULL) break;
    }

    if (r > MAX_RANK) return ERR_PTR(-ENOSPC);

    void *block = free_lists[r];
    remove_from_free_list(block, r);
    free_count[r]--;

    int offset = (int)(((char *)block - (char *)pool) / PAGE_SIZE);

    /* split until we reach the requested rank */
    while (r > rank) {
        r--;
        int bp = block_pages(r);

        int    buddy_off = offset + bp;
        void  *buddy     = (char *)pool + (unsigned long)buddy_off * PAGE_SIZE;

        int j;
        for (j = buddy_off; j < buddy_off + bp; j++) {
            page_rank[j]      = r;
            page_allocated[j] = 0;
        }
        add_to_free_list(buddy, r);
        free_count[r]++;

        /* first half continues to be split */
    }

    /* mark the allocated block */
    {
        int bp = block_pages(rank);
        int j;
        for (j = offset; j < offset + bp; j++) {
            page_allocated[j] = 1;
            page_rank[j]      = rank;
        }
    }

    return block;
}

int return_pages(void *p) {
    if (p == NULL || total_pages == 0 || page_rank == NULL) return -EINVAL;
    if ((char *)p < (char *)pool ||
        (char *)p >= (char *)pool + (unsigned long)total_pages * PAGE_SIZE)
        return -EINVAL;

    unsigned long diff = (char *)p - (char *)pool;
    if (diff % PAGE_SIZE != 0) return -EINVAL;

    int offset = (int)(diff / PAGE_SIZE);

    /* must be the start of an allocated block */
    if (!page_allocated[offset]) return -EINVAL;

    int rank = page_rank[offset];
    if (rank < 1 || rank > MAX_RANK) return -EINVAL;

    int bp = block_pages(rank);

    /* mark as free */
    {
        int j;
        for (j = offset; j < offset + bp; j++) {
            page_allocated[j] = 0;
        }
    }

    /* try to merge with buddy */
    while (rank < MAX_RANK) {
        int buddy_off = buddy_offset(offset, rank);
        bp = block_pages(rank);

        if (buddy_off < 0 || buddy_off + bp > total_pages) break;
        if (page_allocated[buddy_off] || page_rank[buddy_off] != rank) break;

        void *buddy_addr = (char *)pool + (unsigned long)buddy_off * PAGE_SIZE;
        remove_from_free_list(buddy_addr, rank);
        free_count[rank]--;

        /* merged block starts at the aligned offset */
        offset = offset & ~(1 << (rank - 1));
        rank++;

        /* update metadata for the merged block */
        bp = block_pages(rank);
        int j;
        for (j = offset; j < offset + bp; j++) {
            page_rank[j] = rank;
        }
    }

    add_to_free_list((char *)pool + (unsigned long)offset * PAGE_SIZE, rank);
    free_count[rank]++;

    return OK;
}

int query_ranks(void *p) {
    if (p == NULL || total_pages == 0 || page_rank == NULL) return -EINVAL;
    if ((char *)p < (char *)pool ||
        (char *)p >= (char *)pool + (unsigned long)total_pages * PAGE_SIZE)
        return -EINVAL;

    unsigned long diff = (char *)p - (char *)pool;
    if (diff % PAGE_SIZE != 0) return -EINVAL;

    int offset = (int)(diff / PAGE_SIZE);
    return page_rank[offset];
}

int query_page_counts(int rank) {
    if (rank < 1 || rank > MAX_RANK) return -EINVAL;
    return free_count[rank];
}