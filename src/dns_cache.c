/*
 * dns_cache.c - asynchronous DNS cache (see include/dns_cache.h).
 *
 * Threading model
 * ---------------
 *  - Data-path threads only *read* the cache. They take the rwlock in read
 *    mode, so any number of them proceed in parallel. The only things they
 *    write are two atomics per entry: the round-robin counter (address
 *    rotation) and the last-access timestamp (used by the LRU).
 *    On a miss they push the name onto the job queue and return EAGAIN.
 *
 *  - The module thread is the only writer. It inserts, updates and evicts
 *    entries, owns the LRU list, the timer heap, the start queue and all
 *    in-flight getaddrinfo_a() requests. It takes the rwlock in write mode
 *    only for the short moments it changes something a reader can see
 *    (hash-chain links, state, address list).
 *
 *  - The module thread sleeps in poll() on two fds:
 *      * an eventfd, written by data-path threads (new job) and by the
 *        getaddrinfo_a() completion notification (SIGEV_THREAD);
 *      * a timerfd, armed to the earliest deadline in the timer heap
 *        (refresh before TTL expiry, negative-cache expiry, retry back-off).
 *
 * LRU
 * ---
 *  Fixed capacity, preallocated entries. Readers cannot relink a shared list
 *  without a write lock, so "touch" is just an atomic store of the access
 *  time. The module thread keeps the list ordered by the access time it last
 *  recorded for each entry; when it needs a slot it fixes up stale positions
 *  starting from the tail until the tail is provably the least recently used
 *  entry, and evicts that one (see evict_one()). Entries with a resolution
 *  in progress are never evicted.
 */
#define _GNU_SOURCE
#include "dns_cache.h"

#include "job_queue.h"
#include "timer_heap.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#define INTERNAL __attribute__((visibility("hidden")))

#define MAX_ADDRS_LIMIT    64
#define JOB_BATCH          32
/* Safety net: while requests are in flight, poll gai_error() at least this
 * often even if no completion notification arrives. */
#define INFLIGHT_POLL_MS   100
#define SHUTDOWN_POLL_MS   100

#define container_of(p, T, m) ((T *)((char *)(p) - offsetof(T, m)))

enum entry_state {
    ST_PENDING,   /* first resolution queued or running        -> EAGAIN    */
    ST_VALID,     /* addresses available                       -> 0         */
    ST_NEGATIVE,  /* NXDOMAIN, cached for negative_ttl         -> ENOTFOUND */
    ST_FAILED,    /* transient failure, held for retry_ms      -> EAGAIN    */
};

typedef union {
    struct sockaddr     sa;
    struct sockaddr_in  in4;
    struct sockaddr_in6 in6;
} addr_u;

struct entry {
    /* ---- visible to readers: written under the write lock ---- */
    struct entry    *hnext;          /* hash chain */
    uint32_t         hash;
    int              state;
    unsigned         naddrs;
    addr_u          *addrs;          /* max_addrs slots, allocated once */
    char             host[DNS_HOST_MAX + 1];
    /* ---- written by readers ---- */
    _Atomic uint32_t rr;             /* rotation counter */
    _Atomic uint64_t last_access;    /* ms, CLOCK_MONOTONIC */
    /* ---- module thread private ---- */
    struct entry    *lru_prev, *lru_next;
    uint64_t         lru_stamp;      /* last_access when positioned in LRU */
    struct entry    *start_next;     /* start queue link */
    bool             queued;         /* in the start queue */
    bool             inflight;       /* getaddrinfo_a() running */
    uint64_t         expires_at;     /* TTL deadline */
    uint64_t         hard_expire;    /* expires_at + stale grace */
    struct timer_node timer;
    struct addrinfo  hints;
    struct gaicb     cb;
};

/*
 * Wake-up channel shared with getaddrinfo_a() completion callbacks.
 * glibc runs SIGEV_THREAD callbacks on a thread of its own, possibly after
 * gai_error() already reports the request as finished - and so possibly
 * after the cache has been destroyed. The eventfd therefore lives in a
 * refcounted object: one reference for the cache plus one per pending
 * notification; whoever drops the last one closes and frees it.
 */
struct notifier {
    _Atomic int refs;
    int         efd;
};

struct dns_cache {
    dns_cache_config_t  cfg;

    pthread_rwlock_t    lock;
    struct entry      **buckets;
    size_t              bucket_mask;

    struct entry       *pool;
    addr_u             *addr_pool;
    struct entry       *free_list;     /* linked through hnext */
    struct entry        lru;           /* sentinel: lru.lru_next = MRU */
    size_t              count;

    struct job_queue    jobs;
    struct notifier    *ntf;
    int                 tfd;
    struct timer_heap   timers;

    struct entry       *start_head, *start_tail;
    struct entry      **inflight;
    unsigned            ninflight;

    pthread_t           thread;
    _Atomic bool        stop;

    struct {
        _Atomic uint64_t hits, misses, notfound, queue_drops;
        _Atomic uint64_t started, ok, nxdomain, failed, refreshes, evictions;
        _Atomic uint64_t entries;
    } st;
};

#define STAT_INC(c, f) atomic_fetch_add_explicit(&(c)->st.f, 1, memory_order_relaxed)

/* ------------------------------------------------------------------------ */
/* helpers                                                                  */
/* ------------------------------------------------------------------------ */

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint32_t hash_name(const char *s, size_t len)
{
    uint32_t h = 2166136261u; /* FNV-1a */
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    return h;
}

/*
 * Lower-case, drop one trailing dot, validate characters and label lengths.
 * Returns 0 and the length in *len, or EINVAL / ENAMETOOLONG.
 */
static int normalize_host(const char *in, char *out, size_t *len)
{
    size_t n = strnlen(in, DNS_HOST_MAX + 2);
    if (n > 1 && in[n - 1] == '.')
        n--;
    if (n == 0)
        return EINVAL;
    if (n > DNS_HOST_MAX)
        return ENAMETOOLONG;

    size_t label = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c >= 'A' && c <= 'Z')
            c = (unsigned char)(c - 'A' + 'a');
        if (c == '.') {
            if (label == 0)
                return EINVAL;    /* empty label */
            label = 0;
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                   c == '-' || c == '_') {
            if (++label > 63)
                return EINVAL;
        } else {
            return EINVAL;
        }
        out[i] = (char)c;
    }
    out[n] = '\0';
    *len = n;
    return 0;
}

static socklen_t addr_len(const addr_u *a)
{
    return a->sa.sa_family == AF_INET ? (socklen_t)sizeof(a->in4)
                                      : (socklen_t)sizeof(a->in6);
}

static void addr_out(const addr_u *a, struct sockaddr_storage *ss, socklen_t *len)
{
    socklen_t l = addr_len(a);
    memset(ss, 0, sizeof(*ss));
    memcpy(ss, a, l);
    if (len)
        *len = l;
}

static bool addr_eq(const addr_u *a, const addr_u *b)
{
    if (a->sa.sa_family != b->sa.sa_family)
        return false;
    if (a->sa.sa_family == AF_INET)
        return a->in4.sin_addr.s_addr == b->in4.sin_addr.s_addr;
    return memcmp(&a->in6.sin6_addr, &b->in6.sin6_addr, sizeof(a->in6.sin6_addr)) == 0 &&
           a->in6.sin6_scope_id == b->in6.sin6_scope_id;
}

/* Numeric literal fast path: no caching, no module thread involved.
 * Returns -1 if 'host' is not a literal. */
static int numeric_literal(const dns_cache_t *c, const char *host, addr_u *a)
{
    memset(a, 0, sizeof(*a));
    if (inet_pton(AF_INET, host, &a->in4.sin_addr) == 1) {
        a->in4.sin_family = AF_INET;
        return c->cfg.family == AF_INET6 ? ENOTFOUND : 0;
    }
    if (inet_pton(AF_INET6, host, &a->in6.sin6_addr) == 1) {
        a->in6.sin6_family = AF_INET6;
        return c->cfg.family == AF_INET ? ENOTFOUND : 0;
    }
    return -1;
}

static void notifier_put(struct notifier *n)
{
    if (atomic_fetch_sub_explicit(&n->refs, 1, memory_order_acq_rel) == 1) {
        close(n->efd);
        free(n);
    }
}

static void notifier_kick(struct notifier *n)
{
    uint64_t one = 1;
    ssize_t r = write(n->efd, &one, sizeof(one)); /* EAGAIN = already signalled */
    (void)r;
}

/* getaddrinfo_a() completion callback (runs on a glibc helper thread). */
static void gai_notify_cb(union sigval sv)
{
    struct notifier *n = sv.sival_ptr;
    notifier_kick(n);
    notifier_put(n);
}

/* Hash lookup. Caller holds the lock (read or write) or is the module thread. */
static struct entry *find_entry(const dns_cache_t *c, const char *name, uint32_t h)
{
    for (struct entry *e = c->buckets[h & c->bucket_mask]; e; e = e->hnext)
        if (e->hash == h && strcmp(e->host, name) == 0)
            return e;
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* data path                                                                */
/* ------------------------------------------------------------------------ */

static void schedule_job(dns_cache_t *c, const char *name, size_t len)
{
    int was_empty = 0;
    if (job_queue_push(&c->jobs, name, len, &was_empty) != 0) {
        STAT_INC(c, queue_drops);
        return;
    }
    if (was_empty)
        notifier_kick(c->ntf);
}

static int lookup(dns_cache_t *c, const char *host,
                  struct sockaddr_storage *out, socklen_t *outlen,
                  size_t max, size_t *count, bool rotate)
{
    if (!c || !host || !out || (!rotate && (!count || max == 0)))
        return EINVAL;
    if (count)
        *count = 0;

    addr_u lit;
    int r = numeric_literal(c, host, &lit);
    if (r >= 0) {
        if (r == 0) {
            addr_out(&lit, out, outlen);
            if (count)
                *count = 1;
        }
        return r;
    }

    char name[DNS_HOST_MAX + 1];
    size_t len;
    r = normalize_host(host, name, &len);
    if (r)
        return r;
    uint32_t h = hash_name(name, len);

    pthread_rwlock_rdlock(&c->lock);
    struct entry *e = find_entry(c, name, h);
    if (!e) {
        pthread_rwlock_unlock(&c->lock);
        STAT_INC(c, misses);
        schedule_job(c, name, len);
        return EAGAIN;
    }

    atomic_store_explicit(&e->last_access, now_ms(), memory_order_relaxed);
    switch (e->state) {
    case ST_VALID:
        if (rotate) {
            uint32_t i = atomic_fetch_add_explicit(&e->rr, 1, memory_order_relaxed);
            addr_out(&e->addrs[i % e->naddrs], out, outlen);
        } else {
            size_t n = e->naddrs < max ? e->naddrs : max;
            for (size_t i = 0; i < n; i++)
                addr_out(&e->addrs[i], &out[i], NULL);
            *count = n;
        }
        r = 0;
        break;
    case ST_NEGATIVE:
        r = ENOTFOUND;
        break;
    default: /* pending, or failed and waiting for back-off */
        r = EAGAIN;
        break;
    }
    pthread_rwlock_unlock(&c->lock);

    if (r == 0)
        STAT_INC(c, hits);
    else if (r == ENOTFOUND)
        STAT_INC(c, notfound);
    else
        STAT_INC(c, misses);
    return r;
}

DNS_CACHE_API int dns_cache_resolve(dns_cache_t *c, const char *host,
                                    struct sockaddr_storage *addr, socklen_t *addrlen)
{
    return lookup(c, host, addr, addrlen, 1, NULL, true);
}

DNS_CACHE_API int dns_cache_resolve_all(dns_cache_t *c, const char *host,
                                        struct sockaddr_storage *addrs,
                                        size_t max, size_t *count)
{
    return lookup(c, host, addrs, NULL, max, count, false);
}

/* ------------------------------------------------------------------------ */
/* module thread: entries, LRU, timers                                      */
/* ------------------------------------------------------------------------ */

static inline bool entry_busy(const struct entry *e) { return e->queued || e->inflight; }

static void lru_unlink(struct entry *e)
{
    e->lru_prev->lru_next = e->lru_next;
    e->lru_next->lru_prev = e->lru_prev;
}

static void lru_push_head(dns_cache_t *c, struct entry *e)
{
    e->lru_prev = &c->lru;
    e->lru_next = c->lru.lru_next;
    c->lru.lru_next->lru_prev = e;
    c->lru.lru_next = e;
    e->lru_stamp = atomic_load_explicit(&e->last_access, memory_order_relaxed);
}

static void timer_arm(dns_cache_t *c, struct entry *e, uint64_t at)
{
    /* The heap holds one slot per entry, so this cannot fail. */
    (void)timer_heap_set(&c->timers, &e->timer, at);
}

/* Set the reader-visible state under the write lock. */
static void set_state(dns_cache_t *c, struct entry *e, int state)
{
    pthread_rwlock_wrlock(&c->lock);
    e->state = state;
    if (state != ST_VALID)
        e->naddrs = 0;
    pthread_rwlock_unlock(&c->lock);
}

/* Unlink a (non-busy) entry from everything and put it on the free list. */
static void remove_entry(dns_cache_t *c, struct entry *e)
{
    pthread_rwlock_wrlock(&c->lock);
    struct entry **pp = &c->buckets[e->hash & c->bucket_mask];
    while (*pp != e)
        pp = &(*pp)->hnext;
    *pp = e->hnext;
    pthread_rwlock_unlock(&c->lock);

    lru_unlink(e);
    timer_heap_remove(&c->timers, &e->timer);
    e->hnext = c->free_list;
    c->free_list = e;
    c->count--;
    atomic_store_explicit(&c->st.entries, c->count, memory_order_relaxed);
}

/*
 * Re-insert e keeping the list ordered by lru_stamp (newest at the head).
 * Recently used entries belong near the head, so the walk is short.
 */
static void lru_insert_sorted(dns_cache_t *c, struct entry *e, uint64_t stamp)
{
    struct entry *pos = c->lru.lru_next;
    while (pos != &c->lru && pos->lru_stamp > stamp)
        pos = pos->lru_next;
    e->lru_stamp = stamp;
    e->lru_next = pos;
    e->lru_prev = pos->lru_prev;
    pos->lru_prev->lru_next = e;
    pos->lru_prev = e;
}

/*
 * Evict the least recently used idle entry; returns it (on the free list) or
 * NULL if every entry is busy.
 *
 * Invariant: the list is sorted by lru_stamp, and every entry's real access
 * time (last_access) is >= its lru_stamp. Hence if the tail's stamp is still
 * equal to its last_access, no entry was used less recently: it is the exact
 * LRU victim. Otherwise the tail is moved to its correct position (lazy
 * promotion) and the next tail is examined.
 */
static struct entry *evict_one(dns_cache_t *c)
{
    size_t budget = 2 * c->count + 1;
    uint64_t now = now_ms();
    while (budget-- > 0) {
        struct entry *e = c->lru.lru_prev;
        if (e == &c->lru)
            return NULL;
        uint64_t la = atomic_load_explicit(&e->last_access, memory_order_relaxed);
        if (entry_busy(e) || la > e->lru_stamp) {
            lru_unlink(e);
            lru_insert_sorted(c, e, entry_busy(e) ? now : la);
            continue;
        }
        remove_entry(c, e);
        STAT_INC(c, evictions);
        return e;
    }
    return NULL;
}

static struct entry *alloc_entry(dns_cache_t *c)
{
    /* evict_one() returns the victim to the free list */
    if (!c->free_list && !evict_one(c))
        return NULL;
    struct entry *e = c->free_list;
    c->free_list = e->hnext;
    return e;
}

static void start_queue_push(dns_cache_t *c, struct entry *e)
{
    e->queued = true;
    e->start_next = NULL;
    if (c->start_tail)
        c->start_tail->start_next = e;
    else
        c->start_head = e;
    c->start_tail = e;
}

/* A job from the data path: create a pending entry and queue a resolution. */
static void handle_job(dns_cache_t *c, const char *name, uint64_t now)
{
    size_t len = strlen(name);
    uint32_t h = hash_name(name, len);
    if (find_entry(c, name, h))
        return; /* duplicate: already cached, pending or held */

    struct entry *e = alloc_entry(c);
    if (!e) {
        STAT_INC(c, queue_drops); /* every slot is busy resolving */
        return;
    }

    addr_u *addrs = e->addrs;
    memset(e, 0, sizeof(*e));
    e->addrs = addrs;
    memcpy(e->host, name, len + 1);
    e->hash = h;
    e->state = ST_PENDING;
    atomic_init(&e->rr, 0);
    atomic_init(&e->last_access, now);
    timer_node_init(&e->timer);

    pthread_rwlock_wrlock(&c->lock);
    e->hnext = c->buckets[h & c->bucket_mask];
    c->buckets[h & c->bucket_mask] = e;
    pthread_rwlock_unlock(&c->lock);

    lru_push_head(c, e);
    c->count++;
    atomic_store_explicit(&c->st.entries, c->count, memory_order_relaxed);
    start_queue_push(c, e);
}

/* ------------------------------------------------------------------------ */
/* module thread: resolutions                                               */
/* ------------------------------------------------------------------------ */

static void on_result(dns_cache_t *c, struct entry *e, int err, uint64_t now);

static void start_resolve(dns_cache_t *c, struct entry *e)
{
    memset(&e->hints, 0, sizeof(e->hints));
    e->hints.ai_family = c->cfg.family;
    e->hints.ai_socktype = SOCK_STREAM; /* one result per address */
    e->hints.ai_flags = c->cfg.ai_flags;
    memset(&e->cb, 0, sizeof(e->cb));
    e->cb.ar_name = e->host;
    e->cb.ar_request = &e->hints;

    struct sigevent sev;
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_notify_function = gai_notify_cb;
    sev.sigev_value.sival_ptr = c->ntf;

    struct gaicb *list[1] = { &e->cb };
    atomic_fetch_add_explicit(&c->ntf->refs, 1, memory_order_relaxed);
    int r = getaddrinfo_a(GAI_NOWAIT, list, 1, &sev);
    STAT_INC(c, started);
    if (r != 0) {
        notifier_put(c->ntf); /* no notification will come */
        on_result(c, e, r, now_ms());
        return;
    }
    e->inflight = true;
    c->inflight[c->ninflight++] = e;
}

static void start_pending(dns_cache_t *c)
{
    while (c->start_head && c->ninflight < c->cfg.max_inflight) {
        struct entry *e = c->start_head;
        c->start_head = e->start_next;
        if (!c->start_head)
            c->start_tail = NULL;
        e->queued = false;
        start_resolve(c, e);
    }
}

static unsigned collect_addrs(const dns_cache_t *c, const struct addrinfo *ai, addr_u *out)
{
    unsigned n = 0;
    for (; ai && n < c->cfg.max_addrs; ai = ai->ai_next) {
        addr_u a;
        memset(&a, 0, sizeof(a));
        if (ai->ai_family == AF_INET && ai->ai_addrlen >= sizeof(a.in4))
            memcpy(&a.in4, ai->ai_addr, sizeof(a.in4));
        else if (ai->ai_family == AF_INET6 && ai->ai_addrlen >= sizeof(a.in6))
            memcpy(&a.in6, ai->ai_addr, sizeof(a.in6));
        else
            continue;
        bool dup = false;
        for (unsigned i = 0; i < n && !dup; i++)
            dup = addr_eq(&out[i], &a);
        if (!dup)
            out[n++] = a;
    }
    return n;
}

static void on_result(dns_cache_t *c, struct entry *e, int err, uint64_t now)
{
    const dns_cache_config_t *cfg = &c->cfg;

    if (err == 0) {
        addr_u tmp[MAX_ADDRS_LIMIT];
        unsigned n = collect_addrs(c, e->cb.ar_result, tmp);
        if (n > 0) {
            pthread_rwlock_wrlock(&c->lock);
            memcpy(e->addrs, tmp, n * sizeof(*tmp));
            e->naddrs = n;
            e->state = ST_VALID;
            pthread_rwlock_unlock(&c->lock);
            e->expires_at = now + cfg->ttl_ms;
            e->hard_expire = e->expires_at + cfg->stale_grace_ms;
            timer_arm(c, e, e->expires_at - cfg->refresh_ahead_ms);
            STAT_INC(c, ok);
            return;
        }
        err = EAI_NODATA; /* nothing usable of the requested family */
    }

    if (err == EAI_NONAME || err == EAI_NODATA || err == EAI_ADDRFAMILY) {
        /* Authoritative "does not exist": negative-cache it. */
        set_state(c, e, ST_NEGATIVE);
        timer_arm(c, e, now + cfg->negative_ttl_ms);
        STAT_INC(c, nxdomain);
        return;
    }

    /* Transient (EAI_AGAIN, EAI_FAIL, EAI_SYSTEM, EAI_MEMORY, ...). */
    STAT_INC(c, failed);
    if (e->state == ST_VALID && now < e->hard_expire) {
        /* keep serving the old addresses, retry the refresh later */
        uint64_t at = now + cfg->retry_ms;
        timer_arm(c, e, at < e->hard_expire ? at : e->hard_expire);
    } else {
        set_state(c, e, ST_FAILED);
        timer_arm(c, e, now + cfg->retry_ms);
    }
}

static void collect_completions(dns_cache_t *c)
{
    uint64_t now = now_ms();
    for (unsigned i = 0; i < c->ninflight;) {
        struct entry *e = c->inflight[i];
        int r = gai_error(&e->cb);
        if (r == EAI_INPROGRESS) {
            i++;
            continue;
        }
        c->inflight[i] = c->inflight[--c->ninflight];
        e->inflight = false;
        on_result(c, e, r, now);
        if (e->cb.ar_result) {
            freeaddrinfo(e->cb.ar_result);
            e->cb.ar_result = NULL;
        }
    }
}

static void on_timer(dns_cache_t *c, struct entry *e, uint64_t now)
{
    const dns_cache_config_t *cfg = &c->cfg;

    switch (e->state) {
    case ST_NEGATIVE:
    case ST_FAILED:
        if (!entry_busy(e))
            remove_entry(c, e); /* next query starts over */
        return;

    case ST_VALID: {
        if (now >= e->hard_expire) {
            if (entry_busy(e))
                set_state(c, e, ST_PENDING); /* stop serving stale data */
            else
                remove_entry(c, e);
            return;
        }
        uint64_t la = atomic_load_explicit(&e->last_access, memory_order_relaxed);
        if (cfg->idle_timeout_ms && now - la >= cfg->idle_timeout_ms) {
            if (now >= e->expires_at)
                remove_entry(c, e);         /* idle: let it expire */
            else
                timer_arm(c, e, e->expires_at);
            return;
        }
        if (!entry_busy(e)) {
            STAT_INC(c, refreshes);
            start_queue_push(c, e);
        }
        timer_arm(c, e, e->hard_expire);    /* safety net while refreshing */
        return;
    }

    default:
        return;
    }
}

static void fire_timers(dns_cache_t *c)
{
    uint64_t now = now_ms();
    struct timer_node *t;
    while ((t = timer_heap_peek(&c->timers)) && t->at <= now) {
        timer_heap_remove(&c->timers, t);
        on_timer(c, container_of(t, struct entry, timer), now);
    }
}

static void rearm_timerfd(dns_cache_t *c)
{
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    struct timer_node *t = timer_heap_peek(&c->timers);
    if (t) {
        uint64_t at = t->at ? t->at : 1; /* 0 would disarm */
        its.it_value.tv_sec = (time_t)(at / 1000);
        its.it_value.tv_nsec = (long)(at % 1000) * 1000000L;
    }
    timerfd_settime(c->tfd, TFD_TIMER_ABSTIME, &its, NULL);
}

static void process_jobs(dns_cache_t *c)
{
    job_name_t batch[JOB_BATCH];
    size_t n;
    while ((n = job_queue_pop(&c->jobs, batch, JOB_BATCH)) > 0) {
        uint64_t now = now_ms();
        for (size_t i = 0; i < n; i++)
            handle_job(c, batch[i], now);
    }
}

/* Cancel or wait for every in-flight request before the cache is freed. */
static void drain_inflight(dns_cache_t *c)
{
    for (unsigned i = 0; i < c->ninflight; i++) {
        struct entry *e = c->inflight[i];
        if (gai_cancel(&e->cb) == EAI_CANCELED) {
            /* Removed before it ran: glibc will not notify for it. */
            notifier_put(c->ntf);
            continue;
        }
        const struct gaicb *list[1] = { &e->cb };
        struct timespec ts = { 0, SHUTDOWN_POLL_MS * 1000000L };
        while (gai_error(&e->cb) == EAI_INPROGRESS)
            gai_suspend(list, 1, &ts);
        if (e->cb.ar_result) {
            freeaddrinfo(e->cb.ar_result);
            e->cb.ar_result = NULL;
        }
    }
    c->ninflight = 0;
}

static void *module_thread(void *arg)
{
    dns_cache_t *c = arg;
    struct pollfd pfd[2] = {
        { .fd = c->ntf->efd, .events = POLLIN },
        { .fd = c->tfd,      .events = POLLIN },
    };

    while (!atomic_load_explicit(&c->stop, memory_order_acquire)) {
        int timeout = c->ninflight ? INFLIGHT_POLL_MS : -1;
        if (poll(pfd, 2, timeout) < 0)
            continue; /* EINTR */

        uint64_t v;
        if (pfd[0].revents & POLLIN)
            while (read(c->ntf->efd, &v, sizeof(v)) > 0) {}
        if (pfd[1].revents & POLLIN)
            while (read(c->tfd, &v, sizeof(v)) > 0) {}

        if (atomic_load_explicit(&c->stop, memory_order_acquire))
            break;

        process_jobs(c);
        collect_completions(c);
        fire_timers(c);
        start_pending(c);
        rearm_timerfd(c);
    }

    drain_inflight(c);
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* lifecycle                                                                */
/* ------------------------------------------------------------------------ */

DNS_CACHE_API void dns_cache_config_init(dns_cache_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->capacity = 1024;
    cfg->ttl_ms = 60000;
    cfg->refresh_ahead_ms = 5000;
    cfg->negative_ttl_ms = 30000;
    cfg->retry_ms = 2000;
    cfg->stale_grace_ms = 30000;
    cfg->idle_timeout_ms = 0;
    cfg->queue_size = 1024;
    cfg->max_inflight = 64;
    cfg->max_addrs = 16;
    cfg->family = AF_UNSPEC;
    cfg->ai_flags = AI_ADDRCONFIG;
}

static void free_cache(dns_cache_t *c)
{
    if (c->ntf)
        notifier_put(c->ntf);
    if (c->tfd >= 0)
        close(c->tfd);
    job_queue_free(&c->jobs);
    timer_heap_free(&c->timers);
    pthread_rwlock_destroy(&c->lock);
    free(c->inflight);
    free(c->buckets);
    free(c->addr_pool);
    free(c->pool);
    free(c);
}

DNS_CACHE_API int dns_cache_create(const dns_cache_config_t *user_cfg, dns_cache_t **out)
{
    if (!out)
        return EINVAL;
    *out = NULL;

    dns_cache_config_t cfg;
    if (user_cfg)
        cfg = *user_cfg;
    else
        dns_cache_config_init(&cfg);

    if (cfg.capacity == 0 || cfg.capacity > (1u << 26) || cfg.ttl_ms == 0 ||
        cfg.queue_size == 0 || cfg.max_inflight == 0 ||
        cfg.max_addrs == 0 || cfg.max_addrs > MAX_ADDRS_LIMIT ||
        (cfg.family != AF_UNSPEC && cfg.family != AF_INET && cfg.family != AF_INET6))
        return EINVAL;
    if (cfg.refresh_ahead_ms > cfg.ttl_ms / 2)
        cfg.refresh_ahead_ms = cfg.ttl_ms / 2;
    if (cfg.retry_ms == 0)
        cfg.retry_ms = 1;
    if (cfg.negative_ttl_ms == 0)
        cfg.negative_ttl_ms = 1;

    dns_cache_t *c = calloc(1, sizeof(*c));
    if (!c)
        return ENOMEM;
    c->cfg = cfg;
    c->tfd = -1;
    c->lru.lru_next = c->lru.lru_prev = &c->lru;

    pthread_rwlockattr_t ra;
    pthread_rwlockattr_init(&ra);
    /* Do not let a stream of readers starve the module thread's updates. */
    pthread_rwlockattr_setkind_np(&ra, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
    int r = pthread_rwlock_init(&c->lock, &ra);
    pthread_rwlockattr_destroy(&ra);
    if (r) {
        free(c);
        return r;
    }

    size_t nb = 16;
    while (nb < 2 * cfg.capacity)
        nb <<= 1;
    c->bucket_mask = nb - 1;
    c->buckets = calloc(nb, sizeof(*c->buckets));
    c->pool = calloc(cfg.capacity, sizeof(*c->pool));
    c->addr_pool = calloc(cfg.capacity * cfg.max_addrs, sizeof(*c->addr_pool));
    c->inflight = calloc(cfg.max_inflight, sizeof(*c->inflight));
    c->ntf = calloc(1, sizeof(*c->ntf));
    if (!c->buckets || !c->pool || !c->addr_pool || !c->inflight || !c->ntf ||
        timer_heap_init(&c->timers, cfg.capacity) != 0 ||
        job_queue_init(&c->jobs, cfg.queue_size) != 0) {
        free(c->ntf);
        c->ntf = NULL;
        free_cache(c);
        return ENOMEM;
    }

    for (size_t i = cfg.capacity; i-- > 0;) {
        struct entry *e = &c->pool[i];
        e->addrs = &c->addr_pool[i * cfg.max_addrs];
        timer_node_init(&e->timer);
        e->hnext = c->free_list;
        c->free_list = e;
    }

    atomic_init(&c->ntf->refs, 1);
    c->ntf->efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    c->tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (c->ntf->efd < 0 || c->tfd < 0) {
        r = errno;
        if (c->ntf->efd < 0) {
            free(c->ntf);
            c->ntf = NULL;
        }
        free_cache(c);
        return r ? r : EMFILE;
    }

    /* The module thread must not receive the application's signals. */
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &old);
    r = pthread_create(&c->thread, NULL, module_thread, c);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (r) {
        free_cache(c);
        return r;
    }
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 12))
    pthread_setname_np(c->thread, "dns_cache");
#endif

    *out = c;
    return 0;
}

DNS_CACHE_API void dns_cache_destroy(dns_cache_t *c)
{
    if (!c)
        return;
    atomic_store_explicit(&c->stop, true, memory_order_release);
    notifier_kick(c->ntf);
    pthread_join(c->thread, NULL);
    free_cache(c);
}

DNS_CACHE_API void dns_cache_get_stats(dns_cache_t *c, dns_cache_stats_t *s)
{
    if (!c || !s)
        return;
#define LD(f) atomic_load_explicit(&c->st.f, memory_order_relaxed)
    s->hits = LD(hits);
    s->misses = LD(misses);
    s->notfound = LD(notfound);
    s->queue_drops = LD(queue_drops);
    s->resolves_started = LD(started);
    s->resolves_ok = LD(ok);
    s->resolves_nxdomain = LD(nxdomain);
    s->resolves_failed = LD(failed);
    s->refreshes = LD(refreshes);
    s->evictions = LD(evictions);
    s->entries = LD(entries);
#undef LD
}

DNS_CACHE_API const char *dns_cache_strerror(int err)
{
    switch (err) {
    case 0:            return "success";
    case EAGAIN:       return "resolution in progress, try again";
    case ENOTFOUND:    return "host not found";
    case EINVAL:       return "invalid argument or host name";
    case ENAMETOOLONG: return "host name too long";
    default:           return "unknown error";
    }
}
