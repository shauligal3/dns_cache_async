/*
 * dns_cache.h - asynchronous, non-blocking DNS cache for data-path threads.
 *
 * The cache owns one background "module" thread. Data-path threads call
 * dns_cache_resolve(), which never blocks on the network:
 *
 *   - cached mapping  -> returns 0 and the next address of the entry,
 *                        rotating round-robin through the address list
 *                        (atomic counter, safe from any number of threads);
 *   - no mapping yet  -> returns EAGAIN and queues a resolution job for the
 *                        module thread (getaddrinfo_a() / gai_error());
 *   - NXDOMAIN        -> returns ENOTFOUND (negatively cached for a while).
 *
 * Entries live in a fixed-size LRU. Before an entry's TTL expires the module
 * thread re-resolves it in the background (timer driven), so hot names never
 * fall out of the cache.
 */
#ifndef DNS_CACHE_H
#define DNS_CACHE_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) && !defined(DNS_CACHE_STATIC)
#define DNS_CACHE_API __attribute__((visibility("default")))
#else
#define DNS_CACHE_API
#endif

/*
 * "Host not found" (NXDOMAIN / no address of the requested family).
 * ENOTFOUND is not a standard errno on Linux, so the library defines its own
 * value, chosen well outside the range of real errno values.
 */
#define DNS_CACHE_ENOTFOUND 0x10000
#ifndef ENOTFOUND
#define ENOTFOUND DNS_CACHE_ENOTFOUND
#endif

typedef struct dns_cache dns_cache_t;

typedef struct dns_cache_config {
    /* Maximum number of cached names (positive, negative and pending). */
    size_t   capacity;            /* default 1024 */
    /* Lifetime of a positive mapping. getaddrinfo() does not expose the DNS
     * record TTL, so the cache applies this configured TTL. */
    uint32_t ttl_ms;              /* default 60000 */
    /* Re-resolve this long before the TTL expires (clamped to ttl/2). */
    uint32_t refresh_ahead_ms;    /* default 5000 */
    /* How long an NXDOMAIN answer is cached (ENOTFOUND is returned). */
    uint32_t negative_ttl_ms;     /* default 30000 */
    /* Back-off before retrying after a transient failure (EAI_AGAIN, ...). */
    uint32_t retry_ms;            /* default 2000 */
    /* If refreshes keep failing transiently, keep serving the old addresses
     * for this long past the TTL before dropping the mapping. */
    uint32_t stale_grace_ms;      /* default 30000 */
    /* If non-zero, an entry not queried for this long is not refreshed and
     * simply expires. 0 = always refresh until evicted by the LRU. */
    uint32_t idle_timeout_ms;     /* default 0 */
    /* Capacity of the job queue between data path and module thread. */
    size_t   queue_size;          /* default 1024 */
    /* Maximum concurrent getaddrinfo_a() requests. */
    unsigned max_inflight;        /* default 64 */
    /* Maximum addresses kept per name (1..64). */
    unsigned max_addrs;           /* default 16 */
    /* AF_UNSPEC (default), AF_INET or AF_INET6. */
    int      family;
    /* addrinfo.ai_flags passed to getaddrinfo_a(). Default AI_ADDRCONFIG,
     * so a host without IPv6 connectivity does not rotate into AAAA records. */
    int      ai_flags;
} dns_cache_config_t;

typedef struct dns_cache_stats {
    uint64_t hits;            /* lookups answered with an address          */
    uint64_t misses;          /* lookups that returned EAGAIN              */
    uint64_t notfound;        /* lookups that returned ENOTFOUND           */
    uint64_t queue_drops;     /* jobs dropped because the queue was full   */
    uint64_t resolves_started;
    uint64_t resolves_ok;
    uint64_t resolves_nxdomain;
    uint64_t resolves_failed; /* transient failures (EAI_AGAIN, ...)       */
    uint64_t refreshes;       /* background re-resolutions started         */
    uint64_t evictions;       /* LRU evictions                             */
    uint64_t entries;         /* current number of cached names            */
} dns_cache_stats_t;

/* Fill *cfg with the defaults listed above. */
DNS_CACHE_API void dns_cache_config_init(dns_cache_config_t *cfg);

/*
 * Create a cache and start its module thread. cfg may be NULL for defaults.
 * Returns 0 on success or an errno value (EINVAL, ENOMEM, ...).
 */
DNS_CACHE_API int dns_cache_create(const dns_cache_config_t *cfg, dns_cache_t **out);

/*
 * Stop the module thread, cancel/await in-flight resolutions and free
 * everything. No other thread may use the cache during or after this call.
 */
DNS_CACHE_API void dns_cache_destroy(dns_cache_t *cache);

/*
 * Non-blocking lookup. On success returns 0 and stores the next address of
 * the rotation in *addr (port 0) and its length in *addrlen (if not NULL).
 *
 * Returns:
 *   0             address returned
 *   EAGAIN        not resolved yet (a resolution has been scheduled) or a
 *                 transient resolver failure; try again later
 *   ENOTFOUND     the name does not exist (NXDOMAIN)
 *   EINVAL        bad arguments / malformed host name
 *   ENAMETOOLONG  host name longer than 253 characters
 *
 * Numeric IPv4/IPv6 literals are answered immediately and never cached.
 * Safe to call concurrently from any number of threads.
 */
DNS_CACHE_API int dns_cache_resolve(dns_cache_t *cache, const char *host,
                                    struct sockaddr_storage *addr,
                                    socklen_t *addrlen);

/*
 * Like dns_cache_resolve() but copies every cached address (up to max) into
 * addrs[] without advancing the rotation. *count receives the number of
 * addresses copied. Same return values; does not schedule anything on its
 * own beyond what dns_cache_resolve() would.
 */
DNS_CACHE_API int dns_cache_resolve_all(dns_cache_t *cache, const char *host,
                                        struct sockaddr_storage *addrs,
                                        size_t max, size_t *count);

/* Snapshot of the cache counters. */
DNS_CACHE_API void dns_cache_get_stats(dns_cache_t *cache, dns_cache_stats_t *st);

/* Human readable text for the return codes above. */
DNS_CACHE_API const char *dns_cache_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif /* DNS_CACHE_H */
