/* ===========================================================================
 * host_fs.c - the uno_fs_* seam over a real operating system's filesystem.
 *
 * The editor core speaks FAT: integer volumes, '\\'-joined paths, upper-cased
 * names, and listings capped at 15 characters.  This file makes a directory
 * tree on the host answer in that dialect:
 *
 *   - each volume is a root directory registered at startup;
 *   - every path component is resolved CASE-INSENSITIVELY, because the core
 *     upper-cases paths (FAT does not care; ext4 does);
 *   - uno_fs_fat_index() answers -1 for every volume, which sends the core
 *     down its documented fallback road (uno_fs_list_dir + uno_fs_isdir), the
 *     one road of the two whose name buffers hold 15 characters, not 12.
 *     Names longer than 15 bytes are withheld from listings rather than
 *     truncated - a name that cannot round-trip through open() must not be
 *     shown as openable.  Widening the seam is the phase-1 upstream request.
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

#ifdef _WIN32
#  include <direct.h>
#  define host_mkdir(p) _mkdir(p)
#else
#  define host_mkdir(p) mkdir(p, 0755)
#endif

#define MAXVOL   8
#define MAXPATH  1024

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
        strncpy(out, want, cap - 1);
        out[cap - 1] = 0;
        return 1;
    }
    d = opendir(dir);
    if (!d) return 0;
    while ((e = readdir(d)) != 0) {
        if (ieq(e->d_name, want)) {
            strncpy(out, e->d_name, cap - 1);
            out[cap - 1] = 0;
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
}

/* Resolve a core-side path ("EXT\\HELLO\\MAIN.JS", any case) to a host path.
 * `create_leaf` lets a path whose FINAL component does not exist yet resolve
 * anyway (for writes and mkdir); every parent must exist either way. */
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

        if (match_component(out, comp, found, sizeof found)) {
            /* fall through with the on-disk spelling */
        } else {
            const char *rest = p;
            while (*rest == '\\' || *rest == '/') rest++;
            if (!create_leaf || *rest) return 0;   /* a missing PARENT fails */
            strncpy(found, comp, sizeof found - 1);
            found[sizeof found - 1] = 0;
        }
        if (strlen(out) + 1 + strlen(found) + 1 > MAXPATH) return 0;
        strcat(out, "/");
        strcat(out, found);
    }
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
int uno_fs_pref_vol(void) { return 0; }

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

/* Shared listing walk.  Entries whose names exceed `namecap - 1` bytes are
 * withheld (see the file header); dotfiles and "." / ".." are too - the core
 * has no notion of a hidden file, and its own config subtree should not look
 * different on the host than it does on a stick. */
static int list_into(int vol, const char *dir, char (*names)[16], int maxn)
{
    char hp[MAXPATH];
    DIR *d;
    struct dirent *e;
    int n = 0;
    if (!resolve(vol, dir, hp, 0)) return 0;
    d = opendir(hp);
    if (!d) return 0;
    while (n < maxn && (e = readdir(d)) != 0) {
        if (e->d_name[0] == '.') continue;
        if (strlen(e->d_name) > 15) continue;
        strcpy(names[n], e->d_name);
        n++;
    }
    closedir(d);
    return n;
}

int uno_fs_list_dir(int vol, const char *dir, char (*names)[16], int maxn)
{
    return list_into(vol, dir, names, maxn);
}

/* root listing, two-call form */
static char g_rootlist[256][16];
static int  g_rootn;

int uno_fs_list_begin(int vol)
{
    g_rootn = list_into(vol, "", g_rootlist, 256);
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
