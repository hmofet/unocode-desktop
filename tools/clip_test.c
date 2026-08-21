/* ===========================================================================
 * clip_test.c - host_clip.c's contract, against a REAL OS clipboard.
 *
 * Links host_clip.c and stands in for the core's clipboard with a buffer that
 * behaves exactly as uc_doc.c's does, 32 KB cap and all - because the cap is
 * where the interesting bug lives, not in the five lines around it.
 *
 * Needs a display: SDL's clipboard is a video-subsystem service.  On a build
 * box that means Xvfb, which is what ./build.sh --test arranges.
 *
 *   cc tools/clip_test.c host/host_clip.c $(sdl2-config --cflags --libs)
 * ======================================================================== */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

/* ---- the core's clipboard, reproduced faithfully (uc_doc.c) ---------------- */
#define UC_CLIP_CAP (32 * 1024)
static char g_clip[UC_CLIP_CAP];
static int  g_cliplen;

void uc_clip_set(const char *s, int n)
{
    if (n < 0) n = (int)strlen(s);
    if (n > UC_CLIP_CAP - 1) n = UC_CLIP_CAP - 1;
    memcpy(g_clip, s, (unsigned long)n);
    g_clip[n] = 0;
    g_cliplen = n;
}

const char *uc_clip_get(int *n)
{
    if (n) *n = g_cliplen;
    return g_clip;
}

/* ---- harness --------------------------------------------------------------- */
static int g_fail;

static void ok(int cond, const char *what)
{
    printf("%s %s\n", cond ? "  ok  " : "  FAIL", what);
    if (!cond) g_fail++;
}

/* what the OS clipboard holds right now, as a freshly allocated C string */
static char *os_get(void)
{
    char *t = SDL_GetClipboardText();
    char *c = strdup(t ? t : "");
    if (t) SDL_free(t);
    return c;
}

static void os_set(const char *s) { SDL_SetClipboardText(s); }

static void core_set(const char *s) { uc_clip_set(s, -1); }

static const char *core_get(void) { int n; return uc_clip_get(&n); }

int main(void)
{
    SDL_Window *w;
    char *got;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "clip_test: no video (%s) - needs a display\n",
                SDL_GetError());
        return 2;
    }
    /* X11 hands the clipboard to a WINDOW, so there must be one */
    w = SDL_CreateWindow("clip_test", 0, 0, 64, 64, SDL_WINDOW_HIDDEN);
    if (!w) { fprintf(stderr, "clip_test: no window (%s)\n", SDL_GetError()); return 2; }

    printf("1. a pre-launch copy is adopted at startup\n");
    os_set("pasted from a browser");
    host_clip_init();
    ok(!strcmp(core_get(), "pasted from a browser"),
       "host_clip_init() pulls what the OS already held");

    printf("2. OS -> core, on the clipboard-changed road\n");
    os_set("second copy");
    host_clip_pull();
    ok(!strcmp(core_get(), "second copy"), "a later OS copy reaches the core");

    printf("3. CRLF is normalised on the way in\n");
    os_set("alpha\r\nbeta\r\ngamma");
    host_clip_pull();
    ok(!strcmp(core_get(), "alpha\nbeta\ngamma"),
       "CRLF becomes LF (the document model has no carriage return)");
    os_set("old\rmac\rlines");
    host_clip_pull();
    ok(!strcmp(core_get(), "old\nmac\nlines"), "a lone CR becomes LF too");

    printf("4. core -> OS, on the copy road\n");
    core_set("copied out of the editor");
    host_clip_push();
    got = os_get();
    ok(!strcmp(got, "copied out of the editor"), "a core copy reaches the OS");
    free(got);

    printf("5. multi-line text survives the round trip\n");
    core_set("one\ntwo\nthree");
    host_clip_push();
    host_clip_pull();               /* as SDL_CLIPBOARDUPDATE would */
    ok(!strcmp(core_get(), "one\ntwo\nthree"),
       "push then pull leaves the core text unchanged");

    printf("6. the mirror does not loop\n");
    {
        /* push raises SDL_CLIPBOARDUPDATE, whose pull must not read back as a
         * new OS copy and re-push: ten turns of the crank change nothing */
        int i, stable = 1;
        core_set("stable text");
        for (i = 0; i < 10; i++) {
            host_clip_push();
            host_clip_pull();
            if (strcmp(core_get(), "stable text")) stable = 0;
        }
        ok(stable, "ten push/pull cycles leave both sides alone");
        got = os_get();
        ok(!strcmp(got, "stable text"), "and the OS still holds what was copied");
        free(got);
    }

    printf("7. an OS clipboard LARGER than the core's is not destroyed\n");
    {
        /* the trap: pull truncates to 32 KB, and if the core-side signature
         * were taken from what we SENT rather than what the core KEPT, the
         * next push would replace the user's big clipboard with our stub */
        int big = 100 * 1024, i;
        char *blob = (char *)malloc((size_t)big + 1);
        for (i = 0; i < big; i++) blob[i] = (char)('a' + i % 26);
        blob[big] = 0;
        os_set(blob);
        host_clip_pull();
        ok(strlen(core_get()) == UC_CLIP_CAP - 1, "the core kept its 32 KB");
        host_clip_push();           /* the frame after the paste */
        got = os_get();
        ok(strlen(got) == (size_t)big,
           "the OS clipboard is STILL 100 KB, not our truncated copy");
        ok(!strcmp(got, blob), "and byte-for-byte what it was");
        free(got);
        free(blob);
    }

    printf("8. an empty core clipboard never clears the OS one\n");
    os_set("something the user wants to keep");
    host_clip_pull();
    uc_clip_set("", 0);
    host_clip_push();
    got = os_get();
    ok(!strcmp(got, "something the user wants to keep"),
       "an empty copy is not a request to wipe the clipboard");
    free(got);

    printf("9. an empty OS clipboard leaves the core's last copy alone\n");
    core_set("still here");
    os_set("");
    host_clip_pull();
    ok(!strcmp(core_get(), "still here"),
       "the core keeps its copy when the OS has nothing");

    SDL_DestroyWindow(w);
    SDL_Quit();
    printf("\n%s: %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
