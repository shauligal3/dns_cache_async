/*
 * Tests for libdnscache. They use the real system resolver:
 *   - "localhost" (from /etc/hosts) is required;
 *   - tests that need public DNS (NXDOMAIN, multi-address rotation) are
 *     skipped automatically when it is not reachable.
 */
#define _GNU_SOURCE
#include "dns_cache.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int failures, skipped;

#define CHECK(cond) do {                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                     \
            return;                                                         \
        }                                                                   \
    } while (0)

#define SKIP(why) do { printf("  skipped: %s\n", why); skipped++; return; } while (0)

static void sleep_ms(unsigned ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0) {}
}

/* Poll until the answer is no longer EAGAIN (or timeout). */
static int wait_resolve(dns_cache_t *c, const char *host,
                        struct sockaddr_storage *ss, unsigned timeout_ms)
{
    int r = EAGAIN;
    for (unsigned t = 0; t < timeout_ms; t += 5) {
        r = dns_cache_resolve(c, host, ss, NULL);
        if (r != EAGAIN)
            break;
        sleep_ms(5);
    }
    return r;
}

static const char *fmt(const struct sockaddr_storage *ss, char *buf, size_t n)
{
    if (ss->ss_family == AF_INET)
        inet_ntop(AF_INET, &((const struct sockaddr_in *)ss)->sin_addr, buf, (socklen_t)n);
    else
        inet_ntop(AF_INET6, &((const struct sockaddr_in6 *)ss)->sin6_addr, buf, (socklen_t)n);
    return buf;
}

static dns_cache_t *make(void (*tweak)(dns_cache_config_t *))
{
    dns_cache_config_t cfg;
    dns_cache_config_init(&cfg);
    cfg.family = AF_INET;
    if (tweak)
        tweak(&cfg);
    dns_cache_t *c = NULL;
    int r = dns_cache_create(&cfg, &c);
    if (r) {
        fprintf(stderr, "dns_cache_create: %d\n", r);
        exit(1);
    }
    return c;
}

static int have_public_dns = -1;
static const char *NX = "no-such-host-dnscache-test.invalid";

/* ------------------------------------------------------------------------ */

static void test_invalid_args(void)
{
    dns_cache_t *c = make(NULL);
    struct sockaddr_storage ss;
    char longname[300];
    memset(longname, 'a', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = '\0';

    CHECK(dns_cache_resolve(NULL, "localhost", &ss, NULL) == EINVAL);
    CHECK(dns_cache_resolve(c, NULL, &ss, NULL) == EINVAL);
    CHECK(dns_cache_resolve(c, "localhost", NULL, NULL) == EINVAL);
    CHECK(dns_cache_resolve(c, "", &ss, NULL) == EINVAL);
    CHECK(dns_cache_resolve(c, "a..b", &ss, NULL) == EINVAL);
    CHECK(dns_cache_resolve(c, "bad host", &ss, NULL) == EINVAL);
    CHECK(dns_cache_resolve(c, longname, &ss, NULL) == ENAMETOOLONG);

    dns_cache_config_t cfg;
    dns_cache_config_init(&cfg);
    cfg.capacity = 0;
    dns_cache_t *bad = NULL;
    CHECK(dns_cache_create(&cfg, &bad) == EINVAL && bad == NULL);
    dns_cache_destroy(c);
}

static void test_numeric_literal(void)
{
    dns_cache_t *c = make(NULL);
    struct sockaddr_storage ss;
    socklen_t len = 0;
    char buf[64];

    CHECK(dns_cache_resolve(c, "192.0.2.7", &ss, &len) == 0);
    CHECK(ss.ss_family == AF_INET && len == sizeof(struct sockaddr_in));
    CHECK(strcmp(fmt(&ss, buf, sizeof(buf)), "192.0.2.7") == 0);
    /* cache is AF_INET only */
    CHECK(dns_cache_resolve(c, "2001:db8::1", &ss, &len) == ENOTFOUND);

    dns_cache_stats_t st;
    dns_cache_get_stats(c, &st);
    CHECK(st.entries == 0 && st.resolves_started == 0);
    dns_cache_destroy(c);
}

static void test_miss_then_hit(void)
{
    dns_cache_t *c = make(NULL);
    struct sockaddr_storage ss;
    socklen_t len = 0;
    char buf[64];

    CHECK(dns_cache_resolve(c, "localhost", &ss, &len) == EAGAIN);
    CHECK(wait_resolve(c, "localhost", &ss, 5000) == 0);
    CHECK(dns_cache_resolve(c, "localhost", &ss, &len) == 0);
    CHECK(len == sizeof(struct sockaddr_in));
    CHECK(strcmp(fmt(&ss, buf, sizeof(buf)), "127.0.0.1") == 0);

    /* case-insensitive, trailing dot ignored: same entry, immediate hit */
    CHECK(dns_cache_resolve(c, "LocalHost.", &ss, &len) == 0);

    dns_cache_stats_t st;
    dns_cache_get_stats(c, &st);
    CHECK(st.entries == 1);
    CHECK(st.resolves_started == 1 && st.resolves_ok == 1);
    dns_cache_destroy(c);
}

static void test_nxdomain(void)
{
    dns_cache_t *c = make(NULL);
    struct sockaddr_storage ss;
    int r = wait_resolve(c, NX, &ss, 15000);
    if (r != ENOTFOUND) {
        have_public_dns = 0;
        dns_cache_destroy(c);
        SKIP("no NXDOMAIN answer (public DNS unreachable?)");
    }
    have_public_dns = 1;
    CHECK(dns_cache_resolve(c, NX, &ss, NULL) == ENOTFOUND);
    dns_cache_stats_t st;
    dns_cache_get_stats(c, &st);
    CHECK(st.resolves_nxdomain == 1 && st.notfound >= 2);
    dns_cache_destroy(c);
}

static void neg_short(dns_cache_config_t *cfg) { cfg->negative_ttl_ms = 200; }

static void test_negative_expiry(void)
{
    if (have_public_dns != 1)
        SKIP("needs public DNS");
    dns_cache_t *c = make(neg_short);
    struct sockaddr_storage ss;
    CHECK(wait_resolve(c, NX, &ss, 15000) == ENOTFOUND);
    sleep_ms(400);
    /* negative entry dropped: starts over */
    CHECK(dns_cache_resolve(c, NX, &ss, NULL) == EAGAIN);
    CHECK(wait_resolve(c, NX, &ss, 15000) == ENOTFOUND);
    dns_cache_destroy(c);
}

static void test_rotation(void)
{
    if (have_public_dns != 1)
        SKIP("needs public DNS");
    /* one.one.one.one has two A records (1.1.1.1, 1.0.0.1) */
    const char *host = "one.one.one.one";
    dns_cache_t *c = make(NULL);
    struct sockaddr_storage all[16], ss;
    size_t n = 0;
    int r = wait_resolve(c, host, &ss, 15000);
    if (r != 0) {
        dns_cache_destroy(c);
        SKIP("could not resolve one.one.one.one");
    }
    CHECK(dns_cache_resolve_all(c, host, all, 16, &n) == 0);
    if (n < 2) {
        dns_cache_destroy(c);
        SKIP("name has a single address here");
    }

    /* find where the rotation currently is, then expect a strict cycle */
    char a[64], b[64];
    CHECK(dns_cache_resolve(c, host, &ss, NULL) == 0);
    size_t pos = n;
    for (size_t i = 0; i < n; i++)
        if (strcmp(fmt(&ss, a, sizeof(a)), fmt(&all[i], b, sizeof(b))) == 0)
            pos = i;
    CHECK(pos < n);
    for (size_t k = 1; k <= 3 * n; k++) {
        CHECK(dns_cache_resolve(c, host, &ss, NULL) == 0);
        CHECK(strcmp(fmt(&ss, a, sizeof(a)), fmt(&all[(pos + k) % n], b, sizeof(b))) == 0);
    }
    dns_cache_destroy(c);
}

static void ttl_short(dns_cache_config_t *cfg)
{
    cfg->ttl_ms = 300;
    cfg->refresh_ahead_ms = 100;
}

static void test_ttl_refresh(void)
{
    dns_cache_t *c = make(ttl_short);
    struct sockaddr_storage ss;
    CHECK(wait_resolve(c, "localhost", &ss, 5000) == 0);
    /* for > 3 TTLs the entry must stay answerable: refreshed before expiry */
    for (int i = 0; i < 120; i++) {
        CHECK(dns_cache_resolve(c, "localhost", &ss, NULL) == 0);
        sleep_ms(10);
    }
    dns_cache_stats_t st;
    dns_cache_get_stats(c, &st);
    CHECK(st.refreshes >= 4);
    CHECK(st.resolves_ok >= 5);
    CHECK(st.entries == 1);
    dns_cache_destroy(c);
}

static void ttl_idle(dns_cache_config_t *cfg)
{
    cfg->ttl_ms = 200;
    cfg->refresh_ahead_ms = 50;
    cfg->idle_timeout_ms = 100;
    cfg->stale_grace_ms = 0;
}

static void test_idle_expiry(void)
{
    dns_cache_t *c = make(ttl_idle);
    struct sockaddr_storage ss;
    CHECK(wait_resolve(c, "localhost", &ss, 5000) == 0);
    sleep_ms(500); /* not queried: must expire instead of being refreshed */
    dns_cache_stats_t st;
    dns_cache_get_stats(c, &st);
    CHECK(st.entries == 0 && st.refreshes == 0);
    CHECK(dns_cache_resolve(c, "localhost", &ss, NULL) == EAGAIN);
    dns_cache_destroy(c);
}

static void cap2(dns_cache_config_t *cfg) { cfg->capacity = 2; }

static void test_lru_eviction(void)
{
    if (have_public_dns != 1)
        SKIP("needs public DNS (uses NXDOMAIN names as extra entries)");
    dns_cache_t *c = make(cap2);
    struct sockaddr_storage ss;
    const char *a = "localhost", *b = "lru-b.invalid", *d = "lru-c.invalid";

    CHECK(wait_resolve(c, a, &ss, 5000) == 0);
    sleep_ms(5);
    CHECK(wait_resolve(c, b, &ss, 15000) == ENOTFOUND);
    sleep_ms(5);
    CHECK(dns_cache_resolve(c, a, &ss, NULL) == 0);   /* a is now MRU */
    sleep_ms(5);

    CHECK(wait_resolve(c, d, &ss, 15000) == ENOTFOUND); /* evicts b */
    dns_cache_stats_t st;
    dns_cache_get_stats(c, &st);
    CHECK(st.evictions == 1 && st.entries == 2);
    CHECK(dns_cache_resolve(c, a, &ss, NULL) == 0);     /* a survived */
    CHECK(dns_cache_resolve(c, b, &ss, NULL) == EAGAIN);/* b was evicted */
    dns_cache_destroy(c);
}

/* -------- concurrency: many data-path threads, tiny TTL, small LRU -------- */

struct stress { dns_cache_t *c; _Atomic int stop; _Atomic long ok, again, other; };

static void *stress_thread(void *arg)
{
    struct stress *s = arg;
    static const char *names[] = {
        "localhost", "LOCALHOST", "localhost.", "127.0.0.1",
        "stress-1.invalid", "stress-2.invalid", "stress-3.invalid",
    };
    unsigned seed = (unsigned)(uintptr_t)pthread_self();
    while (!atomic_load(&s->stop)) {
        struct sockaddr_storage ss;
        const char *h = names[rand_r(&seed) % (sizeof(names) / sizeof(names[0]))];
        int r = dns_cache_resolve(s->c, h, &ss, NULL);
        if (r == 0)
            atomic_fetch_add(&s->ok, 1);
        else if (r == EAGAIN)
            atomic_fetch_add(&s->again, 1);
        else if (r != ENOTFOUND)
            atomic_fetch_add(&s->other, 1);
    }
    return NULL;
}

static void stress_cfg(dns_cache_config_t *cfg)
{
    cfg->capacity = 3;
    cfg->ttl_ms = 50;
    cfg->refresh_ahead_ms = 20;
    cfg->negative_ttl_ms = 30;
    cfg->retry_ms = 20;
    cfg->queue_size = 8;
}

static void test_concurrency(void)
{
    struct stress s = { .c = make(stress_cfg) };
    pthread_t th[8];
    for (int i = 0; i < 8; i++)
        pthread_create(&th[i], NULL, stress_thread, &s);
    sleep_ms(1500);
    atomic_store(&s.stop, 1);
    for (int i = 0; i < 8; i++)
        pthread_join(th[i], NULL);

    dns_cache_stats_t st;
    dns_cache_get_stats(s.c, &st);
    printf("  %ld hits, %ld EAGAIN; %llu resolutions, %llu refreshes, %llu evictions\n",
           atomic_load(&s.ok), atomic_load(&s.again),
           (unsigned long long)st.resolves_started, (unsigned long long)st.refreshes,
           (unsigned long long)st.evictions);
    CHECK(atomic_load(&s.other) == 0);
    CHECK(atomic_load(&s.ok) > 0);
    CHECK(st.entries <= 3);
    dns_cache_destroy(s.c); /* may have resolutions in flight */
}

static void test_destroy_inflight(void)
{
    /* destroy right after scheduling many jobs */
    dns_cache_t *c = make(NULL);
    struct sockaddr_storage ss;
    char name[64];
    for (int i = 0; i < 50; i++) {
        snprintf(name, sizeof(name), "inflight-%d.invalid", i);
        dns_cache_resolve(c, name, &ss, NULL);
    }
    dns_cache_resolve(c, "localhost", &ss, NULL);
    sleep_ms(2);
    dns_cache_destroy(c);
}

int main(int argc, char **argv)
{
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "invalid_args",     test_invalid_args },
        { "numeric_literal",  test_numeric_literal },
        { "miss_then_hit",    test_miss_then_hit },
        { "nxdomain",         test_nxdomain },
        { "negative_expiry",  test_negative_expiry },
        { "rotation",         test_rotation },
        { "ttl_refresh",      test_ttl_refresh },
        { "idle_expiry",      test_idle_expiry },
        { "lru_eviction",     test_lru_eviction },
        { "concurrency",      test_concurrency },
        { "destroy_inflight", test_destroy_inflight },
    };
    size_t n = sizeof(tests) / sizeof(tests[0]);
    for (size_t i = 0; i < n; i++) {
        if (argc > 1) { /* optional filter: run only the named tests */
            int want = 0;
            for (int a = 1; a < argc; a++)
                want |= strcmp(argv[a], tests[i].name) == 0;
            if (!want)
                continue;
        }
        int before = failures;
        printf("[%2zu/%zu] %s\n", i + 1, n, tests[i].name);
        fflush(stdout);
        tests[i].fn();
        if (failures != before)
            printf("  -> FAILED\n");
    }
    printf("\n%s: %d failure(s), %d skipped\n", failures ? "FAILED" : "OK", failures, skipped);
    return failures ? 1 : 0;
}
