/*
 * heap.h - a binary min-heap written by hand (the C version of Python's heapq).
 *
 * The heap stores fixed-size items in one growable array. Item i has children
 * 2i+1 and 2i+2, and every parent is <= its children, so the smallest item is
 * always at index 0.
 *   push: O(log n)  (append, then sift up)
 *   pop:  O(log n)  (take the root, move the last item to the root, sift down)
 * A comparison function decides the order, like qsort's.
 */
#ifndef HEAP_H
#define HEAP_H

#include <stddef.h>

typedef int (*HeapCmp)(const void *a, const void *b);

typedef struct {
    char *data;
    size_t item_size, count, cap;
    HeapCmp cmp;
    char *tmp;                   /* scratch space for one item (used when swapping) */
} Heap;

void heap_init(Heap *h, size_t item_size, HeapCmp cmp);
void heap_free(Heap *h);
void heap_push(Heap *h, const void *item);
int heap_pop(Heap *h, void *out);            /* 0 when the heap is empty */
void *heap_item(const Heap *h, size_t i);    /* raw access, used to inspect the heap for traces */

#endif
