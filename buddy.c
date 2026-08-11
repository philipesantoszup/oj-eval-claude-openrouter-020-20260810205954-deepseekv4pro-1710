#include "buddy.h"
#include <stdlib.h>
#include <string.h>
#define NULL ((void *)0)

#define PAGE_SIZE    4096
#define MAX_RANK     16

/* ---- internal state ----
 * page_rank[i] encodes both the rank and the allocation status:
 *   > 0  → free block starting at page i (rank = value)
 *   < 0  → allocated block starting at page i (rank = -value)
 *   == 0 → page i is NOT the first page of any block (use scan to find block)
 * Only the first page of each block has a non-zero page_rank.
 */
static int  *page_rank      = NULL;
static void *pool           = NULL;
static int   total_pages    = 0;

/* Doubly-linked free lists: each free block stores [next, prev] at its start */
static void *free_lists[MAX_RANK + 1];
static int   free_count[MAX_RANK + 1];

/* ---- helpers ---- */

static inline int block_pages(int rank) {
    return 1 << (rank - 1);
}

static inline int buddy_offset(int offset, int rank) {
    return offset ^ (1 << (rank - 1));
}

static void remove_from_free_list(void *addr, int rank) {
    void *next = ((void **)addr)[0];
    void *prev = ((void **)addr)[1];
    if (prev) {
        ((void **)prev)[0] = next;
    } else {
        free_lists[rank] = next;
    }
    if (next) {
        ((void **)next)[1] = prev;
    }
}

static void add_to_free_list(void *addr, int rank) {
    ((void **)addr)[0] = free_lists[rank];
    ((void **)addr)[1] = NULL;
    if (free_lists[rank]) {
        ((void **)free_lists[rank])[1] = addr;
    }
    free_lists[rank] = addr;
}

/* ---- public API ---- */

int init_page(void *p, int pgcount) {
    if (page_rank) { free(page_rank); page_rank = NULL; }

    pool        = p;
    total_pages = pgcount;

    int i;
    for (i = 1; i <= MAX_RANK; i++) {
        free_lists[i] = NULL;
        free_count[i] = 0;
    }

    if (pgcount <= 0) return OK;

    page_rank = (int *)calloc((size_t)pgcount, sizeof(int));
    if (!page_rank) return -ENOSPC;

    /* Split into maximal power-of-2 blocks (positive → free) */
    int remaining = pgcount;
    int offset    = 0;

    for (int rank = MAX_RANK; rank >= 1 && remaining > 0; rank--) {
        int bp = block_pages(rank);
        while (remaining >= bp) {
            page_rank[offset] = rank;   /* positive = free */

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

        page_rank[buddy_off] = r;   /* positive = free */
        add_to_free_list(buddy, r);
        free_count[r]++;

        /* first half continues to be split */
    }

    /* mark as allocated: negative rank */
    page_rank[offset] = -rank;

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

    /* must be the start of an allocated block (negative rank) */
    int stored = page_rank[offset];
    if (stored >= 0) return -EINVAL;

    int rank = -stored;
    if (rank < 1 || rank > MAX_RANK) return -EINVAL;

    /* mark as free: positive rank */
    page_rank[offset] = rank;

    /* try to merge with buddy */
    while (rank < MAX_RANK) {
        int buddy_off = buddy_offset(offset, rank);
        int bp = block_pages(rank);

        if (buddy_off < 0 || buddy_off + bp > total_pages) break;
        /* buddy must be free (positive) and same rank */
        if (page_rank[buddy_off] <= 0 || page_rank[buddy_off] != rank) break;

        void *buddy_addr = (char *)pool + (unsigned long)buddy_off * PAGE_SIZE;
        remove_from_free_list(buddy_addr, rank);
        free_count[rank]--;

        /* merged block starts at the aligned offset */
        offset = offset & ~(1 << (rank - 1));
        rank++;

        page_rank[offset] = rank;   /* positive = free */
    }

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

    /* scan from largest rank down to find the containing block */
    int r;
    for (r = MAX_RANK; r >= 1; r--) {
        int bs = offset & ~(block_pages(r) - 1);
        if (bs >= total_pages) continue;
        int val = page_rank[bs];
        if (val == 0) continue;
        int block_rank = (val > 0) ? val : -val;
        if (block_rank == r) {
            return block_rank;
        }
    }
    return 0;
}

int query_page_counts(int rank) {
    if (rank < 1 || rank > MAX_RANK) return -EINVAL;
    return free_count[rank];
}