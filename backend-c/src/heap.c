/*
 * heap.c - see heap.h.
 */
#include "heap.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

#define AT(h, i) ((h)->data + (i) * (h)->item_size)

void heap_init(Heap *h, size_t item_size, HeapCmp cmp) {
    h->item_size = item_size;
    h->count = 0;
    h->cap = 16;
    h->cmp = cmp;
    h->data = xmalloc(h->cap * item_size);
    h->tmp = xmalloc(item_size);
}

void heap_free(Heap *h) {
    free(h->data);
    free(h->tmp);
    h->data = h->tmp = NULL;
    h->count = h->cap = 0;
}

static void swap(Heap *h, size_t i, size_t j) {
    memcpy(h->tmp, AT(h, i), h->item_size);
    memcpy(AT(h, i), AT(h, j), h->item_size);
    memcpy(AT(h, j), h->tmp, h->item_size);
}

void heap_push(Heap *h, const void *item) {
    if (h->count == h->cap) {
        h->cap *= 2;
        h->data = xrealloc(h->data, h->cap * h->item_size);
    }
    size_t i = h->count++;
    memcpy(AT(h, i), item, h->item_size);
    while (i > 0) {                                    /* sift up */
        size_t parent = (i - 1) / 2;
        if (h->cmp(AT(h, i), AT(h, parent)) >= 0) break;
        swap(h, i, parent);
        i = parent;
    }
}

int heap_pop(Heap *h, void *out) {
    if (h->count == 0) return 0;
    memcpy(out, AT(h, 0), h->item_size);
    h->count--;
    if (h->count == 0) return 1;
    memcpy(AT(h, 0), AT(h, h->count), h->item_size);
    size_t i = 0;
    for (;;) {                                         /* sift down */
        size_t left = 2 * i + 1, right = left + 1, smallest = i;
        if (left < h->count && h->cmp(AT(h, left), AT(h, smallest)) < 0) smallest = left;
        if (right < h->count && h->cmp(AT(h, right), AT(h, smallest)) < 0) smallest = right;
        if (smallest == i) break;
        swap(h, i, smallest);
        i = smallest;
    }
    return 1;
}

void *heap_item(const Heap *h, size_t i) { return AT(h, i); }
