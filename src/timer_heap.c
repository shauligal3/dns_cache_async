#include "timer_heap.h"

#include <stdlib.h>

int timer_heap_init(struct timer_heap *h, size_t cap)
{
    h->a = calloc(cap ? cap : 1, sizeof(*h->a));
    h->n = 0;
    h->cap = cap ? cap : 1;
    return h->a ? 0 : -1;
}

void timer_heap_free(struct timer_heap *h)
{
    free(h->a);
    h->a = NULL;
    h->n = h->cap = 0;
}

static void place(struct timer_heap *h, size_t i, struct timer_node *t)
{
    h->a[i] = t;
    t->idx = i;
}

static void sift_up(struct timer_heap *h, size_t i)
{
    struct timer_node *t = h->a[i];
    while (i > 0) {
        size_t p = (i - 1) / 2;
        if (h->a[p]->at <= t->at)
            break;
        place(h, i, h->a[p]);
        i = p;
    }
    place(h, i, t);
}

static void sift_down(struct timer_heap *h, size_t i)
{
    struct timer_node *t = h->a[i];
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, m = i;
        uint64_t best = t->at;
        if (l < h->n && h->a[l]->at < best) { m = l; best = h->a[l]->at; }
        if (r < h->n && h->a[r]->at < best) { m = r; }
        if (m == i)
            break;
        place(h, i, h->a[m]);
        i = m;
    }
    place(h, i, t);
}

int timer_heap_set(struct timer_heap *h, struct timer_node *t, uint64_t at)
{
    if (timer_node_armed(t)) {
        uint64_t old = t->at;
        t->at = at;
        if (at < old)
            sift_up(h, t->idx);
        else
            sift_down(h, t->idx);
        return 0;
    }
    if (h->n == h->cap)
        return -1;
    t->at = at;
    place(h, h->n++, t);
    sift_up(h, t->idx);
    return 0;
}

void timer_heap_remove(struct timer_heap *h, struct timer_node *t)
{
    if (!timer_node_armed(t))
        return;
    size_t i = t->idx;
    t->idx = TIMER_NOT_ARMED;
    if (--h->n == i)
        return;
    struct timer_node *last = h->a[h->n];
    place(h, i, last);
    if (i > 0 && h->a[(i - 1) / 2]->at > last->at)
        sift_up(h, i);
    else
        sift_down(h, i);
}
