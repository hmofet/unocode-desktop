/* ===========================================================================
 * host_dialog.c - the native Open dialogs, and what happens after one.
 *
 * The picker itself is per-platform and lives below (Windows IFileDialog,
 * macOS NSOpenPanel through the C runtime bridge, and on Linux the desktop's
 * own helper).  A NATIVE dialog rather than one drawn in the canvas, because a
 * user's places, bookmarks, network mounts and recent folders live in their
 * file manager, and an in-app list has none of them.
 *
 * This file is the part that is the same everywhere: turning the absolute path
 * the OS hands back into something the editor can address.  That is the whole
 * difficulty.  The core speaks (volume, directory, name), and volume 0 is the
 * workspace - so a file INSIDE the open folder is a relative path, and a file
 * outside it has no address at all until the workspace moves to contain it.
 * Opening a file therefore sometimes re-roots the workspace, which is exactly
 * what VS Code does when you open a file from another project.
 * ======================================================================== */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "host.h"

extern void uc_open_folder(int vol, const char *dir);
extern int  uc_doc_open(int vol, const char *dir, const char *name);

/* the picker, per platform; fills an ABSOLUTE host path.  0 = cancelled or
 * unavailable. */
int  host_pick_path(int want_folder, char *out, int cap);

static char g_root[1024];          /* the workspace root, '/'-separated */

void host_dialog_set_root(const char *root)
{
    size_t i;
    snprintf(g_root, sizeof g_root, "%s", root ? root : ".");
    for (i = 0; g_root[i]; i++) if (g_root[i] == '\\') g_root[i] = '/';
    i = strlen(g_root);
    while (i > 1 && g_root[i - 1] == '/') g_root[--i] = 0;
}

const char *host_dialog_root(void) { return g_root; }

/* Absolute, '/'-separated, no trailing slash, and with the host's own idea of
 * "the same directory" applied: on Windows the comparison below has to be
 * case-insensitive or C:\Repos and c:\repos are different workspaces. */
static void normalise(char *p)
{
    size_t i, n;
    for (i = 0; p[i]; i++) if (p[i] == '\\') p[i] = '/';
    n = strlen(p);
    while (n > 1 && p[n - 1] == '/') p[--n] = 0;
}

static int path_ieq_n(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        int ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
    return 1;
}

/* Split an absolute path into the workspace-relative directory and name the
 * core addresses files by.  1 if it is inside the current workspace. */
static int under_root(const char *abs, char *dir, int dcap, char *name, int ncap)
{
    size_t rn = strlen(g_root);
    const char *rel, *slash, *p;

    if (!rn || !path_ieq_n(abs, g_root, rn)) return 0;
    if (abs[rn] != '/' && abs[rn] != 0) return 0;      /* /foo vs /foobar */
    rel = abs + rn;
    while (*rel == '/') rel++;

    slash = 0;
    for (p = rel; *p; p++) if (*p == '/') slash = p;
    if (slash) {
        int n = (int)(slash - rel);
        if (n >= dcap) n = dcap - 1;
        memcpy(dir, rel, (size_t)n);
        dir[n] = 0;
        { int i; for (i = 0; dir[i]; i++) if (dir[i] == '/') dir[i] = '\\'; }
        snprintf(name, (size_t)ncap, "%s", slash + 1);
    } else {
        dir[0] = 0;
        snprintf(name, (size_t)ncap, "%s", rel);
    }
    return 1;
}

/* The core's hook (see unocode.h).  Returns 1 with an address the core can
 * open, 0 if the user cancelled or this platform has no picker. */
int pc64_shell_pick(int want_folder, int *vol, char *dir, int dcap,
                    char *name, int ncap)
{
    char abs[1024];

    if (dcap > 0) dir[0] = 0;
    if (ncap > 0) name[0] = 0;
    if (vol) *vol = 0;

    if (!host_pick_path(want_folder, abs, sizeof abs)) return 0;
    normalise(abs);

    if (want_folder) {
        /* Re-root: mount the chosen folder as volume 0 and tell the editor its
         * directory is the volume root.  The explorer, quick open and search
         * all address that volume, so this is the whole of "changing folders
         * re-roots the explorer without restarting". */
        if (!host_fs_set_volume_root(0, abs)) return 0;
        host_dialog_set_root(abs);
        host_recent_add(abs);
        snprintf(dir, (size_t)dcap, "%s", "");
        return 1;
    }

    if (under_root(abs, dir, dcap, name, ncap)) return 1;

    /* Outside the workspace.  Move the workspace to the file's folder rather
     * than refusing: a picker that can only reach files you could already
     * reach is not worth opening. */
    {
        char folder[1024];
        char *slash;
        snprintf(folder, sizeof folder, "%s", abs);
        slash = strrchr(folder, '/');
        if (!slash) return 0;
        *slash = 0;
        if (!folder[0]) { folder[0] = '/'; folder[1] = 0; }
        if (!host_fs_set_volume_root(0, folder)) return 0;
        host_dialog_set_root(folder);
        host_recent_add(folder);
        uc_open_folder(0, "");
        snprintf(dir, (size_t)dcap, "%s", "");
        snprintf(name, (size_t)ncap, "%s", slash + 1);
        return 1;
    }
}

/* Adopt an absolute host path that arrived from OUTSIDE the picker - today
 * that means a file or folder dropped on the window (UCD-19).
 *
 * It is the picker's own tail end, factored out rather than copied: the rules
 * for "this is inside the workspace" and "this is not, so move the workspace"
 * are subtle enough that a second copy would drift, and a drop that re-rooted
 * differently from an Open would be a bug nobody could describe. */
int host_adopt_path(const char *in)
{
    char abs[1024], dir[512], name[256];
    struct stat st;

    if (!in || !in[0]) return 0;
    snprintf(abs, sizeof abs, "%s", in);
    normalise(abs);
    if (stat(abs, &st) != 0) return 0;

    if ((st.st_mode & S_IFMT) == S_IFDIR) {
        if (!host_fs_set_volume_root(0, abs)) return 0;
        host_dialog_set_root(abs);
        host_recent_add(abs);
        uc_open_folder(0, "");
        return 1;
    }

    if (under_root(abs, dir, sizeof dir, name, sizeof name)) {
        uc_doc_open(0, dir, name);
        return 1;
    }
    {   /* outside the workspace: move to its folder, as the picker does */
        char folder[1024];
        char *slash;
        snprintf(folder, sizeof folder, "%s", abs);
        slash = strrchr(folder, '/');
        if (!slash) return 0;
        *slash = 0;
        if (!folder[0]) { folder[0] = '/'; folder[1] = 0; }
        if (!host_fs_set_volume_root(0, folder)) return 0;
        host_dialog_set_root(folder);
        host_recent_add(folder);
        uc_open_folder(0, "");
        uc_doc_open(0, "", slash + 1);
        return 1;
    }
}

/* ---- the recent-folder list -----------------------------------------------
 * Kept beside the window state under HOME, newest first, deduplicated.  It
 * outlives the process because that is the entire point of it. */
#define RECENT_MAX 12
static char g_recent[RECENT_MAX][512];
static int  g_nrecent;
static char g_recent_file[1024];

void host_recent_file(const char *home)
{
    snprintf(g_recent_file, sizeof g_recent_file, "%s/RECENT.LST", home ? home : ".");
}

void host_recent_load(void)
{
    FILE *f;
    char line[512];
    g_nrecent = 0;
    if (!g_recent_file[0]) return;
    f = fopen(g_recent_file, "rb");
    if (!f) return;
    while (g_nrecent < RECENT_MAX && fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (n) snprintf(g_recent[g_nrecent++], sizeof g_recent[0], "%s", line);
    }
    fclose(f);
}

static void recent_save(void)
{
    FILE *f;
    int i;
    if (!g_recent_file[0]) return;
    f = fopen(g_recent_file, "wb");
    if (!f) return;
    for (i = 0; i < g_nrecent; i++) fprintf(f, "%s\n", g_recent[i]);
    fclose(f);
}

void host_recent_add(const char *path)
{
    int i, n;
    if (!path || !path[0]) return;
    /* move-to-front, so re-opening a folder does not push the others out */
    for (i = 0; i < g_nrecent; i++)
        if (path_ieq_n(g_recent[i], path, strlen(path) + 1)) break;
    if (i == 0 && g_nrecent) { recent_save(); return; }
    n = (i < g_nrecent) ? i : (g_nrecent < RECENT_MAX ? g_nrecent : RECENT_MAX - 1);
    for (; n > 0; n--) memcpy(g_recent[n], g_recent[n - 1], sizeof g_recent[0]);
    snprintf(g_recent[0], sizeof g_recent[0], "%s", path);
    if (i >= g_nrecent && g_nrecent < RECENT_MAX) g_nrecent++;
    recent_save();
}

int host_recent_count(void) { return g_nrecent; }

const char *host_recent_at(int i)
{
    return (i >= 0 && i < g_nrecent) ? g_recent[i] : "";
}
