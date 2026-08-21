/* ===========================================================================
 * host_net.c - uc_net.h answered by a hosted OS.
 *
 * Sockets from the platform, TLS from BearSSL, and the trust anchors from
 * upstream's tls_ca.c - the SAME fourteen roots UnoDOS validates against,
 * compiled here unchanged.  That file is generated, self-contained and
 * includes only bearssl.h, so sharing it costs nothing and buys the one thing
 * worth having: a certificate that validates on the device validates here, and
 * one that does not, does not.  Two independently curated root stores would
 * have been a bug generator with no upside.
 *
 * THE ENGINE IS DRIVEN BY HAND, not through br_sslio_*.  Those helpers take a
 * read and a write callback and block inside them until the operation
 * completes, which is exactly what this seam exists not to do.  The loop in
 * pump() below is the low-level BearSSL contract instead: ask the engine what
 * it wants, give it exactly that much, never wait.
 *
 * Everything here is non-blocking, including the TCP connect, so uc_tls_open()
 * can return during the same frame the user pressed a key in.
 * ======================================================================== */
#include "host.h"
#include "uc_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <bcrypt.h>
  typedef SOCKET sockfd;
  #define BAD_SOCK        INVALID_SOCKET
  #define sock_close      closesocket
  #define sock_errno()    WSAGetLastError()
  #define SOCK_WOULDBLOCK WSAEWOULDBLOCK
  #define SOCK_INPROGRESS WSAEWOULDBLOCK
#else
  #include <errno.h>
  #include <fcntl.h>
  #include <netdb.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <sys/socket.h>
  #include <unistd.h>
  typedef int sockfd;
  #define BAD_SOCK        (-1)
  #define sock_close      close
  #define sock_errno()    errno
  #define SOCK_WOULDBLOCK EAGAIN
  #define SOCK_INPROGRESS EINPROGRESS
#endif

#include "bearssl.h"
#include "tls_ca.h"

/* ---- state ---------------------------------------------------------------- */

enum { ST_CONNECTING, ST_RUNNING, ST_EOF, ST_FAILED };

struct uc_conn {
    sockfd fd;
    int    state;
    int    uc_err;                  /* a UC_NET_* code once state is ST_FAILED */
    br_ssl_client_context   sc;
    br_x509_minimal_context xc;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
};

static int g_open_err;
static int g_started;

/* ---- the OS's random source ----------------------------------------------- */

/* Fill `buf` from the platform CSPRNG.  Returns 1 on success.  There is no
 * fallback and there must not be one: a seed we invented is worse than a
 * refusal, because a refusal is visible.  pc64 reaches the same conclusion by
 * a longer road (RDRAND, else health-tested timing jitter, else refuse). */
static int os_random(void *buf, size_t len)
{
#ifdef _WIN32
    return BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len,
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    size_t got;
    if (!f) return 0;
    got = fread(buf, 1, len, f);
    fclose(f);
    return got == len;
#endif
}

int uc_net_entropy_ok(void)
{
    unsigned char probe[8];
    return os_random(probe, sizeof probe);
}

/* BearSSL's ssl_engine.c asks for a system seeder before it will reset a
 * client, and there is not one: upstream's bearssl/src/config.h forces
 * BR_USE_URANDOM and BR_USE_WIN32_RAND to 0, so sysrng.c would compile to
 * nothing useful and is excluded from the build on both platforms.  pc64
 * answers this the same way, in tls.c, for the same reason.
 *
 * Saying "none" honestly is safe because uc_tls_open() injects entropy
 * explicitly before reset, and REFUSES if it cannot.  Do not be tempted to
 * turn the config flags on instead: that file is upstream's and shared, and a
 * hosted build quietly needing different crypto configuration from the device
 * is how the two stop being the same TLS. */
br_prng_seeder br_prng_seeder_system(const char **name)
{
    if (name) *name = "none";
    return 0;
}

/* ---- the link ------------------------------------------------------------- */

/* The OS owns the link here, so there is nothing to bring up.  Winsock still
 * needs starting once, and this is the only call guaranteed to run before any
 * socket is made. */
int uc_net_up(void)
{
#ifdef _WIN32
    if (!g_started) {
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return 0;
        g_started = 1;
    }
#else
    g_started = 1;
#endif
    return 1;
}

/* Nothing to pump: the kernel is already moving these packets.  The call
 * exists so the CORE does not have to know that, and so the same core loop
 * runs unchanged on a machine where it is the whole transport. */
void uc_net_pump(void) { }

static int parse_quad(const char *s, unsigned char ip[4])
{
    int part = 0, val = 0, digits = 0;
    for (;; s++) {
        if (*s >= '0' && *s <= '9') {
            val = val * 10 + (*s - '0');
            if (++digits > 3 || val > 255) return 0;
        } else if (*s == '.' || *s == 0) {
            if (!digits || part > 3) return 0;
            ip[part++] = (unsigned char)val;
            val = 0; digits = 0;
            if (*s == 0) break;
        } else {
            return 0;
        }
    }
    return part == 4;
}

/* Is this string made only of digits and dots?  Then it was MEANT as an
 * address, and if parse_quad rejected it, it is a malformed one.
 *
 * This guard is not paranoia, it is a measured result: getaddrinfo("1.2.3")
 * succeeds on glibc and yields 1.2.0.3, because the historical inet_aton
 * shorthand treats the last part as a 24-bit remainder.  So a strict parser in
 * front of a permissive resolver is no protection at all - the resolver simply
 * accepts what the parser turned down, and the caller connects to a host it
 * never named.  tools/net_test.sh caught exactly this. */
static int looks_numeric(const char *s)
{
    for (; *s; s++)
        if ((*s < '0' || *s > '9') && *s != '.') return 0;
    return 1;
}

int uc_net_resolve(const char *host, unsigned char ip[4])
{
    struct addrinfo hints, *res = 0;
    int rc;

    if (!host || !*host) return 0;
    if (parse_quad(host, ip)) return 1;
    if (looks_numeric(host)) return 0;      /* a broken address, not a name */
    if (!uc_net_up()) return 0;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;              /* the seam is IPv4, like pc64's */
    hints.ai_socktype = SOCK_STREAM;
    rc = getaddrinfo(host, 0, &hints, &res);
    if (rc != 0 || !res) return 0;
    memcpy(ip, &((struct sockaddr_in *)res->ai_addr)->sin_addr, 4);
    freeaddrinfo(res);
    return 1;
}

/* ---- a session ------------------------------------------------------------ */

static int set_nonblocking(sockfd fd)
{
#ifdef _WIN32
    u_long on = 1;
    return ioctlsocket(fd, FIONBIO, &on) == 0;
#else
    int fl = fcntl(fd, F_GETFL, 0);
    return fl != -1 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) != -1;
#endif
}

/* BearSSL wants the date as days since 1 January of year 0 plus seconds into
 * the day, because that is what an X.509 validity window is compared against.
 * 719528 is the day count from year 0 to the Unix epoch.
 *
 * WITHOUT THIS, EVERY CERTIFICATE IS EXPIRED.  br_x509_minimal starts at time
 * zero, so a validity window that began in 2024 has not started yet, and the
 * failure arrives as BR_ERR_X509_EXPIRED - which reads as a server problem and
 * is a client one. */
static void set_validation_time(br_x509_minimal_context *xc)
{
    time_t now = time(0);
    uint32_t days = (uint32_t)(now / 86400) + 719528;
    uint32_t secs = (uint32_t)(now % 86400);
    br_x509_minimal_set_time(xc, days, secs);
}

uc_conn *uc_tls_open(const unsigned char ip[4], unsigned short port,
                     const char *sni)
{
    struct sockaddr_in sa;
    unsigned char seed[32];
    uc_conn *c;
    int one = 1;

    if (!uc_net_entropy_ok()) { g_open_err = UC_NET_ENOENTROPY; return 0; }
    if (!uc_net_up())         { g_open_err = UC_NET_ENOLINK;    return 0; }

    c = (uc_conn *)calloc(1, sizeof *c);
    if (!c) { g_open_err = UC_NET_ENOMEM; return 0; }
    c->fd = BAD_SOCK;

    c->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (c->fd == BAD_SOCK) { g_open_err = UC_NET_ERR; free(c); return 0; }
    if (!set_nonblocking(c->fd)) {
        sock_close(c->fd); free(c); g_open_err = UC_NET_ERR; return 0;
    }
    /* A TLS record handed over in two writes is two packets without this, and
     * the second waits on the first's ACK.  It costs a request per round trip
     * on exactly the small writes a request header is made of. */
    setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);

    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    memcpy(&sa.sin_addr, ip, 4);

    if (connect(c->fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        int e = sock_errno();
        if (e != SOCK_INPROGRESS && e != SOCK_WOULDBLOCK) {
            sock_close(c->fd); free(c); g_open_err = UC_NET_ERR; return 0;
        }
    }

    br_ssl_client_init_full(&c->sc, &c->xc, uno_tls_tas, uno_tls_tas_num);
    set_validation_time(&c->xc);
    br_ssl_engine_set_buffer(&c->sc.eng, c->iobuf, sizeof c->iobuf, 1);

    /* Seed explicitly, and REFUSE if we cannot.  br_prng_seeder_system() below
     * reports "none", so this injection is the only entropy the engine will
     * ever get - carrying on without it would hand BearSSL a client it cannot
     * reset, and the failure would arrive later as a bare BR_ERR far from the
     * machine that actually has no random source. */
    if (!os_random(seed, sizeof seed)) {
        sock_close(c->fd); free(c);
        g_open_err = UC_NET_ENOENTROPY;
        return 0;
    }
    br_ssl_engine_inject_entropy(&c->sc.eng, seed, sizeof seed);

    if (!br_ssl_client_reset(&c->sc, sni, 0)) {
        sock_close(c->fd); free(c); g_open_err = UC_NET_ERR; return 0;
    }

    c->state = ST_CONNECTING;
    g_open_err = 0;
    return c;
}

int uc_net_open_error(void) { return g_open_err; }

/* Has the non-blocking connect finished?  1 yes, 0 not yet, <0 it failed. */
static int connect_done(uc_conn *c)
{
    fd_set w, e;
    struct timeval tv;
    int err = 0, rc;
    socklen_t len = sizeof err;

    FD_ZERO(&w); FD_SET(c->fd, &w);
    FD_ZERO(&e); FD_SET(c->fd, &e);
    tv.tv_sec = 0; tv.tv_usec = 0;          /* a poll, never a wait */

    rc = select((int)c->fd + 1, 0, &w, &e, &tv);
    if (rc <= 0) return 0;
    if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, (char *)&err, &len) != 0)
        return -1;
    return err == 0 ? 1 : -1;
}

/* One turn of the BearSSL contract: ask the engine what it wants next and give
 * it exactly that, until it wants something the socket cannot supply right
 * now.  Every branch is bounded by what the engine asked for, so this cannot
 * spin: an engine that wants nothing more returns immediately. */
static void pump(uc_conn *c)
{
    for (;;) {
        unsigned st = br_ssl_engine_current_state(&c->sc.eng);
        unsigned char *buf;
        size_t len;
        int n;

        if (st & BR_SSL_CLOSED) {
            int err = br_ssl_engine_last_error(&c->sc.eng);
            if (err == BR_ERR_OK) {
                c->state = ST_EOF;
            } else {
                c->state = ST_FAILED;
                switch (err) {
                case BR_ERR_X509_EXPIRED:
                case BR_ERR_X509_NOT_TRUSTED:
                case BR_ERR_X509_BAD_SERVER_NAME:
                    c->uc_err = UC_NET_ETRUST; break;
                default:
                    c->uc_err = UC_NET_ERR; break;
                }
            }
            return;
        }

        /* Records the engine has produced and wants on the wire. */
        if (st & BR_SSL_SENDREC) {
            buf = br_ssl_engine_sendrec_buf(&c->sc.eng, &len);
            n = (int)send(c->fd, (const char *)buf, (int)len, 0);
            if (n > 0) { br_ssl_engine_sendrec_ack(&c->sc.eng, (size_t)n); continue; }
            if (n < 0 && sock_errno() != SOCK_WOULDBLOCK) {
                c->state = ST_FAILED; c->uc_err = UC_NET_ERR;
            }
            return;
        }

        /* Room the engine has for records off the wire. */
        if (st & BR_SSL_RECVREC) {
            buf = br_ssl_engine_recvrec_buf(&c->sc.eng, &len);
            n = (int)recv(c->fd, (char *)buf, (int)len, 0);
            if (n > 0) { br_ssl_engine_recvrec_ack(&c->sc.eng, (size_t)n); continue; }
            if (n == 0) {
                /* The peer hung up without a close_notify.  That is a
                 * truncation, not a clean end, and treating it as EOF is how a
                 * caller ends up parsing a half-received body as a whole one. */
                c->state = ST_FAILED; c->uc_err = UC_NET_ERR;
                return;
            }
            if (sock_errno() != SOCK_WOULDBLOCK) {
                c->state = ST_FAILED; c->uc_err = UC_NET_ERR;
            }
            return;
        }

        return;                            /* it wants application data now */
    }
}

int uc_tls_poll(uc_conn *c)
{
    if (!c) return UC_NET_ERR;

    if (c->state == ST_CONNECTING) {
        int r = connect_done(c);
        if (r == 0) return UC_NET_PENDING;
        if (r < 0)  { c->state = ST_FAILED; c->uc_err = UC_NET_ERR;
                      return UC_NET_ERR; }
        c->state = ST_RUNNING;
    }

    if (c->state == ST_RUNNING) pump(c);

    if (c->state == ST_FAILED) return c->uc_err;
    if (c->state == ST_EOF)    return UC_NET_EOF;

    /* SENDAPP means the handshake is behind us and the engine will take a
     * request.  Anything else at this point is still handshaking. */
    return (br_ssl_engine_current_state(&c->sc.eng) & BR_SSL_SENDAPP)
           ? UC_NET_READY : UC_NET_PENDING;
}

int uc_tls_send(uc_conn *c, const void *data, int len)
{
    unsigned char *buf;
    size_t room;
    int n;

    if (!c || len < 0) return UC_NET_ERR;
    if (c->state == ST_FAILED) return c->uc_err;
    if (c->state != ST_RUNNING) return 0;
    if (len == 0) return 0;

    if (!(br_ssl_engine_current_state(&c->sc.eng) & BR_SSL_SENDAPP)) return 0;

    buf = br_ssl_engine_sendapp_buf(&c->sc.eng, &room);
    if (!buf || room == 0) return 0;
    n = (size_t)len < room ? len : (int)room;
    memcpy(buf, data, (size_t)n);
    br_ssl_engine_sendapp_ack(&c->sc.eng, (size_t)n);

    /* Hand it to the engine now rather than at the next poll.  Without this a
     * request sits in the buffer until something else happens, which on a
     * quiet connection is "until the server times out". */
    br_ssl_engine_flush(&c->sc.eng, 0);
    pump(c);
    return n;
}

int uc_tls_recv(uc_conn *c, void *buf, int cap)
{
    unsigned char *src;
    size_t have;
    int n;

    if (!c || cap <= 0) return UC_NET_ERR;
    if (c->state == ST_FAILED) return c->uc_err;
    if (c->state == ST_RUNNING) pump(c);
    if (c->state == ST_FAILED) return c->uc_err;

    if (!(br_ssl_engine_current_state(&c->sc.eng) & BR_SSL_RECVAPP)) return 0;

    src = br_ssl_engine_recvapp_buf(&c->sc.eng, &have);
    if (!src || have == 0) return 0;
    n = (size_t)cap < have ? cap : (int)have;
    memcpy(buf, src, (size_t)n);
    br_ssl_engine_recvapp_ack(&c->sc.eng, (size_t)n);
    return n;
}

void uc_tls_free(uc_conn *c)
{
    if (!c) return;
    if (c->state == ST_RUNNING) {
        br_ssl_engine_close(&c->sc.eng);
        pump(c);                            /* one try at a clean close_notify */
    }
    if (c->fd != BAD_SOCK) sock_close(c->fd);
    free(c);
}

const char *uc_net_error(uc_conn *c)
{
    if (!c) return "no connection";
    switch (br_ssl_engine_last_error(&c->sc.eng)) {
    case BR_ERR_OK:
        return c->state == ST_FAILED ? "the connection failed" : "no error";
    case BR_ERR_X509_EXPIRED:
        return "the server's certificate has expired, or this machine's "
               "clock is wrong";
    case BR_ERR_X509_NOT_TRUSTED:
        return "the server's certificate is not signed by a trusted authority";
    case BR_ERR_X509_BAD_SERVER_NAME:
        return "the server's certificate is for a different name";
    default:
        return "the secure connection failed";
    }
}
