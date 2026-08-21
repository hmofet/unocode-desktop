/* ===========================================================================
 * host_fs.c - the uno_fs_* seam over a real operating system's filesystem.
 *
 * The editor core speaks FAT: integer volumes, '\\'-joined paths and
 * upper-cased names.  This file makes a directory tree on the host answer in
 * that dialect:
 *
 *   - each volume is a root directory registered at startup;
 *   - every path component is resolved CASE-INSENSITIVELY, because the core
 *     upper-cases paths (FAT does not care; ext4 does);
 *   - uno_fs_fat_index() answers -1 for every volume, which sends the core
 *     down its documented fallback road (uno_fs_list_dir + uno_fs_isdir);
 *   - names cross the seam VERBATIM, at whatever width the caller asked for.
 *     Until UCD-11 the seam was 15 bytes wide and this file kept a FAT-style
 *     alias table to squeeze real names through it; the seam takes a stride
 *     now, so the table is gone.
 *
 * Return conventions are the core's, verified against its call sites:
 * read/size answer bytes or -1; write/mkdir/delete answer 1 on success, 0 on
 * failure (uc_term prints "mkdir failed" on falsy); isdir answers 1/0.
 * ======================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

#ifndef _WIN32
#  include <limits.h>
#  include <stdlib.h>
#endif

#ifdef _WIN32
#  include <direct.h>
#  define host_mkdir(p) _mkdir(p)
#else
#  define host_mkdir(p) mkdir(p, 0755)
#endif

#define MAXVOL   8
#define MAXPATH  1024
/* The width this file hands out when IT owns the buffer (the root listing).
 * Every other listing is the caller's array at the caller's stride, which is
 * the whole point of the seam taking one - this constant binds nothing but
 * g_rootlist and never has to agree with the core's. */
#define UC_HOST_NAME_MAX 256

typedef struct {
    char name[16];              /* volume label shown by the core            */
    char root[MAXPATH];         /* host directory, '/'-separated, no slash   */
    int  writable;
} HostVol;

static HostVol g_vol[MAXVOL];
static int     g_nvol;

int host_fs_add_volume(const char *name, const char *root, int writable)
{
    HostVol *v;
    size_t n;
    if (g_nvol >= MAXVOL || !name || !root) return -1;
    v = &g_vol[g_nvol];
    strncpy(v->name, name, sizeof v->name - 1);
    v->name[sizeof v->name - 1] = 0;
    strncpy(v->root, root, sizeof v->root - 1);
    v->root[sizeof v->root - 1] = 0;
    for (n = 0; v->root[n]; n++) if (v->root[n] == '\\') v->root[n] = '/';
    n = strlen(v->root);
    while (n > 1 && v->root[n - 1] == '/') v->root[--n] = 0;
    v->writable = writable;
    return g_nvol++;
}

/* Canonical absolute path, '/'-separated.  Everything downstream compares
 * paths - "is this file inside the workspace", "is this the folder last
 * session had open" - and "." compares equal to nothing, including itself
 * after a chdir.  Falls back to copying the input rather than failing: a
 * relative root still works, it just will not match. */
int host_fs_abspath(const char *in, char *out, int cap)
{
    size_t n;
    /* The ALLOCATING form of both, deliberately.  realpath() into a caller's
     * buffer is only safe when that buffer is PATH_MAX - glibc's _FORTIFY
     * check aborts the process outright otherwise, which is exactly what a
     * 1 KB buffer here did - and PATH_MAX is 4 KB on Linux, larger than
     * anything else in this file wants to be. */
#ifdef _WIN32
    char *full = _fullpath(0, in, 0);
#else
    char *full = realpath(in, 0);
#endif
    snprintf(out, (size_t)cap, "%s", full ? full : in);
    if (full) free(full);
    for (n = 0; out[n]; n++) if (out[n] == '\\') out[n] = '/';
    n = strlen(out);
    while (n > 1 && out[n - 1] == '/') out[--n] = 0;
    return full != 0;
}

/* mkdir -p.  The per-user data directory does not exist on a first run, and
 * uno_fs_mkdir() cannot create it: that road resolves INSIDE a volume, which
 * needs the volume's own root to be there already. */
int host_fs_mkpath(const char *path)
{
    char buf[MAXPATH];
    size_t i, n;
    struct stat st;

    if (!path || !*path) return 0;
    n = strlen(path);
    if (n >= sizeof buf) return 0;
    memcpy(buf, path, n + 1);
    for (i = 0; i < n; i++) if (buf[i] == '\\') buf[i] = '/';

    /* skip a leading "/" or a "C:/" so the first component is never empty */
    i = (buf[0] == '/') ? 1 : (n > 2 && buf[1] == ':') ? 3 : 0;
    for (; i <= n; i++) {
        if (buf[i] && buf[i] != '/') continue;
        { char save = buf[i];
          buf[i] = 0;
          if (buf[0] && stat(buf, &st) != 0 && host_mkdir(buf) != 0) {
              /* a race with another process creating it is not a failure */
              if (stat(buf, &st) != 0) return 0;
          }
          buf[i] = save; }
    }
    return stat(path, &st) == 0;
}

/* ---- case-insensitive path resolution ------------------------------------- */

static int ieq(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = (unsigned char)*a++, cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
    }
    return *a == *b;
}

/* Find the on-disk spelling of `want` inside directory `dir`; 1 if found.
 * An exact-case entry is preferred without a scan. */
static int match_component(const char *dir, const char *want, char *out, int cap)
{
    char probe[MAXPATH];
    struct stat st;
    DIR *d;
    struct dirent *e;

    if (snprintf(probe, sizeof probe, "%s/%s", dir, want) >= (int)sizeof probe)
        return 0;
    if (stat(probe, &st) == 0) {
        snprintf(out, (size_t)cap, "%s", want);
        return 1;
    }
    d = opendir(dir);
    if (!d) return 0;
    while ((e = readdir(d)) != 0) {
        if (ieq(e->d_name, want)) {
            snprintf(out, (size_t)cap, "%s", e->d_name);
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
}

/* ---- long names ------------------------------------------------------------
 * There is nothing here any more, and that is the feature.
 *
 * Phase 0 could only pass 15 bytes across the listing seam, so this file kept
 * a FAT-style alias table - VeryLongComponentName.tsx became VeryLongCo~1.tsx
 * on the way out and was translated back on every read, write, mkdir and
 * listing.  ~230 lines, four caps, a hash table and a string pool, all to make
 * a normal project listable.
 *
 * UCD-11 widened the seam instead (uno_fs_list_dir takes the caller's stride
 * now), so names cross verbatim and the table, its caps and its whole class of
 * failure - a stale alias resolving into the wrong file - are gone. */

/* Resolve a core-side path ("EXT\\HELLO\\MAIN.JS", any case) to a host path.
 * `create_leaf` lets a path whose FINAL component does not exist yet resolve
 * anyway (for writes and mkdir); every parent must exist either way.
 *
 * Every component is now the file's REAL name - UCD-11 widened the seam, so
 * there is no alias to translate back and no per-directory table to consult.
 * The only translation left is the case-insensitive match the core was
 * written against, which match_component() does against the actual disk. */
static int resolve(int vol, const char *path, char *out, int create_leaf)
{
    char comp[MAXPATH], found[MAXPATH];
    const char *p;
    int ci;

    if (vol < 0 || vol >= g_nvol) return 0;
    strncpy(out, g_vol[vol].root, MAXPATH - 1);
    out[MAXPATH - 1] = 0;
    if (!path || !path[0]) return 1;

    p = path;
    while (*p) {
        while (*p == '\\' || *p == '/') p++;
        if (!*p) break;
        ci = 0;
        while (*p && *p != '\\' && *p != '/' && ci < (int)sizeof comp - 1)
            comp[ci++] = *p++;
        comp[ci] = 0;
        if (!strcmp(comp, ".") || !strcmp(comp, "..")) return 0;  /* no escape */

        if (!match_component(out, comp, found, sizeof found)) {
            const char *rest = p;
            while (*rest == '\\' || *rest == '/') rest++;
            if (!create_leaf || *rest) return 0;   /* a missing PARENT fails */
            snprintf(found, sizeof found, "%s", comp);
        }
        if (strlen(out) + 1 + strlen(found) + 1 > MAXPATH) return 0;
        strcat(out, "/");
        strcat(out, found);
    }
    return 1;
}

/* Re-point an existing volume at a different directory.  Open Folder does this
 * to volume 0 rather than restarting the process, so the explorer, quick open
 * and search all re-root together - they all address the volume, not a path.
 *
 * Nothing is cached across the change: with the alias table gone (UCD-11),
 * this volume's state IS its root path, so re-pointing it is complete. */
int host_fs_set_volume_root(int vol, const char *root)
{
    size_t n;
    struct stat st;
    if (vol < 0 || vol >= g_nvol || !root || !root[0]) return 0;
    if (stat(root, &st) != 0 || (st.st_mode & S_IFMT) != S_IFDIR) return 0;
    snprintf(g_vol[vol].root, sizeof g_vol[vol].root, "%s", root);
    for (n = 0; g_vol[vol].root[n]; n++)
        if (g_vol[vol].root[n] == '\\') g_vol[vol].root[n] = '/';
    n = strlen(g_vol[vol].root);
    while (n > 1 && g_vol[vol].root[n - 1] == '/') g_vol[vol].root[--n] = 0;
    return 1;
}

/* ---- the seam ------------------------------------------------------------- */

int uno_fs_volumes(void) { return g_nvol; }

const char *uno_fs_volume_name(int vol)
{
    return (vol >= 0 && vol < g_nvol) ? g_vol[vol].name : "";
}

int uno_fs_writable(int vol)
{
    return (vol >= 0 && vol < g_nvol) ? g_vol[vol].writable : 0;
}

int uno_fs_kind(int vol)  { (void)vol; return 0; }

/* Where per-user state belongs.  This answered 0 - the WORKSPACE - so
 * UNOCODE\SETTINGS.JSN was written into the folder being edited: every project
 * UnoCode touched grew a settings directory, and sooner or later one of them
 * would have arrived in somebody's pull request.  main.c points this at HOME
 * once that volume is registered.  Workspace-scoped files (TASKS.JSN,
 * LAUNCH.JSN) do not come through here - they are read from UC.ws_dir on the
 * workspace volume, which is where they belong. */
static int g_pref_vol;
void host_fs_set_pref_vol(int vol) { if (vol >= 0 && vol < g_nvol) g_pref_vol = vol; }
int  uno_fs_pref_vol(void)         { return g_pref_vol; }

/* -1 for every volume: the core then lists through uno_fs_list_dir and probes
 * directories with uno_fs_isdir, both of which this host answers truthfully.
 * (The FAT road's uno_fat_entry.name is char[13]; this road's is char[16].) */
int uno_fs_fat_index(int vol) { (void)vol; return -1; }

long uno_fs_size(int vol, const char *name)
{
    char hp[MAXPATH];
    struct stat st;
    if (!resolve(vol, name, hp, 0)) return -1;
    if (stat(hp, &st) != 0 || (st.st_mode & S_IFMT) != S_IFREG) return -1;
    return (long)st.st_size;
}

long uno_fs_read(int vol, const char *name, unsigned char *buf, long max)
{
    char hp[MAXPATH];
    FILE *f;
    size_t n;
    if (max < 0 || !resolve(vol, name, hp, 0)) return -1;
    f = fopen(hp, "rb");
    if (!f) return -1;
    n = fread(buf, 1, (size_t)max, f);
    if (ferror(f)) { fclose(f); return -1; }
    fclose(f);
    return (long)n;
}

int uno_fs_write(int vol, const char *name, const unsigned char *buf, long len)
{
    char hp[MAXPATH];
    FILE *f;
    size_t n;
    if (len < 0 || vol < 0 || vol >= g_nvol || !g_vol[vol].writable) return 0;
    if (!resolve(vol, name, hp, 1)) return 0;
    f = fopen(hp, "wb");
    if (!f) return 0;
    n = fwrite(buf, 1, (size_t)len, f);
    if (fclose(f) != 0) return 0;
    return n == (size_t)len;
}

int uno_fs_isdir(int vol, const char *path)
{
    char hp[MAXPATH];
    struct stat st;
    if (!path || !path[0]) return (vol >= 0 && vol < g_nvol);  /* root */
    if (!resolve(vol, path, hp, 0)) return 0;
    return stat(hp, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
}

int uno_fs_mkdir(int vol, const char *path)
{
    char hp[MAXPATH];
    if (vol < 0 || vol >= g_nvol || !g_vol[vol].writable) return 0;
    if (!resolve(vol, path, hp, 1)) return 0;
    if (uno_fs_isdir(vol, path)) return 1;         /* already there is fine  */
    return host_mkdir(hp) == 0;
}

/* Shared listing walk.  Names cross VERBATIM now (UCD-11): the seam takes the
 * caller's stride, so the only reason to shorten one would be a slot too small
 * to hold it, and a name that does not fit is WITHHELD rather than truncated -
 * a truncated name is a file the core would then fail to open, which is worse
 * than a file it never saw.  Dotfiles and "." / ".." are withheld outright:
 * the core has no notion of a hidden file, and its own config subtree should
 * not look different on the host than it does on a stick. */
static int list_into(int vol, const char *dir, char *names, int stride, int maxn)
{
    char hp[MAXPATH];
    DIR *d;
    struct dirent *e;
    int n = 0;
    if (!resolve(vol, dir, hp, 0) || !names || stride < 2) return 0;
    d = opendir(hp);
    if (!d) return 0;
    while (n < maxn && (e = readdir(d)) != 0) {
        if (e->d_name[0] == '.') continue;
        if ((int)strlen(e->d_name) > stride - 1) continue;
        strcpy(names + (size_t)n * stride, e->d_name);
        n++;
    }
    closedir(d);
    return n;
}

int uno_fs_list_dir(int vol, const char *dir, char *names, int stride, int maxn)
{
    return list_into(vol, dir, names, stride, maxn);
}

/* root listing, two-call form */
static char g_rootlist[256][UC_HOST_NAME_MAX];
static int  g_rootn;

int uno_fs_list_begin(int vol)
{
    g_rootn = list_into(vol, "", g_rootlist[0], UC_HOST_NAME_MAX, 256);
    return g_rootn;
}

int uno_fs_list_get(int vol, int idx, char *name, int max)
{
    (void)vol;
    if (idx < 0 || idx >= g_rootn || max <= 0) return 0;
    strncpy(name, g_rootlist[idx], max - 1);
    name[max - 1] = 0;
    return 1;
}

/* Reached only through a fat index, which this host never hands out (the
 * terminal already prints "this volume cannot delete" on fi < 0).  Kept as a
 * link-satisfying honest failure rather than a working deleter so that file
 * deletion arrives deliberately, with the long-name story, in phase 1. */
int uno_fat_delete(int vol, const char *path)
{
    (void)vol; (void)path;
    return 0;
}

int uno_fat_list_ex(int vol, const char *dir, void *ents, int maxn)
{
    (void)vol; (void)dir; (void)ents; (void)maxn;
    return -1;
}
