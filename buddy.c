#include "buddy.h"
#include <stdlib.h>
#define NULL ((void *)0)

#define PAGE_SIZE    4096
#define MAX_RANK     16

/* ---- internal state ---- */
static int  *page_allocated = NULL;   /* 1=allocated, 0=free */
static int  *page_rank      = NULL;   /* rank of the block containing this page */
static void *pool           = NULL;   /* start of the managed memory */
static int   total_pages    = 0;      /* number of 4K pages */

/* Doubly-linked free list pointers stored OUTSIDE the pool,
 * indexed by page offset. This avoids corruption from writes
 * to allocated blocks. */
static void **free_next     = NULL;
static void **free_prev     = NULL;

static void *free_lists[MAX_RANK + 1];  /* index 1..16, head of each list */
static int   free_count[MAX_RANK + 1];  /* how many free blocks of each rank */

/* ---- helpers ---- */

/* number of 4K pages in a block of given rank */
static inline int block_pages(int rank) {
    return 1 << (rank - 1);
}

/* buddy offset (in pages) */
static inline int buddy_offset(int offset, int rank) {
    return offset ^ (1 << (rank - 1));
}

/* Get the page offset for a block address */
static inline int addr_to_offset(void *addr) {
    return (int)(((char *)addr - (char *)pool) / PAGE_SIZE);
}

/* Doubly-linked list operations using external arrays */
static void remove_from_free_list(void *addr, int rank) {
    int off = addr_to_offset(addr);
    void *next = free_next[off];
    void *prev = free_prev[off];
    if (prev) {
        free_next[addr_to_offset(prev)] = next;
    } else {
        free_lists[rank] = next;
    }
    if (next) {
        free_prev[addr_to_offset(next)] = prev;
    }
}

static void add_to_free_list(void *addr, int rank) {
    int off = addr_to_offset(addr);
    free_next[off] = free_lists[rank];
    free_prev[off] = NULL;
    if (free_lists[rank]) {
        free_prev[addr_to_offset(free_lists[rank])] = addr;
    }
    free_lists[rank] = addr;
}

/* ---- public API ---- */

int init_page(void *p, int pgcount) {
    /* free previous metadata if any */
    if (page_allocated) { free(page_allocated); page_allocated = NULL; }
    if (page_rank)      { free(page_rank);      page_rank      = NULL; }
    if (free_next)      { free(free_next);      free_next      = NULL; }
    if (free_prev)      { free(free_prev);      free_prev      = NULL; }

    pool        = p;
    total_pages = pgcount;

    /* clear free lists */
    int i;
    for (i = 1; i <= MAX_RANK; i++) {
        free_lists[i] = NULL;
        free_count[i] = 0;
    }

    if (pgcount <= 0) return OK;

    page_allocated = (int  *)calloc((size_t)pgcount, sizeof(int));
    page_rank      = (int  *)calloc((size_t)pgcount, sizeof(int));
    free_next      = (void **)calloc((size_t)pgcount, sizeof(void *));
    free_prev      = (void **)calloc((size_t)pgcount, sizeof(void *));

    if (!page_allocated || !page_rank || !free_next || !free_prev) {
        if (page_allocated) { free(page_allocated); page_allocated = NULL; }
        if (page_rank)      { free(page_rank);      page_rank      = NULL; }
        if (free_next)      { free(free_next);      free_next      = NULL; }
        if (free_prev)      { free(free_prev);      free_prev      = NULL; }
        return -ENOSPC;
    }

    /* Split the whole range into maximal power-of-2 blocks */
    int remaining = pgcount;
    int offset    = 0;

    for (int rank = MAX_RANK; rank >= 1 && remaining > 0; rank--) {
        int bp = block_pages(rank);
        while (remaining >= bp) {
            /* mark all pages of this block */
            int j;
            for (j = offset; j < offset + bp; j++) {
                page_rank[j] = rank;
                /* page_allocated already 0 from calloc */
            }

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

    /* find the smallest rank >= requested rank that has a free block */
    int r;
    for (r = rank; r <= MAX_RANK; r++) {
        if (free_lists[r] != NULL) break;
    }

    if (r > MAX_RANK) return ERR_PTR(-ENOSPC);

    /* take the block from its free list */
    void *block = free_lists[r];
    remove_from_free_list(block, r);
    free_count[r]--;

    int offset = addr_to_offset(block);

    /* split until we reach the requested rank */
    while (r > rank) {
        r--;
        int bp = block_pages(r);

        /* the buddy is the second half */
        int    buddy_off = offset + bp;
        void  *buddy     = (char *)pool + (unsigned long)buddy_off * PAGE_SIZE;

        /* mark buddy as free */
        int j;
        for (j = buddy_off; j < buddy_off + bp; j++) {
            page_rank[j]      = r;
            page_allocated[j] = 0;
        }
        add_to_free_list(buddy, r);
        free_count[r]++;

        /* first half (offset unchanged) continues to be split */
    }

    /* mark the final block as allocated */
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
    /* validate address */
    if (p == NULL) return -EINVAL;
    if ((char *)p < (char *)pool ||
        (char *)p >= (char *)pool + (unsigned long)total_pages * PAGE_SIZE)
        return -EINVAL;

    unsigned long diff = (char *)p - (char *)pool;
    if (diff % PAGE_SIZE != 0) return -EINVAL;

    int offset = (int)(diff / PAGE_SIZE);

    /* must be the start of an allocated block */
    if (!page_allocated[offset]) return -EINVAL;

    int rank = page_rank[offset];
    int bp   = block_pages(rank);

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

        /* remove buddy from its free list */
        void *buddy_addr = (char *)pool + (unsigned long)buddy_off * PAGE_SIZE;
        remove_from_free_list(buddy_addr, rank);
        free_count[rank]--;

        /* merged block starts at the aligned (lower) offset */
        offset = offset & ~(1 << (rank - 1));
        rank++;

        /* update metadata for the merged block */
        bp = block_pages(rank);
        int j;
        for (j = offset; j < offset + bp; j++) {
            page_rank[j] = rank;
        }
    }

    /* add the (possibly merged) block to the free list */
    add_to_free_list((char *)pool + (unsigned long)offset * PAGE_SIZE, rank);
    free_count[rank]++;

    return OK;
}

int query_ranks(void *p) {
    if (p == NULL) return -EINVAL;
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