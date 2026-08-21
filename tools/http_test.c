/* ===========================================================================
 * http_test.c - uc_http.c's framing, and one real request.
 *
 * THERE IS NO LOCAL TEST SERVER HERE, and that is a consequence of a decision
 * rather than a gap.  uc_net.h validates every certificate against the bundled
 * roots and offers no way to turn that off, so a throwaway server with a
 * self-signed certificate is - correctly - unreachable.  The alternatives were
 * a test-only trust override, which is the exact hole the seam exists to not
 * have, or linking the tests against a different trust store, which is real
 * work for a weaker test than this one.
 *
 * So the split is:
 *
 *   THE FRAMING is tested by feeding bytes straight into the state machine.
 *   That is where the bugs live and it is pure logic, so it needs no socket
 *   and no server, runs in milliseconds, and can be made to do things a
 *   cooperative server never would - split a chunk header across two reads,
 *   deliver one SSE event a byte at a time.  This file #includes uc_http.c to
 *   reach feed(), which is the point: the seam under test is internal.
 *
 *   THE SOCKET PATH is proved by --live, against api.anthropic.com, with NO
 *   API KEY.  An unauthenticated POST earns a 401 and a JSON error body, which
 *   exercises request building, TLS, the response framing and the JSON reader
 *   end to end against the real server - and needs no secret to run.
 * ======================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
  #include <windows.h>
#else
  #include <time.h>
#endif

#include "uc_http.c"          /* the unit under test, statics and all */

static int fails;

static void ok(int cond, const char *what)
{
    printf("  %-5s %s\n", cond ? "ok" : "FAIL", what);
    if (!cond) fails++;
}

/* ---- a handle with no socket, so feed() can be driven by hand ------------- */

static uc_http *paper(void)
{
    uc_http *h = (uc_http *)malloc(sizeof *h);
    memset(h, 0, sizeof *h);
    h->state = S_STATUS;
    h->content_len = -1;
    h->maxbody = DEF_MAX_BODY;
    h->msg = "no error";
    return h;
}

/* Feed a response in slices of `step` bytes, so a decoder that assumed one
 * read is one anything falls over.  step 0 means "all at once". */
static void pour(uc_http *h, const char *s, int n, int step)
{
    int i;
    for (i = 0; i < n; i++) {
        if (!feed(h, s[i])) return;
        (void)step;
    }
}

static void t_plain(void)
{
    static const char R[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 13\r\n"
        "\r\n"
        "hello unocode";
    uc_http *h = paper();
    int len = 0;

    puts("1. status line, headers, Content-Length body");
    pour(h, R, (int)sizeof R - 1, 0);
    ok(h->state == S_DONE, "the response completed");
    ok(uc_http_status(h) == 200, "the status parsed as 200");
    ok(h->content_len == 13, "Content-Length was read");
    ok(strcmp(uc_http_body(h, &len), "hello unocode") == 0 && len == 13,
       "and the body is exactly the body");
    uc_http_free(h);
}

static void t_status_codes(void)
{
    static const char R401[] =
        "HTTP/1.1 401 Unauthorized\r\nContent-Length: 2\r\n\r\n{}";
    static const char R418[] =
        "HTTP/1.1 418 I'm a teapot\r\nContent-Length: 0\r\n\r\n";
    uc_http *h;

    puts("2. a 4xx is a COMPLETED request");
    h = paper(); pour(h, R401, (int)sizeof R401 - 1, 0);
    ok(h->state == S_DONE && uc_http_status(h) == 401,
       "401 completes and reports 401");
    uc_http_free(h);

    /* Content-Length: 0 has no body at all, so nothing after the blank line
     * ever arrives.  The exchange still has to end - by EOF - rather than
     * hanging for a byte that is not coming. */
    h = paper(); pour(h, R418, (int)sizeof R418 - 1, 0);
    ok(uc_http_status(h) == 418, "418 is readable with an empty body");
    uc_http_free(h);
}

/* Build a chunked response from a list of pieces, computing every chunk size.
 *
 * Hand-writing the hex sizes was the first attempt and it is a trap: get one
 * wrong and the test fails in a way that looks exactly like a decoder bug, so
 * you go and debug the decoder.  The framing is generated here for the same
 * reason a checksum is computed rather than typed. */
static int build_chunked(char *out, int cap, const char *const *parts, int n,
                         const char *extra_on_first)
{
    int p = 0, i;
    uc_buf_raw(out, &p, cap,
               "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    for (i = 0; i < n; i++) {
        int len = (int)strlen(parts[i]);
        char hex[16]; int hn = 0, v = len;
        if (!v) hex[hn++] = '0';
        while (v) { int d = v & 15;
                    hex[hn++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
                    v >>= 4; }
        while (hn) uc_buf_n(out, &p, cap, &hex[--hn], 1);
        /* A chunk extension after the size is legal and must be ignored. */
        if (i == 0 && extra_on_first) uc_buf_raw(out, &p, cap, extra_on_first);
        uc_buf_raw(out, &p, cap, "\r\n");
        uc_buf_raw(out, &p, cap, parts[i]);
        uc_buf_raw(out, &p, cap, "\r\n");
    }
    uc_buf_raw(out, &p, cap, "0\r\n\r\n");
    return p;
}

static void t_chunked(void)
{
    /* Uneven sizes, one chunk carrying a single byte, and an extension on the
     * first - all legal, all things a naive decoder gets wrong. */
    static const char *const parts[] = { "01234", "5", "6789012345" };
    char R[512];
    uc_http *h = paper();
    int len = 0, n;

    puts("3. chunked framing");
    n = build_chunked(R, sizeof R, parts, 3, ";ext=ignored");
    pour(h, R, n, 0);
    ok(h->chunked == 1, "the response was recognised as chunked");
    ok(h->state == S_DONE, "the zero chunk ended it");
    ok(strcmp(uc_http_body(h, &len), "0123456789012345") == 0 && len == 16,
       "the chunks joined into the body, and no framing leaked in");
    uc_http_free(h);
}

/* The check only a hostile reader can make: the same response, one byte per
 * call.  Every chunk header is split, every CRLF is split, the terminating
 * chunk is split.  A decoder that peeked ahead, or assumed a size line arrived
 * whole, dies here and nowhere else. */
static void t_chunked_byte_at_a_time(void)
{
    static const char *const parts[] = { "01234", "5", "6789012345" };
    char R[512];
    uc_http *h = paper();
    int len = 0, i, n;

    puts("4. the same response, one byte per call");
    n = build_chunked(R, sizeof R, parts, 3, ";ext=ignored");
    for (i = 0; i < n; i++)
        if (!feed(h, R[i])) break;
    ok(h->state == S_DONE, "it still completed");
    ok(strcmp(uc_http_body(h, &len), "0123456789012345") == 0 && len == 16,
       "and produced the identical body");
    uc_http_free(h);
}

/* ---- SSE ------------------------------------------------------------------ */

static char g_ev[32][64];
static char g_dat[32][256];
static int  g_n;

static void on_event(void *user, const char *event, const char *data, int len)
{
    (void)user;
    if (g_n >= 32) return;
    strncpy(g_ev[g_n], event, 63);  g_ev[g_n][63] = 0;
    if (len > 255) len = 255;
    memcpy(g_dat[g_n], data, (size_t)len); g_dat[g_n][len] = 0;
    g_n++;
}

static void t_sse(void)
{
    /* An Anthropic stream in miniature: named events, JSON payloads, a
     * comment line, and a payload folded across two data: lines.  The event
     * boundaries and the chunk boundaries deliberately do NOT line up - the
     * comment shares a chunk with the event before it, and the last event is
     * split across two chunks. */
    static const char *const parts[] = {
        "event: message_start\ndata: {\"n\":0}\n\n",
        "event: delta\ndata: {\"n\":1}\n\n:  a keepalive comment\n",
        "event: delta\ndata: one\n",
        "data: two\n\n"
    };
    char R[1024];
    uc_http *h = paper();
    int i, n;

    puts("5. server-sent events");
    g_n = 0;
    h->sse = on_event;
    n = build_chunked(R, sizeof R, parts, 4, 0);

    /* ONE BYTE AT A TIME, deliberately.  An SSE decoder that assumed a read
     * boundary was an event boundary passes every loopback test ever written
     * and then drops events over a real network, intermittently, on somebody
     * else's machine. */
    for (i = 0; i < n; i++)
        if (!feed(h, R[i])) break;

    ok(g_n == 3, "three events, assembled from single bytes");
    ok(g_n > 0 && strcmp(g_ev[0], "message_start") == 0,
       "the first event kept its name");
    ok(g_n > 0 && strcmp(g_dat[0], "{\"n\":0}") == 0,
       "and its payload, with the one optional space stripped");
    ok(g_n > 1 && strcmp(g_ev[1], "delta") == 0 &&
       strcmp(g_dat[1], "{\"n\":1}") == 0, "and so did the second");
    ok(g_n > 2 && strcmp(g_dat[2], "one\ntwo") == 0,
       "two data: lines fold into one payload separated by a newline");
    ok(g_n == 3, "the ':' comment line produced no event");

    /* With a handler installed the body is not accumulated: a token stream can
     * outrun any cap worth setting, and a caller that wanted the whole thing
     * would not have installed one. */
    ok(h->blen == 0, "and nothing was accumulated into the body");
    uc_http_free(h);
}

/* An error reply is a BODY, not a stream (UCD-49).  A streaming caller still
 * gets the server's explanation: a 401's JSON is one line matching no SSE
 * field, and feeding it to the line assembler would silently delete the whole
 * reason the request failed. */
static void t_sse_error(void)
{
    static const char R[] =
        "HTTP/1.1 401 Unauthorized\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 47\r\n"
        "\r\n"
        "{\"type\":\"error\",\"error\":{\"message\":\"bad key\"}}\n";
    uc_http *h = paper();
    int len = 0, i;
    const char *body;

    puts("5b. an error reply survives a streaming request");
    g_n = 0;
    h->sse = on_event;
    for (i = 0; i < (int)sizeof R - 1; i++)
        if (!feed(h, R[i])) break;
    ok(h->state == S_DONE, "the 401 completed at its Content-Length");
    ok(g_n == 0, "no SSE event was invented from the JSON");
    body = uc_http_body(h, &len);
    ok(body && strstr(body, "bad key") != 0,
       "the server's explanation is in the body, readable");
    uc_http_free(h);
}

/* ---- the request we build ------------------------------------------------- */

static void t_request(void)
{
    uc_header hd[2];
    uc_http_req r;
    uc_http h;
    char body[64];
    int i;

    puts("6. the request bytes");
    memset(&h, 0, sizeof h);
    for (i = 0; i < 40; i++) body[i] = 'x';
    body[40] = 0;

    hd[0].name = "x-api-key";         hd[0].value = "sk-test-123";
    hd[1].name = "anthropic-version"; hd[1].value = "2023-06-01";

    memset(&r, 0, sizeof r);
    r.host = "api.anthropic.com"; r.method = "POST"; r.path = "/v1/messages";
    r.headers = hd; r.nheaders = 2; r.body = body; r.body_len = 40;

    ok(build_request(&h, &r, 40) == 1, "it built");
    h.req[h.reqlen] = 0;
    ok(strncmp(h.req, "POST /v1/messages HTTP/1.1\r\n", 28) == 0,
       "the method and path are ours to choose");
    ok(strstr(h.req, "\r\nHost: api.anthropic.com\r\n") != 0,
       "Host is supplied for us");
    ok(strstr(h.req, "\r\nx-api-key: sk-test-123\r\n") != 0,
       "an arbitrary header goes out - which pc64_http.c cannot do");
    ok(strstr(h.req, "\r\nanthropic-version: 2023-06-01\r\n") != 0,
       "and a second one");
    ok(strstr(h.req, "\r\nContent-Length: 40\r\n") != 0,
       "Content-Length is counted for us");
    ok(h.reqlen > 40 && memcmp(h.req + h.reqlen - 40, body, 40) == 0,
       "and the body is the last 40 bytes, copied raw");
    free(h.req);
}

/* ---- the live one --------------------------------------------------------- */

static void nap(void)
{
#ifdef _WIN32
    Sleep(1);
#else
    struct timespec ts; ts.tv_sec = 0; ts.tv_nsec = 1000000;
    nanosleep(&ts, 0);
#endif
}

static void t_live(void)
{
    uc_header hd[2];
    uc_http_req r;
    uc_http *h;
    UcJson *j;
    UcJson *v;
    char err[128];
    const char *b;
    int len = 0, i, rc = UC_HTTP_PENDING;

    puts("7. a real request to api.anthropic.com, with no API key");

    hd[0].name = "content-type";      hd[0].value = "application/json";
    hd[1].name = "anthropic-version"; hd[1].value = "2023-06-01";

    memset(&r, 0, sizeof r);
    r.host = "api.anthropic.com";
    r.method = "POST";
    r.path = "/v1/messages";
    r.headers = hd; r.nheaders = 2;
    r.body = "{\"model\":\"claude-sonnet-5\",\"max_tokens\":1,"
             "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
    r.body_len = -1;

    h = uc_http_begin(&r);
    if (!h) { ok(0, "began the request"); return; }

    for (i = 0; i < 20000 && rc == UC_HTTP_PENDING; i++) {
        uc_net_pump();
        rc = uc_http_poll(h);
        if (rc == UC_HTTP_PENDING) nap();
    }

    ok(rc == UC_HTTP_DONE, "the exchange completed over real TLS");
    if (rc != UC_HTTP_DONE) printf("        (%s)\n", uc_http_error(h));

    ok(uc_http_status(h) == 401,
       "the server answered 401 - no key was sent, and that is the point");
    printf("        status %d\n", uc_http_status(h));

    b = uc_http_body(h, &len);
    ok(len > 0, "a body came back");

    j = uc_json_parse(b, len, err, sizeof err);
    v = j ? uc_json_path(j, "error.message") : 0;
    ok(v && v->type == UJ_STR, "and it parsed as JSON with error.message in it");
    if (v && v->str) printf("        \"%s\"\n", v->str);
    uc_json_free(j);

    uc_http_free(h);
}

/* ---- the claim UCD-47 actually makes -------------------------------------- */

/* "It does not stop the frame" is not a feeling, it is a number: the longest
 * time spent inside a single uc_http_poll() call. A frame at 60 Hz is 16.7 ms
 * and the editor has to draw in it too, so anything over a couple of
 * milliseconds is a stutter and anything over ~16 is a dropped frame.
 *
 * This is measured across a REAL request including the name lookup, because
 * the name lookup is the step that used to block: uc_http_begin() called
 * getaddrinfo, which on a cold cache takes as long as it takes. */
static double ms_now(void)
{
#ifdef _WIN32
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
#endif
}

static void t_no_stall(void)
{
    uc_header hd[2];
    uc_http_req r;
    uc_http *h;
    double worst = 0, after = 0, t0, t1, begin_ms;
    int i, rc = UC_HTTP_PENDING, polls = 0, worst_at = -1;

    puts("8. no call stops the frame");

    hd[0].name = "content-type";      hd[0].value = "application/json";
    hd[1].name = "anthropic-version"; hd[1].value = "2023-06-01";

    memset(&r, 0, sizeof r);
    r.host = "api.anthropic.com"; r.method = "POST"; r.path = "/v1/messages";
    r.headers = hd; r.nheaders = 2;
    r.body = "{\"model\":\"claude-sonnet-5\",\"max_tokens\":1,"
             "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
    r.body_len = -1;

    /* begin() is timed too: it is the call that used to carry the whole DNS
     * lookup, so it is the one whose old number was seconds. */
    t0 = ms_now();
    h = uc_http_begin(&r);
    begin_ms = ms_now() - t0;
    if (!h) { ok(0, "began the request"); return; }

    for (i = 0; i < 30000 && rc == UC_HTTP_PENDING; i++) {
        uc_net_pump();
        t0 = ms_now();
        rc = uc_http_poll(h);
        t1 = ms_now();
        if (t1 - t0 > worst) { worst = t1 - t0; worst_at = polls; }
        /* Once a status line has arrived the handshake is behind us, so from
         * here the numbers are the cost of MOVING DATA - which is the cost a
         * streaming generation pays over and over. */
        if (uc_http_status(h) && t1 - t0 > after) after = t1 - t0;
        polls++;
        if (rc == UC_HTTP_PENDING) nap();
    }

    printf("        %d polls, begin %.2f ms, worst %.2f ms (poll #%d), "
           "worst after handshake %.2f ms\n",
           polls, begin_ms, worst, worst_at, after);
    ok(rc == UC_HTTP_DONE, "the request completed");
    ok(begin_ms < 16.0, "uc_http_begin() returned within one frame");
    ok(worst < 16.0, "and no single poll took a frame's worth of time");

    /* The one that matters for a long generation. The handshake's certificate
     * verification is a few milliseconds of unavoidable arithmetic and happens
     * ONCE; if the transfer itself ever costs that much per poll, streaming
     * has started stuttering and this is what will say so. */
    ok(after < 2.0, "and moving data costs well under a frame, every time");

    /* A lookup was genuinely needed and genuinely pumped, rather than the
     * whole thing having been answered out of a warm cache in one call. */
    ok(polls > 1, "the exchange really was spread across many polls");
    uc_http_free(h);
}

/* Cancelling mid-flight must return immediately and leave nothing behind. The
 * hard case is cancelling DURING the name lookup: getaddrinfo cannot be
 * interrupted, so a cancel that waited for it would block for exactly as long
 * as the thing it was abandoning. */
static void t_cancel(void)
{
    uc_http_req r;
    uc_http *h;
    double t0, spent;
    int i;

    puts("9. cancelling");

    memset(&r, 0, sizeof r);
    r.host = "api.anthropic.com"; r.method = "GET"; r.path = "/";

    /* Cancel immediately, while the lookup is almost certainly still running. */
    h = uc_http_begin(&r);
    if (!h) { ok(0, "began"); return; }
    t0 = ms_now();
    uc_http_free(h);
    spent = ms_now() - t0;
    printf("        cancel during resolve: %.2f ms\n", spent);
    ok(spent < 16.0, "cancelling during the name lookup returns at once");

    /* And a second request straight afterwards must still work - i.e. the
     * abandoned lookup released the single resolver slot rather than wedging
     * it, which is the failure this refcount exists to prevent. */
    h = uc_http_begin(&r);
    if (!h) { ok(0, "began a second request"); return; }
    for (i = 0; i < 30000; i++) {
        int rc;
        uc_net_pump();
        rc = uc_http_poll(h);
        if (rc != UC_HTTP_PENDING) break;
        nap();
    }
    ok(uc_http_status(h) > 0,
       "a request started right after a cancel still completes");
    uc_http_free(h);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--live") == 0) {
        t_live();
        t_no_stall();
        t_cancel();
        printf("\n%s: %d failure(s)\n", fails ? "FAILED" : "PASSED", fails);
        return fails ? 1 : 0;
    }

    t_plain();
    t_status_codes();
    t_chunked();
    t_chunked_byte_at_a_time();
    t_sse();
    t_sse_error();
    t_request();

    printf("\n%s: %d failure(s)\n", fails ? "FAILED" : "PASSED", fails);
    return fails ? 1 : 0;
}
