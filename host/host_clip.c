/* ===========================================================================
 * host_clip.c - the OS clipboard, mirrored onto the core's.
 *
 * UnoCode already has a module-wide clipboard: the editor, the terminal and
 * the extension host all cut and paste through uc_clip_set/uc_clip_get, the
 * way they do in VS Code.  What phase 0 lacked was any connection between
 * that buffer and the one the rest of the desktop uses, so a URL could not be
 * pasted in and a snippet could not be copied out.
 *
 * This mirrors the two, and does it by SYNCHRONISING the buffers rather than
 * by intercepting Ctrl+C/X/V.  That matters: copy and paste are reachable from
 * the command palette, the menus and an extension as well as from the
 * keyboard, and a key-level hook would serve only one of those roads.  Two
 * pumps instead:
 *
 *   pull   OS -> core.  On SDL_CLIPBOARDUPDATE (the OS told us it changed),
 *          on focus-gained (belt and braces: to copy in another application
 *          you must focus it, so coming back is the event that cannot be
 *          missed), and immediately before a Ctrl+V is dispatched.
 *   push   core -> OS.  After any batch of events that could have run a copy.
 *
 * The loop the two would otherwise make is broken with signatures rather than
 * with a flag, because SDL_SetClipboardText itself raises SDL_CLIPBOARDUPDATE:
 * each side remembers what it last saw, and a round trip therefore compares
 * equal and stops.
 *
 * Two details that are easy to get wrong and are deliberate here:
 *
 *   - The core's buffer is 32 KB and TRUNCATES.  So the core-side signature is
 *     taken AFTER uc_clip_set, from what the core actually kept.  Take it from
 *     what we sent instead and a 100 KB clipboard pulled in from a browser
 *     reads back as "changed", gets pushed, and the user's 100 KB is replaced
 *     by our 32 KB.  Truncating our own copy is unavoidable; destroying theirs
 *     is not.
 *   - An EMPTY core clipboard is never pushed.  Clearing the OS clipboard is
 *     not something the user asked for, and at startup the core's is empty.
 * ======================================================================== */
#include <SDL.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

/* the core's clipboard (uc_doc.c).  Declared rather than included, as main.c
 * declares uc_doc_open: the contract is the symbol. */
extern void        uc_clip_set(const char *s, int n);
extern const char *uc_clip_get(int *n);

static unsigned long long g_os_sig;    /* the OS text we last saw   */
static unsigned long long g_core_sig;  /* the core text we last saw */

static unsigned long long sig_of(const char *s, int n)
{
    unsigned long long h = 1469598103934665603ULL;   /* FNV-1a 64 */
    int i;
    for (i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return h ^ ((unsigned long long)(unsigned)n << 32);
}

/* CRLF (and a lone CR) -> LF, in place.  The document model has no notion of a
 * carriage return: one pasted in becomes a character in the buffer, draws as a
 * glyph and gets written back into the file.  Returns the new length. */
static int to_lf(char *s, int n)
{
    int i, o = 0;
    for (i = 0; i < n; i++) {
        if (s[i] == '\r') {
            if (i + 1 < n && s[i + 1] == '\n') continue;   /* CRLF: keep the LF */
            s[o++] = '\n';                                 /* a lone CR         */
        } else s[o++] = s[i];
    }
    s[o] = 0;
    return o;
}

void host_clip_pull(void)
{
    char *os;
    const char *core;
    int n, cn;
    unsigned long long s;

    /* An empty OS clipboard leaves the core's alone rather than blanking it:
     * VS Code keeps its last copy too, and there is nothing to gain by
     * forgetting one. */
    if (!SDL_HasClipboardText()) return;
    os = SDL_GetClipboardText();
    if (!os) return;

    n = (int)strlen(os);
    s = sig_of(os, n);
    if (s == g_os_sig) { SDL_free(os); return; }
    g_os_sig = s;

    n = to_lf(os, n);
    uc_clip_set(os, n);
    SDL_free(os);

    core = uc_clip_get(&cn);            /* what the core KEPT, not what we sent */
    g_core_sig = sig_of(core, cn);
}

void host_clip_push(void)
{
    const char *core;
    int n;
    unsigned long long s;
    char *out;

    core = uc_clip_get(&n);
    s = sig_of(core, n);
    if (s == g_core_sig) return;
    g_core_sig = s;
    if (n <= 0) return;                 /* never CLEAR the OS clipboard */

#ifdef _WIN32
    /* Windows applications expect CRLF from the clipboard; a Notepad paste of
     * LF-only text is one long line there. */
    {
        int i, o = 0;
        out = (char *)malloc((size_t)n * 2 + 1);
        if (!out) return;
        for (i = 0; i < n; i++) {
            if (core[i] == '\n') out[o++] = '\r';
            out[o++] = core[i];
        }
        out[o] = 0;
    }
#else
    out = (char *)malloc((size_t)n + 1);
    if (!out) return;
    memcpy(out, core, (size_t)n);
    out[n] = 0;
#endif

    SDL_SetClipboardText(out);
    /* remember what we put there, so the SDL_CLIPBOARDUPDATE this raises does
     * not read back as somebody else's change */
    g_os_sig = sig_of(out, (int)strlen(out));
    free(out);
}

void host_clip_init(void)
{
    const char *core;
    int n;
    core = uc_clip_get(&n);
    g_core_sig = sig_of(core, n);
    g_os_sig = 0;                       /* nothing seen yet: the first pull runs */
    host_clip_pull();                   /* a copy made before launch pastes */
}
