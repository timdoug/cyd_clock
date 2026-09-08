#define NTP_TEST_RESOLVER
#include "ntp_host.h"
#include <netdb.h>

static int count4, count6, queries;
static int test_getaddrinfo(const char *host, const char *service,
                            const struct addrinfo *hints, struct addrinfo **out) {
    (void)host; (void)service;
    static struct addrinfo records[4];
    static struct sockaddr_storage addresses[4];
    int n = hints->ai_family == AF_INET6 ? count6 : count4;
    queries++;
    memset(records, 0, sizeof(records));
    memset(addresses, 0, sizeof(addresses));
    for (int i = 0; i < n; i++) {
        addresses[i].ss_family = hints->ai_family;
        if (hints->ai_family == AF_INET6) {
            ((struct sockaddr_in6 *)&addresses[i])->sin6_addr.s6_addr[15] = i+1;
            records[i].ai_addrlen = sizeof(struct sockaddr_in6);
        } else {
            ((struct sockaddr_in *)&addresses[i])->sin_addr.s_addr = htonl(i+1);
            records[i].ai_addrlen = sizeof(struct sockaddr_in);
        }
        records[i].ai_family = hints->ai_family;
        records[i].ai_addr = (struct sockaddr *)&addresses[i];
        records[i].ai_next = i+1 < n ? &records[i+1] : NULL;
    }
    *out = n ? records : NULL;
    return n ? 0 : EAI_NONAME;
}
#define getaddrinfo test_getaddrinfo
#define freeaddrinfo(...) ((void)0)
#define vSemaphoreDelete(...) ((void)0)
#define xSemaphoreCreateBinary() ((void *)1)
#define dns_clear_cache() ((void)0)
#define ERR_OK 0
static int tcpip_callback(void (*cb)(void *), void *arg) { cb(arg); return 0; }
#include "../main/ntp_resolve.c"

int main(void) {
    struct sockaddr_storage out[4];
    for (int prefer = 0; prefer < 2; prefer++) {
        for (count4 = 0; count4 <= 4; count4++) {
            for (count6 = 0; count6 <= 4; count6++) {
                for (int cap = 1; cap <= 4; cap++) {
                    queries = 0;
                    int n = ntp_resolve_host("example.test", prefer, out, cap);
                    int expected = count4+count6 < cap ? count4+count6 : cap;
                    assert(n == expected && queries == 2);
                    if (cap >= 2 && count4 && count6) {
                        assert(out[0].ss_family == (prefer ? AF_INET6 : AF_INET));
                        assert(out[1].ss_family == (prefer ? AF_INET : AF_INET6));
                    }
                }
            }
        }
    }
    queries = 0;
    assert(ntp_resolve_host("192.0.2.1", true, out, 4) == 1);
    assert(out[0].ss_family == AF_INET && queries == 0);
    assert(ntp_resolve_host("2001:db8::1", false, out, 4) == 1);
    assert(out[0].ss_family == AF_INET6 && queries == 0);
    assert(ntp_resolve_host("example.test", true, out, 0) == 0);
    puts("PASS DNS family fallback and capacity limits");
}
