/* ===========================================================================
 * host_win.c - the parts of being a real window: the title bar, the mouse
 * pointer's shape, and remembering where the window was.
 *
 * All three are things the pc64 shell owned and a hosted build has to provide
 * itself.  They share a file because they share a question - "what does the OS
 * know about this window that the editor does not" - and a state file.
 *
 * The editor is asked, not guessed at.  Which document is active, whether it
 * is dirty and what sits under the pointer are all answered by uc_host_*
 * (see unocode.h): re-deriving the workbench's layout here would produce a
 * pointer that promises a resize where a click does nothing, the first time
 * either side changed.
 * ======================================================================== */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

/* the editor's host-query block (unocode.h is subsystem-internal) */
extern const char *uc_host_title(void);
extern int uc_host_cursor_at(int x, int y);
extern int uc_host_tab_count(void);
extern int uc_host_tab_info(int i, int *vol, char *dir, int dcap,
                            char *name, int ncap);
extern int uc_doc_open(int vol, const char *dir, const char *name);
enum { UC_CUR_ARROW = 0, UC_CUR_TEXT, UC_CUR_WE, UC_CUR_NS };

static char g_state[1024];          /* <home>/WINDOW.STA */

void host_state_dir(const char *home)
{
    snprintf(g_state, sizeof g_state, "%s/WINDOW.STA", home ? home : ".");
}

/* ---- the title ------------------------------------------------------------ */

/* "file - folder - UnoCode", with a dot on the file while it is unsaved.  The
 * document half comes from the editor; the FOLDER half is ours, because the
 * core knows the workspace only as a volume number and would otherwise put its
 * own label there. */
void host_title_update(SDL_Window *win)
{
    static char last[512];
    char want[512];
    const char *doc = uc_host_title();
    const char *root = host_dialog_root(), *folder = root, *p;

    for (p = root; *p; p++) if (*p == '/' || *p == '\\') folder = p + 1;
    if (!folder[0]) folder = root;           /* the filesystem root itself */

    if (doc && doc[0] && folder[0])
        snprintf(want, sizeof want, "%s - %s - UnoCode", doc, folder);
    else if (folder[0])
        snprintf(want, sizeof want, "%s - UnoCode", folder);
    else
        snprintf(want, sizeof want, "UnoCode");

    if (!strcmp(want, last)) return;         /* SetWindowTitle is not free */
    snprintf(last, sizeof last, "%s", want);
    SDL_SetWindowTitle(win, want);
}

/* ---- the pointer ---------------------------------------------------------- */

static SDL_Cursor *g_cur[4];
static int g_cur_now = -1;

void host_cursors_init(void)
{
    g_cur[UC_CUR_ARROW] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
    g_cur[UC_CUR_TEXT]  = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_IBEAM);
    g_cur[UC_CUR_WE]    = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEWE);
    g_cur[UC_CUR_NS]    = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZENS);
}

void host_cursor_update(int x, int y)
{
    int want = uc_host_cursor_at(x, y);
    if (want < 0 || want > UC_CUR_NS) want = UC_CUR_ARROW;
    if (want == g_cur_now || !g_cur[want]) return;
    g_cur_now = want;
    SDL_SetCursor(g_cur[want]);
}

/* ---- geometry and the open workspace -------------------------------------- */
/* A plain key=value file rather than JSON: it is written on quit and read
 * before the window exists, so it cannot go through the editor's config
 * machinery (which lives on a volume the core has not mounted yet), and a
 * corrupt one has to degrade to "use the defaults" rather than to an error. */

void host_geom_defaults(HostGeom *g)
{
    g->x = SDL_WINDOWPOS_CENTERED;
    g->y = SDL_WINDOWPOS_CENTERED;
    g->w = 1280;
    g->h = 800;
    g->maximized = 0;
    g->folder[0] = 0;
    g->ntab = 0;
}

/* Is this rectangle on a display that still exists?  A window restored to a
 * monitor that has been unplugged is invisible and unrecoverable without
 * editing the state file, so an off-screen geometry is CLAMPED back to the
 * primary display rather than honoured. */
static int on_some_display(int x, int y, int w, int h)
{
    int n = SDL_GetNumVideoDisplays(), i;
    SDL_Rect r;
    r.x = x; r.y = y; r.w = w; r.h = h;
    for (i = 0; i < n; i++) {
        SDL_Rect d, isect;
        if (SDL_GetDisplayBounds(i, &d) != 0) continue;
        if (SDL_IntersectRect(&r, &d, &isect) &&
            isect.w >= 200 && isect.h >= 100) return 1;   /* enough to grab */
    }
    return 0;
}

void host_geom_load(HostGeom *g)
{
    FILE *f;
    char line[1024];
    host_geom_defaults(g);
    if (!g_state[0]) return;
    f = fopen(g_state, "rb");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        char *v;
        size_t n;
        if (!eq) continue;
        *eq = 0;
        v = eq + 1;
        n = strlen(v);
        while (n && (v[n - 1] == '\n' || v[n - 1] == '\r')) v[--n] = 0;
        if      (!strcmp(line, "x")) g->x = atoi(v);
        else if (!strcmp(line, "y")) g->y = atoi(v);
        else if (!strcmp(line, "w")) g->w = atoi(v);
        else if (!strcmp(line, "h")) g->h = atoi(v);
        else if (!strcmp(line, "max")) g->maximized = atoi(v) != 0;
        else if (!strcmp(line, "folder")) snprintf(g->folder, sizeof g->folder, "%s", v);
        else if (!strcmp(line, "tab") && g->ntab < HOST_TABS)
            snprintf(g->tab[g->ntab++], sizeof g->tab[0], "%s", v);
    }
    fclose(f);

    if (g->w < 700 || g->h < 460 || g->w > 8192 || g->h > 8192)
        { g->w = 1280; g->h = 800; }
    if (!on_some_display(g->x, g->y, g->w, g->h)) {
        g->x = SDL_WINDOWPOS_CENTERED;
        g->y = SDL_WINDOWPOS_CENTERED;
    }
}

/* The last non-maximized geometry, kept live: querying the window at quit time
 * would read the MAXIMIZED size, and restoring that as a normal window gives
 * one that fills the screen and cannot be un-maximized back to anything. */
static HostGeom g_live;
static int g_live_ok;

void host_geom_note(SDL_Window *win)
{
    Uint32 fl = SDL_GetWindowFlags(win);
    g_live.maximized = (fl & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN)) != 0;
    if (!g_live.maximized) {
        SDL_GetWindowPosition(win, &g_live.x, &g_live.y);
        SDL_GetWindowSize(win, &g_live.w, &g_live.h);
    }
    g_live_ok = 1;
}

void host_geom_save(SDL_Window *win, const char *folder)
{
    FILE *f;
    int i;
    if (!g_state[0]) return;
    if (!g_live_ok) host_geom_note(win);
    f = fopen(g_state, "wb");
    if (!f) return;
    fprintf(f, "# UnoCode Desktop window state - safe to delete\n");
    fprintf(f, "x=%d\ny=%d\nw=%d\nh=%d\nmax=%d\n",
            g_live.x, g_live.y, g_live.w, g_live.h, g_live.maximized);
    if (folder && folder[0]) fprintf(f, "folder=%s\n", folder);
    for (i = 0; i < uc_host_tab_count(); i++) {
        char dir[128], name[32], path[192];
        int vol = 0;
        if (!uc_host_tab_info(i, &vol, dir, sizeof dir, name, sizeof name)) continue;
        if (vol != 0) continue;              /* only the workspace reopens */
        if (dir[0]) snprintf(path, sizeof path, "%s\\%s", dir, name);
        else        snprintf(path, sizeof path, "%s", name);
        fprintf(f, "tab=%s\n", path);
    }
    fclose(f);
}

void host_geom_reopen(const HostGeom *g)
{
    int i;
    for (i = 0; i < g->ntab; i++) {
        const char *p = g->tab[i], *name = p, *q;
        char dir[128];
        size_t dn;
        for (q = p; *q; q++) if (*q == '\\' || *q == '/') name = q + 1;
        dn = (size_t)(name - p);
        if (dn >= sizeof dir) dn = sizeof dir - 1;
        memcpy(dir, p, dn);
        while (dn && (dir[dn - 1] == '\\' || dir[dn - 1] == '/')) dn--;
        dir[dn] = 0;
        /* A file that has been deleted or renamed since is simply skipped:
         * uc_doc_open reports it and the session comes back one tab lighter,
         * which is better than refusing to start. */
        uc_doc_open(0, dir, name);
    }
}
