/*
 * Bounded multi-producer / single-consumer queue of host names.
 * Producers are data-path threads (on a cache miss); the consumer is the
 * module thread. A short mutex-protected critical section copies the name in
 * or out; producers learn whether the queue was empty so that only the first
 * job after the consumer drained the queue pays for a wake-up syscall.
 */
#ifndef DNS_CACHE_JOB_QUEUE_H
#define DNS_CACHE_JOB_QUEUE_H

#include <pthread.h>
#include <stddef.h>

#define DNS_HOST_MAX 253

typedef char job_name_t[DNS_HOST_MAX + 1];

struct job_queue {
    pthread_mutex_t mu;
    job_name_t *slots;
    size_t cap, head, count;
};

int  job_queue_init(struct job_queue *q, size_t cap);
void job_queue_free(struct job_queue *q);
/* Returns 0, or -1 if the queue is full. *was_empty is set on success. */
int  job_queue_push(struct job_queue *q, const char *name, size_t len, int *was_empty);
/* Moves up to max names into out[]; returns how many. */
size_t job_queue_pop(struct job_queue *q, job_name_t *out, size_t max);

#endif
