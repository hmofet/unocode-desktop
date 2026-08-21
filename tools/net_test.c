/* ===========================================================================
 * net_test.c - host_net.c's contract, driven directly.
 *
 * The check that matters most here is the one that asserts a FAILURE: given a
 * server presenting a certificate signed by nobody, uc_tls_open() must end in
 * UC_NET_ETRUST and must never reach UC_NET_READY.  Everything else in this
 * seam is visible when it breaks - a connection that does not connect is
 * obvious.  A connection that connects to ANYBODY is not visible at all: it
 * works perfectly, all the way to whoever is on the other end.
 *
 * Run through tools/net_test.sh, which supplies the untrusted server.  Without
 * an address argument the TLS checks are skipped and the rest still run.
 *
 *   ./net_test                 the offline checks only
 *   ./net_test 127.0.0.1 4443  + the untrusted-certificate check
 * ======================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
  #include <windows.h>
#else
  #include <time.h>
#endif

#include "uc_net.h"

static int fails;

static void ok(int cond, const char *what)
{
    printf("  %-5s %s\n", cond ? "ok" : "FAIL", what);
    if (!cond) fails++;
}

/* ---- resolution ----------------------------------------------------------- */

static void t_resolve(void)
{
    unsigned char ip[4];

    puts("1. addresses and names");

    memset(ip, 0, 4);
    ok(uc_net_resolve("127.0.0.1", ip) && ip[0] == 127 && ip[1] == 0 &&
       ip[2] == 0 && ip[3] == 1, "a dotted quad is taken literally");

    memset(ip, 0, 4);
    ok(uc_net_resolve("10.0.2.100", ip) && ip[0] == 10 && ip[3] == 100,
       "and needs no resolver to do it");

    /* Each of these would be accepted by an atoi-and-walk parser, and each
     * would connect somewhere the caller did not name.  A name that merely
     * LOOKS like an address must go to DNS or fail, never be guessed at. */
    ok(!uc_net_resolve("1.2.3.4.5", ip),   "five parts is not an address");
    ok(!uc_net_resolve("1.2.3", ip),       "three parts is not an address");
    ok(!uc_net_resolve("256.1.1.1", ip),   "a part over 255 is not an address");
    ok(!uc_net_resolve("1.2.3.4x", ip),    "trailing rubbish is not an address");
    ok(!uc_net_resolve("1.2..4", ip),      "an empty part is not an address");
    ok(!uc_net_resolve("", ip),            "the empty string resolves to nothing");
    ok(!uc_net_resolve(0, ip),             "and neither does NULL");
}

/* ---- the machine ---------------------------------------------------------- */

static void t_machine(void)
{
    puts("2. what this machine can do");
    ok(uc_net_up() == 1, "the link is up (the OS owns it here)");
    ok(uc_net_entropy_ok() == 1, "there is a usable random source");
}

/* ---- the one that matters ------------------------------------------------- */

static int drive(uc_conn *c, int *last)
{
    int i, r = UC_NET_PENDING;
    /* 4000 turns with a 1 ms sleep is four seconds of wall clock, which is far
     * more than a loopback handshake needs and still bounded - a test that can
     * hang is a test that will, on the one machine nobody is watching. */
    for (i = 0; i < 4000; i++) {
        uc_net_pump();
        r = uc_tls_poll(c);
        *last = r;
        if (r != UC_NET_PENDING) return r;
#ifdef _WIN32
        Sleep(1);
#else
        {   struct timespec ts; ts.tv_sec = 0; ts.tv_nsec = 1000000;
            nanosleep(&ts, 0); }
#endif
    }
    return r;
}

static void t_untrusted(const char *host, int port)
{
    unsigned char ip[4];
    uc_conn *c;
    int r, last = UC_NET_PENDING;

    puts("3. a certificate signed by nobody");

    if (!uc_net_resolve(host, ip)) { ok(0, "resolved the test server"); return; }

    c = uc_tls_open(ip, (unsigned short)port, "unocode.test");
    if (!c) { ok(0, "opened a handle to the test server"); return; }

    r = drive(c, &last);

    ok(r != UC_NET_READY,
       "the handshake did NOT complete against an untrusted certificate");
    ok(r == UC_NET_ETRUST,
       "and it failed as a TRUST problem, not a generic one");
    ok(strstr(uc_net_error(c), "trusted") != 0,
       "with a message that names the reason");
    printf("        (%s)\n", uc_net_error(c));

    uc_tls_free(c);
}

/* A port with nothing behind it must fail, and must fail promptly.  This is
 * the other half of "never blocks": a refused connection that spun here would
 * look exactly like a working one that is slow. */
static void t_dead_port(const char *host, int port)
{
    unsigned char ip[4];
    uc_conn *c;
    int r, last = UC_NET_PENDING;

    puts("4. a port with nothing behind it");
    if (!uc_net_resolve(host, ip)) { ok(0, "resolved"); return; }

    c = uc_tls_open(ip, (unsigned short)port, "unocode.test");
    if (!c) { ok(1, "refused at open, which is also a valid answer"); return; }
    r = drive(c, &last);
    ok(r < 0, "it failed rather than connecting or hanging");
    uc_tls_free(c);
}

/* The real thing, against the real endpoint.  Deliberately NOT in the gate:
 * it needs the internet, so in a gate it would fail on a train and teach
 * everyone to ignore a red result.  Run it by hand when the trust store or the
 * handshake changes.
 *
 *   ./net_test --live
 *
 * It stops at the handshake and sends no request - the point is the CA path,
 * and an unauthenticated POST to /v1/messages proves nothing extra. */
static void t_live(void)
{
    unsigned char ip[4];
    uc_conn *c;
    int r, last = UC_NET_PENDING;

    puts("5. the real endpoint");
    if (!uc_net_resolve("api.anthropic.com", ip)) {
        ok(0, "resolved api.anthropic.com (is there internet?)");
        return;
    }
    printf("        %u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3]);

    c = uc_tls_open(ip, 443, "api.anthropic.com");
    if (!c) { ok(0, "opened a handle"); return; }

    r = drive(c, &last);
    ok(r == UC_NET_READY, "the handshake completed against the bundled roots");
    if (r != UC_NET_READY) printf("        (%s)\n", uc_net_error(c));
    uc_tls_free(c);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--live") == 0) {
        t_machine();
        t_live();
        printf("\n%s: %d failure(s)\n", fails ? "FAILED" : "PASSED", fails);
        return fails ? 1 : 0;
    }

    t_resolve();
    t_machine();

    if (argc >= 3) {
        t_untrusted(argv[1], atoi(argv[2]));
        t_dead_port(argv[1], atoi(argv[2]) + 1);
    } else {
        puts("3. a certificate signed by nobody");
        puts("  SKIP  no test server given (see tools/net_test.sh)");
    }

    printf("\n%s: %d failure(s)\n", fails ? "FAILED" : "PASSED", fails);
    return fails ? 1 : 0;
}
