#include "job_queue.h"

#include <stdlib.h>
#include <string.h>

int job_queue_init(struct job_queue *q, size_t cap)
{
    q->slots = calloc(cap, sizeof(*q->slots));
    if (!q->slots)
        return -1;
    if (pthread_mutex_init(&q->mu, NULL) != 0) {
        free(q->slots);
        q->slots = NULL;
        return -1;
    }
    q->cap = cap;
    q->head = q->count = 0;
    return 0;
}

void job_queue_free(struct job_queue *q)
{
    if (!q->slots)
        return;
    pthread_mutex_destroy(&q->mu);
    free(q->slots);
    q->slots = NULL;
}

int job_queue_push(struct job_queue *q, const char *name, size_t len, int *was_empty)
{
    pthread_mutex_lock(&q->mu);
    if (q->count == q->cap) {
        pthread_mutex_unlock(&q->mu);
        return -1;
    }
    char *slot = q->slots[(q->head + q->count) % q->cap];
    memcpy(slot, name, len);
    slot[len] = '\0';
    *was_empty = (q->count++ == 0);
    pthread_mutex_unlock(&q->mu);
    return 0;
}

size_t job_queue_pop(struct job_queue *q, job_name_t *out, size_t max)
{
    size_t n = 0;
    pthread_mutex_lock(&q->mu);
    while (n < max && q->count > 0) {
        memcpy(out[n++], q->slots[q->head], sizeof(job_name_t));
        q->head = (q->head + 1) % q->cap;
        q->count--;
    }
    pthread_mutex_unlock(&q->mu);
    return n;
}
