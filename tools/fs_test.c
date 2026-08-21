/* ===========================================================================
 * fs_test.c - the uno_fs_* seam's own test, with the WIDENED listing seam
 * (UCD-11) as its subject.
 *
 * This links host_fs.c directly rather than driving the editor, because what
 * needs proving is a CONTRACT, not a picture: that a name of any length the
 * caller has room for crosses VERBATIM, that reading and writing it reaches
 * the file it names, that a long directory is descendable, and that a name the
 * caller has NO room for is withheld rather than truncated - a truncated name
 * is a file the core would then fail to open, which is worse than one it never
 * saw.
 *
 * Until UCD-11 this file tested the opposite: a FAT-style alias table that
 * squeezed real names through a 15-byte seam.  The seam takes the caller's
 * STRIDE now, so the table and its four caps are gone, and the tests that
 * proved it worked are gone with it.  What replaced them is the round trip the
 * table existed to fake.
 *
 *   cc tools/fs_test.c host/host_fs.c -o fs_test
 *   ./fs_test <scratch-dir>
 * ======================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

#ifdef _WIN32
#  include <direct.h>
#  define t_mkdir(p) _mkdir(p)
#else
#  define t_mkdir(p) mkdir(p, 0755)
#endif

/* The width the EDITOR uses.  Declared here rather than included, because
 * unocode.h is subsystem-internal; the point of the stride is that this
 * number is the caller's business and need agree with nobody. */
#define NAME_MAX_T 256

int  host_fs_add_volume(const char *name, const char *root, int writable);
int  uno_fs_list_dir(int vol, const char *dir, char *names, int stride, int maxn);
long uno_fs_read(int vol, const char *name, unsigned char *buf, long max);
long uno_fs_size(int vol, const char *name);
int  uno_fs_write(int vol, const char *name, const unsigned char *buf, long len);
int  uno_fs_isdir(int vol, const char *path);

static int g_fail;
static char g_ws[1024];

static void ok(int cond, const char *what)
{
    printf("%s %s\n", cond ? "  ok  " : "  FAIL", what);
    if (!cond) g_fail++;
}

/* ---- scratch tree ---------------------------------------------------------- */

static void wpath(char *out, int cap, const char *rel)
{
    snprintf(out, (size_t)cap, "%s/%s", g_ws, rel);
}

static void mk_file(const char *rel, const char *body)
{
    char p[1024];
    FILE *f;
    wpath(p, sizeof p, rel);
    f = fopen(p, "wb");
    if (!f) { fprintf(stderr, "cannot create %s\n", p); exit(2); }
    fputs(body, f);
    fclose(f);
}

static void mk_dir(const char *rel)
{
    char p[1024];
    wpath(p, sizeof p, rel);
    t_mkdir(p);
}

static int count_entries(const char *rel)
{
    char p[1024];
    DIR *d;
    struct dirent *e;
    int n = 0;
    wpath(p, sizeof p, rel);
    d = opendir(p[0] ? p : g_ws);
    if (!d) return -1;
    while ((e = readdir(d)) != 0) if (e->d_name[0] != '.') n++;
    closedir(d);
    return n;
}

static int slurp(const char *rel, char *buf, int cap)
{
    char p[1024];
    FILE *f;
    int n;
    wpath(p, sizeof p, rel);
    f = fopen(p, "rb");
    if (!f) return -1;
    n = (int)fread(buf, 1, (size_t)cap - 1, f);
    fclose(f);
    buf[n > 0 ? n : 0] = 0;
    return n;
}

/* the listing is a flat block of `stride`-byte slots, exactly as the seam
 * writes it - indexing it by hand is the point, not an inconvenience */
static char *at(char *names, int stride, int i) { return names + (size_t)i * stride; }

static int has_exact(char *names, int stride, int n, const char *want)
{
    int i;
    for (i = 0; i < n; i++) if (!strcmp(at(names, stride, i), want)) return 1;
    return 0;
}

/* ---- the tests ------------------------------------------------------------- */

#define LONG_A "VeryLongComponentName.tsx"
#define LONG_B "VeryLongComponentOther.tsx"
#define LONG_D "ALongDirectoryNameHere"
#define LONG_I "InnerVeryLongFileName.md"

int main(int argc, char **argv)
{
    static char names[64][NAME_MAX_T], again[64][NAME_MAX_T];
    char buf[512], sub[1024];
    int n, n2, i;

    if (argc < 2) { fprintf(stderr, "usage: fs_test <scratch-dir>\n"); return 2; }
    snprintf(g_ws, sizeof g_ws, "%s", argv[1]);
    t_mkdir(g_ws);

    mk_file("HELLO.C",  "int main(void) { return 0; }\n");
    mk_file(LONG_A,     "AAA the first long file\n");
    mk_file(LONG_B,     "BBB the second long file\n");
    mk_dir(LONG_D);
    mk_file(LONG_D "/" LONG_I, "III the nested long file\n");
    mk_file(LONG_D "/SHORT.md", "short\n");

    if (host_fs_add_volume("WORK", g_ws, 1) != 0) {
        fprintf(stderr, "volume 0 did not register\n");
        return 2;
    }

    printf("1. every name crosses VERBATIM - no alias, no truncation\n");
    n = uno_fs_list_dir(0, "", names[0], NAME_MAX_T, 64);
    ok(n == 4, "the root lists all four entries");
    ok(has_exact(names[0], NAME_MAX_T, n, "HELLO.C"), "a short name is itself");
    ok(has_exact(names[0], NAME_MAX_T, n, LONG_A), "a 25-byte file name is itself");
    ok(has_exact(names[0], NAME_MAX_T, n, LONG_B), "and so is its near-twin");
    ok(has_exact(names[0], NAME_MAX_T, n, LONG_D), "a long DIRECTORY name is itself");
    {
        int tilde = 0;
        for (i = 0; i < n; i++) if (strchr(at(names[0], NAME_MAX_T, i), '~')) tilde++;
        ok(tilde == 0, "nothing was given a ~1 alias");
    }

    printf("2. a long name READS the file it names\n");
    {
        long got = uno_fs_read(0, LONG_A, (unsigned char *)buf, sizeof buf - 1);
        if (got < 0) got = 0;
        buf[got] = 0;
        ok(got > 0 && buf[0] == 'A', "the first long file reads as itself");
        ok(uno_fs_size(0, LONG_A) == got, "size agrees with read");
        got = uno_fs_read(0, LONG_B, (unsigned char *)sub, sizeof sub - 1);
        if (got < 0) got = 0;
        sub[got] = 0;
        ok(got > 0 && sub[0] == 'B', "the second reads as ITSELF, not the first");
    }

    printf("3. paths are still case-insensitive, as the core's are\n");
    {
        char up[NAME_MAX_T];
        long got;
        strcpy(up, LONG_A);
        for (i = 0; up[i]; i++) if (up[i] >= 'a' && up[i] <= 'z') up[i] -= 32;
        got = uno_fs_read(0, up, (unsigned char *)buf, sizeof buf - 1);
        ok(got > 0, "an upper-cased long name still resolves");
    }

    printf("4. a long name SAVES back to itself\n");
    {
        const char *body = "AAA edited through the full name\n";
        int before = count_entries("");
        int wrote  = uno_fs_write(0, LONG_A, (const unsigned char *)body,
                                  (long)strlen(body));
        int after  = count_entries("");
        char back[512];
        int rl;
        ok(wrote == 1, "the write reports success");
        ok(after == before, "no second file appeared beside the original");
        rl = slurp(LONG_A, back, sizeof back);
        ok(rl > 0 && !strcmp(back, body),
           "the file ON DISK under its real name holds the new text");
        rl = slurp(LONG_B, back, sizeof back);
        ok(rl > 0 && back[0] == 'B', "and its near-twin was not touched");
    }

    printf("5. a re-listing is identical\n");
    n2 = uno_fs_list_dir(0, "", again[0], NAME_MAX_T, 64);
    ok(n2 == n, "the same number of entries comes back");
    {
        int same = 1;
        for (i = 0; i < n2; i++)
            if (!has_exact(names[0], NAME_MAX_T, n, at(again[0], NAME_MAX_T, i)))
                same = 0;
        ok(same, "every name is the one handed out the first time");
    }

    printf("6. a long DIRECTORY is descendable, and nests\n");
    ok(uno_fs_isdir(0, LONG_D) == 1, "it reports as a directory");
    {
        static char inner[64][NAME_MAX_T];
        int m = uno_fs_list_dir(0, LONG_D, inner[0], NAME_MAX_T, 64);
        long got;
        ok(m == 2, "it lists both of its entries");
        ok(has_exact(inner[0], NAME_MAX_T, m, "SHORT.md"), "the short one is itself");
        ok(has_exact(inner[0], NAME_MAX_T, m, LONG_I), "the long one is itself");
        snprintf(sub, sizeof sub, "%s\\%s", LONG_D, LONG_I);
        got = uno_fs_read(0, sub, (unsigned char *)buf, sizeof buf - 1);
        if (got < 0) got = 0;
        buf[got] = 0;
        ok(got > 0 && buf[0] == 'I',
           "a long name INSIDE a long directory reads the nested file");
    }

    printf("7. the caller's STRIDE is what bounds a name\n");
    {
        /* The same directory, listed into narrow slots.  Anything that does
         * not fit is withheld - never truncated, because a truncated name
         * would be handed back as a file that cannot be opened. */
        static char narrow[64][16];
        int m = uno_fs_list_dir(0, "", narrow[0], 16, 64), fits = 1;
        for (i = 0; i < m; i++) if (strlen(at(narrow[0], 16, i)) > 15) fits = 0;
        ok(m == 1, "at stride 16 only the short name is offered");
        ok(has_exact(narrow[0], 16, m, "HELLO.C"), "and it is the right one");
        ok(fits, "nothing longer than the slot came back");
        ok(n == 4, "the same directory at stride 256 still offered all four");
    }

    printf("8. the fence: stride-1 fits, stride does not\n");
    {
        static char slot[8][32];
        char fits[64], over[64], rel[128];
        int m;
        /* Lengths are COMPUTED, not spelled out: the first draft of this test
         * hand-counted two names as 31 and 32 bytes, and they were 27 and 28,
         * so it asserted the fence while standing nowhere near it. */
        memset(fits, 'A', 27); strcpy(fits + 27, ".txt");   /* 31 bytes */
        memset(over, 'B', 28); strcpy(over + 28, ".txt");   /* 32 bytes */
        ok(strlen(fits) == 31 && strlen(over) == 32,
           "the two probe names really are 31 and 32 bytes");
        mk_dir("fence");
        snprintf(rel, sizeof rel, "fence/%s", fits);
        mk_file(rel, "fits\n");
        snprintf(rel, sizeof rel, "fence/%s", over);
        mk_file(rel, "over\n");

        m = uno_fs_list_dir(0, "fence", slot[0], 32, 8);
        ok(m == 1, "at stride 32 only the 31-byte name fits");
        ok(has_exact(slot[0], 32, m, fits), "and it came back whole");
        {
            static char wide[8][64];
            int w = uno_fs_list_dir(0, "fence", wide[0], 64, 8);
            ok(w == 2, "at stride 64 BOTH are offered - the name never changed, "
                       "only the room for it");
            ok(has_exact(wide[0], 64, w, over), "including the 32-byte one");
        }
    }

    printf("\n%s: %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
