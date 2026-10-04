/*
 * Intrusive binary min-heap of timers, keyed by absolute deadline (ms,
 * CLOCK_MONOTONIC). Owned and used by the module thread only - no locking.
 */
#ifndef DNS_CACHE_TIMER_HEAP_H
#define DNS_CACHE_TIMER_HEAP_H

#include <stddef.h>
#include <stdint.h>

#define TIMER_NOT_ARMED ((size_t)-1)

struct timer_node {
    uint64_t at;   /* absolute deadline, ms */
    size_t   idx;  /* position in the heap, TIMER_NOT_ARMED if not queued */
};

struct timer_heap {
    struct timer_node **a;
    size_t n, cap;
};

int  timer_heap_init(struct timer_heap *h, size_t cap);
void timer_heap_free(struct timer_heap *h);

static inline void timer_node_init(struct timer_node *t) { t->at = 0; t->idx = TIMER_NOT_ARMED; }
static inline int  timer_node_armed(const struct timer_node *t) { return t->idx != TIMER_NOT_ARMED; }

/* Arm (or re-arm) t to fire at 'at'. Returns -1 if the heap is full. */
int  timer_heap_set(struct timer_heap *h, struct timer_node *t, uint64_t at);
/* Disarm t if armed. */
void timer_heap_remove(struct timer_heap *h, struct timer_node *t);
/* Earliest timer or NULL. */
static inline struct timer_node *timer_heap_peek(const struct timer_heap *h) { return h->n ? h->a[0] : NULL; }

#endif
