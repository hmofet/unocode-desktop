/* ===========================================================================
 * fs_test.c - the uno_fs_* seam's own test, with the long-name alias table
 * (UCD-01) as its subject.
 *
 * This links host_fs.c directly rather than driving the editor, because what
 * needs proving is a CONTRACT, not a picture: that a name too long for the
 * seam's char[16] is listed, that the alias it is listed under reads and
 * writes the original file, that it does not change under the caller's feet,
 * and that the table's caps are announced rather than silently hit.
 *
 * Built with small caps (-DALIAS_MAX=8 ...) so the full-table road is reached
 * with eight names instead of four thousand.
 *
 *   cc -DALIAS_MAX=8 -DALIAS_DIRS=4 tools/fs_test.c host/host_fs.c -o fs_test
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

int  host_fs_add_volume(const char *name, const char *root, int writable);
int  uno_fs_list_dir(int vol, const char *dir, char (*names)[16], int maxn);
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

/* the listed name that starts with `pfx`, or 0 */
static const char *find_pfx(char (*names)[16], int n, const char *pfx)
{
    int i, l = (int)strlen(pfx);
    for (i = 0; i < n; i++) if (!strncmp(names[i], pfx, (size_t)l)) return names[i];
    return 0;
}

static int has_exact(char (*names)[16], int n, const char *want)
{
    int i;
    for (i = 0; i < n; i++) if (!strcmp(names[i], want)) return 1;
    return 0;
}

/* ---- the tests ------------------------------------------------------------- */

#define LONG_A "VeryLongComponentName.tsx"
#define LONG_B "VeryLongComponentOther.tsx"
#define LONG_D "ALongDirectoryNameHere"
#define LONG_I "InnerVeryLongFileName.md"

int main(int argc, char **argv)
{
    char names[64][16], again[64][16];
    char alias_a[16], alias_b[16], alias_d[16], alias_i[16];
    char buf[512], sub[256];
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

    printf("1. a long name is LISTED, not withheld\n");
    n = uno_fs_list_dir(0, "", names, 64);
    ok(n == 4, "the root lists all four entries");
    ok(has_exact(names, n, "HELLO.C"), "a short name crosses the seam verbatim");
    {
        const char *a = find_pfx(names, n, "VeryLong");
        const char *d = find_pfx(names, n, "ALongDir");
        ok(a != 0, "the long file appears under an alias");
        ok(d != 0, "the long directory appears under an alias");
        if (!a || !d) { printf("cannot continue\n"); return 1; }
    }
    {
        int fits = 1;
        for (i = 0; i < n; i++) if (strlen(names[i]) > 15) fits = 0;
        ok(fits, "every listed name fits the seam");
    }

    printf("2. the alias keeps the extension, so the language still resolves\n");
    {
        int tsx = 0;
        for (i = 0; i < n; i++) {
            size_t l = strlen(names[i]);
            if (l > 4 && !strcmp(names[i] + l - 4, ".tsx")) tsx++;
        }
        ok(tsx == 2, "both .tsx files still end in .tsx");
    }

    printf("3. two long names sharing a base get DISTINCT aliases\n");
    {
        int seen = 0;
        alias_a[0] = alias_b[0] = 0;
        for (i = 0; i < n; i++) {
            size_t l = strlen(names[i]);
            if (l > 4 && !strcmp(names[i] + l - 4, ".tsx")) {
                if (!seen++) strcpy(alias_a, names[i]);
                else         strcpy(alias_b, names[i]);
            }
            if (!strncmp(names[i], "ALongDir", 8)) strcpy(alias_d, names[i]);
        }
        ok(strcmp(alias_a, alias_b) != 0, "the two aliases differ");
        printf("       %s -> one of {%s, %s}\n", "the .tsx pair", alias_a, alias_b);
    }

    printf("4. the alias READS the file it stands for\n");
    {
        long got = uno_fs_read(0, alias_a, (unsigned char *)buf, sizeof buf - 1);
        int which_a, which_b;
        if (got < 0) got = 0;
        buf[got] = 0;
        which_a = buf[0] == 'A' || buf[0] == 'B';
        ok(got > 0 && which_a, "reading alias A returns one of the long files");
        ok(uno_fs_size(0, alias_a) == got, "size agrees with read");

        got = uno_fs_read(0, alias_b, (unsigned char *)sub, sizeof sub - 1);
        if (got < 0) got = 0;
        sub[got] = 0;
        which_b = sub[0] == 'A' || sub[0] == 'B';
        ok(got > 0 && which_b, "reading alias B returns one of the long files");
        ok(buf[0] != sub[0], "the two aliases reach DIFFERENT files");
    }

    printf("5. the alias is case-insensitive, as the core's paths are\n");
    {
        char up[16];
        long got;
        strcpy(up, alias_a);
        for (i = 0; up[i]; i++) if (up[i] >= 'a' && up[i] <= 'z') up[i] -= 32;
        got = uno_fs_read(0, up, (unsigned char *)buf, sizeof buf - 1);
        ok(got > 0, "an upper-cased alias still resolves");
    }

    printf("6. the alias SAVES back to the original name\n");
    {
        const char *body = "AAA edited through the alias\n";
        int before = count_entries("");
        int wrote  = uno_fs_write(0, alias_a, (const unsigned char *)body,
                                  (long)strlen(body));
        int after  = count_entries("");
        char back[512];
        int rl;
        ok(wrote == 1, "the write reports success");
        ok(after == before, "no new file was created beside the original");
        /* whichever of the two the alias stands for, one of them now holds it */
        rl = slurp(LONG_A, back, sizeof back);
        if (rl <= 0 || strcmp(back, body) != 0)
            rl = slurp(LONG_B, back, sizeof back);
        ok(rl > 0 && !strcmp(back, body),
           "the ORIGINAL long-named file on disk holds the new text");
    }

    printf("7. aliases are STABLE across a re-listing\n");
    n2 = uno_fs_list_dir(0, "", again, 64);
    ok(n2 == n, "the same number of entries comes back");
    {
        int same = 1;
        for (i = 0; i < n2; i++) if (!has_exact(names, n, again[i])) same = 0;
        ok(same, "every name is the one handed out the first time");
    }

    printf("8. a long DIRECTORY name is descendable\n");
    ok(uno_fs_isdir(0, alias_d) == 1, "the directory alias reports as a directory");
    {
        char inner[64][16];
        int m = uno_fs_list_dir(0, alias_d, inner, 64);
        const char *ia = find_pfx(inner, m, "InnerVery");
        ok(m == 2, "the aliased directory lists both of its entries");
        ok(has_exact(inner, m, "SHORT.md"), "its short name is verbatim");
        ok(ia != 0, "its long name is aliased too");
        if (ia) {
            long got;
            strcpy(alias_i, ia);
            snprintf(sub, sizeof sub, "%s\\%s", alias_d, alias_i);
            got = uno_fs_read(0, sub, (unsigned char *)buf, sizeof buf - 1);
            if (got < 0) got = 0;
            buf[got] = 0;
            ok(got > 0 && buf[0] == 'I',
               "an alias INSIDE an aliased directory reads the nested file");
        }
    }

    printf("9. a real file named like an alias is not stolen\n");
    {
        char decoy[64][16];
        int m;
        /* Plant a real file whose name IS the alias the long name below would
         * otherwise be given ("ClashingNameHere.txt" -> 15 - 4 - 2 = 9 base
         * bytes + "~1" + ".txt"), then add that long name.  DECOY is short
         * enough to cross the seam on its own, so both must be listed and each
         * must read as itself. */
        static const char *DECOY = "ClashingN~1.txt";
        mk_dir("collide");
        mk_file("collide/ClashingN~1.txt",     "decoy\n");
        mk_file("collide/ClashingNameHere.txt", "real\n");
        m = uno_fs_list_dir(0, "collide", decoy, 64);
        ok(m == 2, "both the decoy and the long name are listed");
        ok(has_exact(decoy, m, DECOY), "the decoy keeps its name");
        {
            const char *a = 0;
            for (i = 0; i < m; i++)
                if (strcmp(decoy[i], DECOY)) a = decoy[i];
            ok(a && strcmp(a, DECOY),
               "the long name got a DIFFERENT alias");
            if (a) {
                long got;
                /* %.15s, not %s: a seam name is at most 15 bytes, and saying
                 * so is what stops the compiler assuming the worst */
                snprintf(sub, sizeof sub, "collide\\%.15s", a);
                got = uno_fs_read(0, sub, (unsigned char *)buf, sizeof buf - 1);
                if (got < 0) got = 0;
                buf[got] = 0;
                ok(!strcmp(buf, "real\n"), "and it reads the long file, not the decoy");
                snprintf(sub, sizeof sub, "collide\\%s", DECOY);
                got = uno_fs_read(0, sub, (unsigned char *)buf, sizeof buf - 1);
                if (got < 0) got = 0;
                buf[got] = 0;
                ok(!strcmp(buf, "decoy\n"), "and the decoy still reads as itself");
            }
        }
    }

    printf("10. the cap is ANNOUNCED, and over-cap names are withheld\n");
    {
        char full[64][16];
        int m, aliased = 0;
        mk_dir("many");
        for (i = 0; i < 12; i++) {
            char rel[128];
            snprintf(rel, sizeof rel, "many/OverflowFileNumber%02d.txt", i);
            mk_file(rel, "x\n");
        }
        fflush(stdout);
        m = uno_fs_list_dir(0, "many", full, 64);
        for (i = 0; i < m; i++) if (strchr(full[i], '~')) aliased++;
        ok(m < 12, "past the cap, names are withheld rather than mis-served");
        ok(aliased == m, "everything that IS listed is a real alias");
        printf("       %d of 12 listed (ALIAS_MAX is %d)\n", m, ALIAS_MAX);
        {
            /* every alias that WAS handed out must still resolve */
            int good = 1;
            for (i = 0; i < m; i++) {
                snprintf(sub, sizeof sub, "many\\%.15s", full[i]);
                if (uno_fs_size(0, sub) != 2) good = 0;
            }
            ok(good, "every alias handed out under a full table still resolves");
        }
    }

    printf("\n%s: %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
