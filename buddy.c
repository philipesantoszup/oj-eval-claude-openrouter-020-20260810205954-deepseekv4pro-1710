#include "buddy.h"
#include <stdlib.h>
#include <string.h>

#define PAGE_SIZE    4096
#define MAX_RANK     16

/* ---- internal state ----
 * page_rank[i] encodes the rank of the block starting at page i (always positive).
 *   > 0  → block of that rank starts at page i
 *   == 0 → page i is NOT the first page of any block
 * page_allocated[i] is 1 if page i is the start of an allocated block, 0 otherwise.
 * Only the first page of each block has non-zero page_rank and a meaningful page_allocated.
 */
static char *page_allocated = NULL;
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

    /* Split into maximal power-of-2 blocks (positive → free, page_allocated=0) */
    int remaining = pgcount;
    int offset    = 0;

    for (int rank = MAX_RANK; rank >= 1 && remaining > 0; rank--) {
        int bp = block_pages(rank);
        while (remaining >= bp) {
            page_rank[offset] = rank;   /* positive = free */
            /* page_allocated[offset] already 0 from calloc */

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
        /* page_allocated[buddy_off] already 0 from calloc */
        add_to_free_list(buddy, r);
        free_count[r]++;

        /* first half continues to be split */
    }

    /* mark as allocated */
    page_rank[offset]      = rank;   /* positive, but we track via page_allocated */
    page_allocated[offset] = 1;

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

    /* mark as free */
    page_allocated[offset] = 0;

    /* try to merge with buddy */
    while (rank < MAX_RANK) {
        int buddy_off = buddy_offset(offset, rank);
        int bp = block_pages(rank);

        if (buddy_off < 0 || buddy_off + bp > total_pages) break;
        /* buddy must be free (not allocated) and same rank */
        if (page_allocated[buddy_off] || page_rank[buddy_off] != rank) break;

        void *buddy_addr = (char *)pool + (unsigned long)buddy_off * PAGE_SIZE;
        remove_from_free_list(buddy_addr, rank);
        free_count[rank]--;
        page_rank[buddy_off]      = 0;   /* clear stale rank */
        page_allocated[buddy_off] = 0;   /* clear stale allocated flag */

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
    if (p == NULL || total_pages == 0 || page_rank == NULL) return -EINVAL;
    if ((char *)p < (char *)pool ||
        (char *)p >= (char *)pool + (unsigned long)total_pages * PAGE_SIZE)
        return -EINVAL;

    unsigned long diff = (char *)p - (char *)pool;
    if (diff % PAGE_SIZE != 0) return -EINVAL;

    int offset = (int)(diff / PAGE_SIZE);

    /* scan from largest rank down to find the containing block.
     * Only the first page of each block has a non-zero page_rank.
     * For inner pages, we must scan to find the block start. */
    int r;
    for (r = MAX_RANK; r >= 1; r--) {
        int bs = offset & ~(block_pages(r) - 1);
        if (bs >= total_pages) continue;
        int val = page_rank[bs];
        if (val == 0) continue;
        int block_end = bs + block_pages(val);
        if (offset >= bs && offset < block_end) {
            return val;
        }
    }
    return 0;
}

int query_page_counts(int rank) {
    if (rank < 1 || rank > MAX_RANK) return -EINVAL;
    return free_count[rank];
}