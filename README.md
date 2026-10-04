# dns_cache_async

Asynchronous DNS cache for a data path, as a C shared library (`libdnscache.so`).

Data-path threads never block on DNS. A lookup either returns an address from
the cache immediately, or returns `EAGAIN` and lets the library's own thread
resolve the name in the background with `getaddrinfo_a()` / `gai_error()`.

```c
#include <dns_cache.h>

dns_cache_t *cache;
dns_cache_create(NULL, &cache);              /* defaults; starts the module thread */

struct sockaddr_storage ss;
socklen_t len;
switch (dns_cache_resolve(cache, "api.example.com", &ss, &len)) {
case 0:         /* ss holds the next address in the rotation (port 0) */ break;
case EAGAIN:    /* not cached yet - resolution scheduled, retry later  */ break;
case ENOTFOUND: /* NXDOMAIN                                            */ break;
}

dns_cache_destroy(cache);
```

## Behaviour

| Cache state for the name          | `dns_cache_resolve()` returns                               |
|-----------------------------------|-------------------------------------------------------------|
| mapping cached                    | `0` + the **next** address (round-robin via an atomic counter) |
| no mapping                        | `EAGAIN`; a resolution job is queued for the module thread  |
| resolution in progress            | `EAGAIN`                                                    |
| resolution failed: NXDOMAIN       | `ENOTFOUND` (cached for `negative_ttl_ms`)                  |
| resolution failed transiently     | `EAGAIN` (retried after `retry_ms`)                         |
| numeric IPv4/IPv6 literal         | `0` immediately, never cached                               |
| malformed name                    | `EINVAL` / `ENAMETOOLONG`                                   |

`ENOTFOUND` is not a standard errno on Linux; the header defines it
(`DNS_CACHE_ENOTFOUND`, outside the errno range) if the platform does not.
Names are case-insensitive and a trailing dot is ignored.

**TTL and refresh.** Each positive mapping lives for `ttl_ms`. Shortly before it
expires (`refresh_ahead_ms`), a timer makes the module thread re-resolve it, so
hot names are never missing from the cache. If a refresh fails transiently the
old addresses keep being served for up to `stale_grace_ms` past the TTL while
retries continue; an NXDOMAIN answer switches the entry to negative.
`getaddrinfo()` does not expose record TTLs, so the TTL is a configuration value.

**LRU.** The cache holds at most `capacity` names (positive, negative and
pending). When a new name needs a slot, the least recently used entry is evicted.
Entries with a resolution in flight are never evicted.

## API

See [`include/dns_cache.h`](include/dns_cache.h).

| Function | |
|---|---|
| `dns_cache_config_init(cfg)` | fill a config with defaults |
| `dns_cache_create(cfg, &cache)` | allocate the cache and start the module thread |
| `dns_cache_resolve(cache, host, &ss, &len)` | non-blocking lookup, one address, rotating |
| `dns_cache_resolve_all(cache, host, arr, max, &n)` | all cached addresses, no rotation |
| `dns_cache_get_stats(cache, &stats)` | hit/miss/resolution/eviction counters |
| `dns_cache_strerror(err)` | text for the return codes |
| `dns_cache_destroy(cache)` | stop the thread, cancel/await resolutions, free |

### Configuration (`dns_cache_config_t`)

| Field | Default | Meaning |
|---|---|---|
| `capacity` | 1024 | max cached names (LRU size) |
| `ttl_ms` | 60000 | lifetime of a positive mapping |
| `refresh_ahead_ms` | 5000 | re-resolve this long before expiry (clamped to ttl/2) |
| `negative_ttl_ms` | 30000 | how long NXDOMAIN is cached |
| `retry_ms` | 2000 | back-off after a transient failure |
| `stale_grace_ms` | 30000 | serve old addresses this long past TTL while refresh keeps failing |
| `idle_timeout_ms` | 0 | if set, names not queried for this long expire instead of refreshing |
| `queue_size` | 1024 | job queue capacity (full queue = job dropped, caller still gets `EAGAIN`) |
| `max_inflight` | 64 | concurrent `getaddrinfo_a()` requests |
| `max_addrs` | 16 | addresses kept per name (≤ 64) |
| `family` | `AF_UNSPEC` | or `AF_INET` / `AF_INET6` |
| `ai_flags` | `AI_ADDRCONFIG` | passed to `getaddrinfo_a()` |

## Design

```
 data-path threads                         module thread ("dns_cache")
 ─────────────────                         ─────────────────────────────
 dns_cache_resolve()                       poll(eventfd, timerfd)
   rdlock ─ hash lookup ─ unlock             ├─ drain job queue → create PENDING entry
   hit:  addrs[atomic rr++ % n]              ├─ getaddrinfo_a(GAI_NOWAIT, SIGEV_THREAD)
         atomic store last_access            ├─ gai_error() on in-flight requests
   miss: push name → job queue ──eventfd──►  ├─ wrlock: publish addrs / state
         return EAGAIN                       ├─ timer heap → refresh / expire / retry
                                             └─ re-arm timerfd to earliest deadline
            getaddrinfo_a completion ──eventfd──┘
```

* **Readers share a `pthread_rwlock` in read mode** and run fully in parallel.
  The only things they write are two atomics per entry: the rotation counter
  and the last-access time. The lock prefers writers so updates are not
  starved.
* **The module thread is the only writer.** It owns the LRU list, a min-heap
  of timers (one per entry) behind a `timerfd`, the start queue and in-flight
  requests, and takes the write lock only to publish changes.
* **Exact LRU without reader writes to the list.** The list is kept sorted by
  the access time last recorded per entry. On eviction, a tail entry whose
  real access time moved on is re-inserted at its correct position; the first
  tail entry whose time is unchanged is provably the least recently used one.
* **Completion notification.** `getaddrinfo_a()` is given a `SIGEV_THREAD`
  callback that just writes to the eventfd (no signals are used, and the module
  thread blocks all signals). The eventfd lives in a refcounted object so a
  late callback can never touch freed memory after `dns_cache_destroy()`. The
  module thread additionally polls `gai_error()` every 100 ms while requests
  are in flight, as a safety net.
* **Entries are preallocated**; hit/miss paths do no allocation. Misses wake
  the module thread only when the job queue goes from empty to non-empty.

## Build and test

Linux + glibc (`getaddrinfo_a` is a GNU extension).

```sh
make              # build/libdnscache.so.1.0.0 (+ .a, example)
make test         # test suite (uses the real resolver)
make asan         # tests under AddressSanitizer + UBSan
make memcheck     # tests under valgrind
make install PREFIX=/usr/local
```

Link with `-ldnscache -lanl -lpthread` (on glibc ≥ 2.34 `-lanl` is a stub and
optional). Try it:

```sh
./build/dnsc_resolve -n 3 localhost one.one.one.one nope.invalid
```

The tests need `localhost` in `/etc/hosts`; tests that need public DNS
(NXDOMAIN, multi-address rotation, LRU via negative entries) are skipped
automatically when it is unreachable.

## Notes and limitations

* `dns_cache_destroy()` cancels queued resolutions but must wait for ones
  already running inside glibc, which can take up to the resolver timeout
  (`options timeout`/`attempts` in `/etc/resolv.conf`).
* ThreadSanitizer cannot be used with this library: glibc ≥ 2.34 starts
  `getaddrinfo_a()` worker threads via an internal `pthread_create()` that TSan
  does not intercept, and TSan crashes inside them. Helgrind reports only
  known false positives (C11 atomics, and `gaicb` fields handed over by glibc).
