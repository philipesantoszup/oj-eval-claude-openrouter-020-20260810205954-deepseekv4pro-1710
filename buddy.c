#include "buddy.h"
#include <stdlib.h>
#include <string.h>
#define NULL ((void *)0)

#define PAGE_SIZE    4096
#define MAX_RANK     16

/* ---- internal state ---- */
static int  *page_allocated = NULL;   /* 1=allocated, 0=free  (accurate for every page) */
static int  *page_rank      = NULL;   /* rank of the block; accurate ONLY at the first
                                         page of each block.  Inner pages may be stale. */
static void *pool           = NULL;   /* start of the managed memory */
static int   total_pages    = 0;      /* number of 4K pages */

static void *free_lists[MAX_RANK + 1];  /* index 1..16, singly-linked */
static int   free_count[MAX_RANK + 1];  /* how many free blocks of each rank */

/* ---- helpers ---- */

static inline int block_pages(int rank) {
    return 1 << (rank - 1);
}

static inline int buddy_offset(int offset, int rank) {
    return offset ^ (1 << (rank - 1));
}

static void remove_from_free_list(void *addr, int rank) {
    void **curr = &free_lists[rank];
    while (*curr) {
        if (*curr == addr) {
            *curr = *(void **)(*curr);
            return;
        }
        curr = (void **)(*curr);
    }
}

static void add_to_free_list(void *addr, int rank) {
    *(void **)addr = free_lists[rank];
    free_lists[rank] = addr;
}

/* ---- public API ---- */

int init_page(void *p, int pgcount) {
    if (page_allocated) { free(page_allocated); page_allocated = NULL; }
    if (page_rank)      { free(page_rank);      page_rank      = NULL; }

    pool        = p;
    total_pages = pgcount;

    int i;
    for (i = 1; i <= MAX_RANK; i++) {
        free_lists[i] = NULL;
        free_count[i] = 0;
    }

    if (pgcount <= 0) return OK;

    page_allocated = (int *)calloc((size_t)pgcount, sizeof(int));
    page_rank      = (int *)calloc((size_t)pgcount, sizeof(int));

    if (!page_allocated || !page_rank) {
        if (page_allocated) { free(page_allocated); page_allocated = NULL; }
        if (page_rank)      { free(page_rank);      page_rank      = NULL; }
        return -ENOSPC;
    }

    /* Split the whole range into maximal power-of-2 blocks.
     * Only set page_rank at the first page of each block. */
    int remaining = pgcount;
    int offset    = 0;

    for (int rank = MAX_RANK; rank >= 1 && remaining > 0; rank--) {
        int bp = block_pages(rank);
        while (remaining >= bp) {
            page_rank[offset] = rank;   /* first page only */

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

        /* mark buddy's first page only */
        page_rank[buddy_off] = r;
        page_allocated[buddy_off] = 0;
        add_to_free_list(buddy, r);
        free_count[r]++;

        /* first half continues to be split */
    }

    /* mark the allocated block: set allocated flag for every page,
     * set rank only at the first page */
    {
        int bp = block_pages(rank);
        int j;
        for (j = offset; j < offset + bp; j++) {
            page_allocated[j] = 1;
        }
        page_rank[offset] = rank;
    }

    return block;
}

int return_pages(void *p) {
    if (p == NULL) return -EINVAL;
    if ((char *)p < (char *)pool ||
        (char *)p >= (char *)pool + (unsigned long)total_pages * PAGE_SIZE)
        return -EINVAL;

    unsigned long diff = (char *)p - (char *)pool;
    if (diff % PAGE_SIZE != 0) return -EINVAL;

    int offset = (int)(diff / PAGE_SIZE);

    /* must be the start of an allocated block */
    if (!page_allocated[offset]) return -EINVAL;

    /* find the rank of this allocated block */
    int rank = page_rank[offset];
    if (rank < 1 || rank > MAX_RANK) return -EINVAL;

    int bp = block_pages(rank);

    /* mark all pages of the block as free */
    {
        int j;
        for (j = offset; j < offset + bp; j++) {
            page_allocated[j] = 0;
        }
    }
    /* page_rank[offset] stays – it is still the correct rank for the free block */

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

        /* update rank only at the first page of the merged block */
        page_rank[offset] = rank;
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

    if (page_allocated[offset]) {
        /* allocated page: find the block start by scanning alignment */
        int r;
        for (r = MAX_RANK; r >= 1; r--) {
            int bs = offset & ~(block_pages(r) - 1);
            if (page_allocated[bs] && page_rank[bs] == r) {
                return r;
            }
        }
        /* fallback: should not reach here */
        return page_rank[offset];
    } else {
        /* free page: find the containing free block */
        int r;
        for (r = MAX_RANK; r >= 1; r--) {
            int bs = offset & ~(block_pages(r) - 1);
            if (bs < total_pages && !page_allocated[bs] && page_rank[bs] == r) {
                return r;
            }
        }
        /* fallback */
        return page_rank[offset];
    }
}

int query_page_counts(int rank) {
    if (rank < 1 || rank > MAX_RANK) return -EINVAL;
    return free_count[rank];
}