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
 *     one road of the two whose name buffers hold 15 characters, not 12;
 *   - a name too long for those 15 characters is given a FAT-style ALIAS
 *     ("VeryLongCo~1.tsx") which is what crosses the seam, and which resolve()
 *     turns back into the real name on the way to the host.  See below.
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

/* ---- long names: a FAT-style alias table ----------------------------------
 * The listing seam hands out char[16] buffers, so 15 bytes is all a name gets.
 * Withholding anything longer - the phase-0 behaviour - makes a real project
 * look half-empty, because most source files are named past that.  So do what
 * FAT itself did: give the long name a short ALIAS, hand the alias across the
 * seam, and translate it back here.
 *
 *   VeryLongComponentName.tsx   ->   VeryLongCo~1.tsx
 *
 * The extension is kept WHOLE (the core picks the language, the icon and the
 * grammar off it) and the base is truncated to fit, never mid-UTF-8-sequence.
 * A file is therefore listed, opened, edited and saved back to its original
 * name; only the DISPLAY is shortened.  UCD-11 widens the seam upstream and
 * this table goes away.
 *
 * Two properties this must have and does:
 *   - STABLE within a session.  An alias, once handed out, is remembered for
 *     the (volume, directory, name) it was made for, so a tab opened before an
 *     explorer refresh still points at the same file afterwards.  Candidates
 *     are probed in a deterministic order and the first free one wins, so a
 *     re-listing of an unchanged directory reproduces the same table.
 *   - BOUNDED, and loudly.  The table, its name pool and its directory list
 *     all have caps; hitting one is reported on stderr once, and names beyond
 *     it fall back to phase 0's behaviour (withheld) rather than being served
 *     under an alias that resolve() cannot honour.
 *
 * A candidate collides if another alias in the same directory already uses it,
 * or if a real file of that name is there.  The real-file probe is a single
 * stat(), not a directory scan: on a case-sensitive host a real file differing
 * only in case would slip past it, but resolve() consults this table first, so
 * the alias still reaches the file it was minted for. */

#define SEAM_NAME_MAX  15               /* char[16] minus the terminator     */
#define ALIAS_DIRKEY   128              /* core-side dir; UC_PATH_MAX is 72  */

/* The caps are -D-overridable so tools/fs_test.c can drive the full-table road
 * with eight names instead of four thousand.  ALIAS_HASH must stay a power of
 * two and at least twice ALIAS_MAX, or alias_find()'s probe never terminates
 * on a full table. */
#ifndef ALIAS_DIRS
#  define ALIAS_DIRS   128
#endif
#ifndef ALIAS_MAX
#  define ALIAS_MAX    4096
#endif
#ifndef ALIAS_POOL
#  define ALIAS_POOL   (192 * 1024)
#endif
#ifndef ALIAS_HASH
#  define ALIAS_HASH   (2 * ALIAS_MAX)
#endif

typedef struct {
    int  vol;
    char path[ALIAS_DIRKEY];            /* normalised core-side directory    */
} AliasDir;

typedef struct {
    int  dir;                           /* index into g_adir                 */
    int  off;                           /* real name, offset into g_apool    */
    char alias[16];                     /* the spelling that crosses the seam*/
} AliasEnt;

static AliasDir g_adir[ALIAS_DIRS];
static int      g_adirn;
static AliasEnt g_alias[ALIAS_MAX];
static int      g_aliasn;
static char     g_apool[ALIAS_POOL];
static int      g_apooln;
static int      g_ahash[ALIAS_HASH];    /* 1-based entry index; 0 = empty    */
static int      g_alias_full;           /* a cap was hit and has been logged */

static void alias_cap(const char *what)
{
    if (g_alias_full) return;
    g_alias_full = 1;
    fprintf(stderr, "host_fs: long-name alias table full (%s); files with "
                    "names over %d bytes are hidden from here on\n",
            what, SEAM_NAME_MAX);
}

/* "src\\Components\\" and "/SRC/components" both key the same directory */
static void norm_dir(const char *dir, char *out, int cap)
{
    int n = 0;
    const char *p = dir ? dir : "";
    while (*p == '\\' || *p == '/') p++;
    while (*p && n < cap - 1) {
        char c = *p++;
        if (c == '/') c = '\\';
        if (c == '\\') {
            while (*p == '\\' || *p == '/') p++;
            if (!*p) break;                       /* a trailing separator */
        }
        out[n++] = c;
    }
    out[n] = 0;
}

/* Index of (vol, dir) in the directory list, or -1.  A dir too long to key is
 * refused rather than truncated: a truncated key would alias a DIFFERENT
 * directory's names, and no alias is better than the wrong file. */
static int alias_dir_index(int vol, const char *dir, int create)
{
    char key[ALIAS_DIRKEY];
    int i;

    if (dir && strlen(dir) >= ALIAS_DIRKEY - 1) return -1;
    norm_dir(dir, key, sizeof key);
    for (i = 0; i < g_adirn; i++)
        if (g_adir[i].vol == vol && ieq(g_adir[i].path, key)) return i;
    if (!create) return -1;
    if (g_adirn >= ALIAS_DIRS) { alias_cap("directories"); return -1; }
    g_adir[g_adirn].vol = vol;
    strcpy(g_adir[g_adirn].path, key);
    return g_adirn++;
}

static unsigned alias_hash(int di, const char *alias)
{
    unsigned h = 2166136261u ^ (unsigned)di;
    for (; *alias; alias++) {
        int c = (unsigned char)*alias;
        if (c >= 'A' && c <= 'Z') c += 32;
        h = (h ^ (unsigned)c) * 16777619u;
    }
    return h;
}

/* The entry for (di, alias), or 0.  Open addressing, never deleted from, so a
 * run of probes ends at the first empty slot. */
static AliasEnt *alias_find(int di, const char *alias)
{
    unsigned i = alias_hash(di, alias) & (ALIAS_HASH - 1);
    if (di < 0) return 0;
    for (;;) {
        int slot = g_ahash[i];
        if (!slot) return 0;
        if (g_alias[slot - 1].dir == di && ieq(g_alias[slot - 1].alias, alias))
            return &g_alias[slot - 1];
        i = (i + 1) & (ALIAS_HASH - 1);
    }
}

static int alias_insert(int di, const char *alias, const char *real)
{
    int len = (int)strlen(real) + 1;
    unsigned i;
    if (g_aliasn >= ALIAS_MAX)       { alias_cap("names");     return 0; }
    if (g_apooln + len > ALIAS_POOL) { alias_cap("name pool"); return 0; }
    g_alias[g_aliasn].dir = di;
    g_alias[g_aliasn].off = g_apooln;
    strcpy(g_alias[g_aliasn].alias, alias);
    memcpy(g_apool + g_apooln, real, (size_t)len);
    g_apooln += len;
    i = alias_hash(di, alias) & (ALIAS_HASH - 1);
    while (g_ahash[i]) i = (i + 1) & (ALIAS_HASH - 1);
    g_ahash[i] = ++g_aliasn;
    return 1;
}

/* Compose the n-th alias candidate for `real`: <base>~<n><ext>, within
 * SEAM_NAME_MAX bytes.  `out` must hold 16. */
static void alias_build(const char *real, int n, char *out)
{
    char suf[8];
    const char *dot = 0, *p;
    int rl = (int)strlen(real), el, sl, keep, v, i;

    for (p = real + 1; *p; p++) if (*p == '.') dot = p;
    el = dot ? (int)(real + rl - dot) : 0;
    if (el > 8) el = 0;                   /* that is not a suffix, it is text */

    suf[0] = '~';
    for (v = n, sl = 1; v >= 10; v /= 10) sl++;
    suf[1 + sl] = 0;
    for (v = n, i = sl; i >= 1; i--) { suf[i] = (char)('0' + v % 10); v /= 10; }
    sl += 1;                                             /* the '~' itself   */

    keep = SEAM_NAME_MAX - el - sl;
    if (keep < 0) { el = 0; keep = SEAM_NAME_MAX - sl; }
    if (keep > rl - el) keep = rl - el;
    if (keep < 0) keep = 0;
    /* never split a UTF-8 sequence: back off the truncation to a lead byte */
    while (keep > 0 && ((unsigned char)real[keep] & 0xC0) == 0x80) keep--;

    memcpy(out, real, (size_t)keep);
    strcpy(out + keep, suf);
    if (el) memcpy(out + keep + sl, real + rl - el, (size_t)el);
    out[keep + sl + el] = 0;
}

/* The alias for `real` in `coredir` of `vol`, minting one if needed, or 0 if a
 * cap has been reached.  `hostdir` is that directory's resolved host path, for
 * the real-file collision probe. */
static const char *alias_for(int vol, const char *coredir, const char *hostdir,
                             const char *real)
{
    char cand[16], probe[MAXPATH];
    struct stat st;
    int di = alias_dir_index(vol, coredir, 1), n;

    if (di < 0) return 0;
    for (n = 1; n < 100000; n++) {
        AliasEnt *e;
        alias_build(real, n, cand);
        e = alias_find(di, cand);
        if (e) {
            if (!strcmp(g_apool + e->off, real)) return e->alias;  /* ours */
            continue;                                   /* somebody else's */
        }
        if (snprintf(probe, sizeof probe, "%s/%s", hostdir, cand)
                < (int)sizeof probe && stat(probe, &st) == 0)
            continue;                            /* a real file is named that */
        if (!alias_insert(di, cand, real)) return 0;
        return alias_find(di, cand)->alias;
    }
    return 0;
}

/* The real name behind `comp` in `coredir` of `vol`, or 0 if it is not an
 * alias.  Every alias carries a '~', so an ordinary name costs one strchr. */
static const char *alias_real(int vol, const char *coredir, const char *comp)
{
    AliasEnt *e;
    int di;
    if (!g_aliasn || !strchr(comp, '~')) return 0;
    di = alias_dir_index(vol, coredir, 0);
    if (di < 0) return 0;
    e = alias_find(di, comp);
    return e ? g_apool + e->off : 0;
}

/* Resolve a core-side path ("EXT\\HELLO\\MAIN.JS", any case) to a host path.
 * `create_leaf` lets a path whose FINAL component does not exist yet resolve
 * anyway (for writes and mkdir); every parent must exist either way.
 * Alias components are translated as they are walked, so a long name is a real
 * name again by the time it reaches the host - at every depth, not just the
 * last one, because a long DIRECTORY name is aliased too. */
static int resolve(int vol, const char *path, char *out, int create_leaf)
{
    char comp[MAXPATH], found[MAXPATH], key[ALIAS_DIRKEY];
    const char *p, *want, *real;
    int ci, kn = 0;

    if (vol < 0 || vol >= g_nvol) return 0;
    strncpy(out, g_vol[vol].root, MAXPATH - 1);
    out[MAXPATH - 1] = 0;
    key[0] = 0;
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

        real = kn >= 0 ? alias_real(vol, key, comp) : 0;
        want = real ? real : comp;

        if (match_component(out, want, found, sizeof found)) {
            /* fall through with the on-disk spelling */
        } else {
            const char *rest = p;
            while (*rest == '\\' || *rest == '/') rest++;
            if (!create_leaf || *rest) return 0;   /* a missing PARENT fails */
            snprintf(found, sizeof found, "%s", want);
        }
        if (strlen(out) + 1 + strlen(found) + 1 > MAXPATH) return 0;
        strcat(out, "/");
        strcat(out, found);

        /* the key tracks the CORE's spelling, which is what list_into keyed */
        if (kn >= 0) {
            int need = (kn ? 1 : 0) + ci;
            if (need + kn >= ALIAS_DIRKEY) kn = -1;      /* too deep to alias */
            else {
                if (kn) key[kn++] = '\\';
                memcpy(key + kn, comp, (size_t)ci);
                kn += ci;
                key[kn] = 0;
            }
        }
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

/* Shared listing walk.  A name that does not fit the seam's 15 bytes is served
 * under an alias (see the alias table above) and only withheld if the table is
 * full; dotfiles and "." / ".." are withheld outright - the core has no notion
 * of a hidden file, and its own config subtree should not look different on
 * the host than it does on a stick. */
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
        if (strlen(e->d_name) <= SEAM_NAME_MAX) {
            strcpy(names[n++], e->d_name);
        } else {
            const char *a = alias_for(vol, dir, hp, e->d_name);
            if (a) strcpy(names[n++], a);
        }
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
