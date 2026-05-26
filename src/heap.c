/* Flat-array binary min-heap keyed on (time, priority, seq).
 * Single-file; grows geometrically. */

#include "internal.h"

#include <stdlib.h>
#include <string.h>

static inline int entry_lt(const heap_entry_t *a, const heap_entry_t *b) {
    if (a->time < b->time) return 1;
    if (a->time > b->time) return 0;
    if (a->priority < b->priority) return 1;
    if (a->priority > b->priority) return 0;
    return a->seq < b->seq;
}

void heap_init(event_heap_t *h) {
    h->data = NULL;
    h->len = 0;
    h->cap = 0;
}

void heap_free(event_heap_t *h) {
    free(h->data);
    h->data = NULL;
    h->len = h->cap = 0;
}

static void heap_grow(event_heap_t *h) {
    size_t ncap = h->cap ? h->cap * 2 : 32;
    h->data = (heap_entry_t *)realloc(h->data, ncap * sizeof(*h->data));
    h->cap = ncap;
}

static void sift_up(heap_entry_t *a, size_t i) {
    while (i > 0) {
        size_t p = (i - 1) >> 1;
        if (entry_lt(&a[i], &a[p])) {
            heap_entry_t t = a[i]; a[i] = a[p]; a[p] = t;
            i = p;
        } else break;
    }
}

static void sift_down(heap_entry_t *a, size_t n, size_t i) {
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, best = i;
        if (l < n && entry_lt(&a[l], &a[best])) best = l;
        if (r < n && entry_lt(&a[r], &a[best])) best = r;
        if (best == i) break;
        heap_entry_t t = a[i]; a[i] = a[best]; a[best] = t;
        i = best;
    }
}

void heap_push(event_heap_t *h, const heap_entry_t *e) {
    if (h->len == h->cap) heap_grow(h);
    h->data[h->len] = *e;
    sift_up(h->data, h->len);
    h->len++;
}

int heap_pop(event_heap_t *h, heap_entry_t *out) {
    if (h->len == 0) return -1;
    *out = h->data[0];
    h->len--;
    if (h->len > 0) {
        h->data[0] = h->data[h->len];
        sift_down(h->data, h->len, 0);
    }
    return 0;
}

int heap_peek(const event_heap_t *h, heap_entry_t *out) {
    if (h->len == 0) return -1;
    *out = h->data[0];
    return 0;
}
