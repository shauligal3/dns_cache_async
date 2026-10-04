/*
 * dnsc_resolve - tiny demo of libdnscache.
 *
 *   dnsc_resolve [-n rounds] [-i interval_ms] host...
 *
 * Every interval, queries each host once (as a data-path thread would) and
 * prints the answer: an address, "EAGAIN" while resolving, or "ENOTFOUND".
 */
#define _GNU_SOURCE
#include "dns_cache.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int rounds = 5, interval = 200, opt;
    while ((opt = getopt(argc, argv, "n:i:")) != -1) {
        if (opt == 'n')
            rounds = atoi(optarg);
        else if (opt == 'i')
            interval = atoi(optarg);
        else
            goto usage;
    }
    if (optind >= argc)
        goto usage;

    dns_cache_t *c;
    int r = dns_cache_create(NULL, &c);
    if (r) {
        fprintf(stderr, "dns_cache_create: %s\n", dns_cache_strerror(r));
        return 1;
    }

    for (int round = 0; round < rounds; round++) {
        for (int i = optind; i < argc; i++) {
            struct sockaddr_storage ss;
            char buf[INET6_ADDRSTRLEN] = "";
            r = dns_cache_resolve(c, argv[i], &ss, NULL);
            if (r == 0) {
                if (ss.ss_family == AF_INET)
                    inet_ntop(AF_INET, &((struct sockaddr_in *)&ss)->sin_addr, buf, sizeof(buf));
                else
                    inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&ss)->sin6_addr, buf, sizeof(buf));
            }
            printf("round %d  %-30s %s\n", round, argv[i],
                   r == 0 ? buf : r == EAGAIN ? "EAGAIN" : r == ENOTFOUND ? "ENOTFOUND"
                                              : dns_cache_strerror(r));
        }
        struct timespec ts = { interval / 1000, (long)(interval % 1000) * 1000000L };
        nanosleep(&ts, NULL);
    }

    dns_cache_stats_t st;
    dns_cache_get_stats(c, &st);
    printf("hits=%llu misses=%llu notfound=%llu resolutions=%llu\n",
           (unsigned long long)st.hits, (unsigned long long)st.misses,
           (unsigned long long)st.notfound, (unsigned long long)st.resolves_started);
    dns_cache_destroy(c);
    return 0;

usage:
    fprintf(stderr, "usage: %s [-n rounds] [-i interval_ms] host...\n", argv[0]);
    return 2;
}
