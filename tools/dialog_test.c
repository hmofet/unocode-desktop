/* ===========================================================================
 * dialog_test.c - what happens AFTER the Open dialog closes.
 *
 * The dialog itself is the OS's and is verified by opening it; the part that
 * can be wrong quietly is the translation on the way back.  The core addresses
 * files as (volume, directory, name) with volume 0 the workspace, so an
 * absolute path from a picker is only meaningful once it has been made
 * relative - and a path OUTSIDE the workspace has no address at all until the
 * workspace is moved to contain it.
 *
 * host_pick_path() is supplied here instead of the platform's, which is the
 * whole point: the dialog is stubbed, the plumbing is real.
 *
 *   cc tools/dialog_test.c host/host_dialog.c host/host_fs.c -o dialog_test
 * ======================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#  include <direct.h>
#  define t_mkdir(p) _mkdir(p)
#else
#  include <sys/types.h>
#  define t_mkdir(p) mkdir(p, 0755)
#endif

int  host_fs_add_volume(const char *name, const char *root, int writable);
int  host_fs_set_volume_root(int vol, const char *root);
int  host_fs_abspath(const char *in, char *out, int cap);
int  uno_fs_list_dir(int vol, const char *dir, char *names, int stride, int maxn);
long uno_fs_read(int vol, const char *name, unsigned char *buf, long max);
void host_dialog_set_root(const char *root);
const char *host_dialog_root(void);
void host_recent_file(const char *home);
void host_recent_load(void);
void host_recent_add(const char *path);
int  host_recent_count(void);
const char *host_recent_at(int i);
int  pc64_shell_pick(int want_folder, int *vol, char *dir, int dcap,
                     char *name, int ncap);
int  host_adopt_path(const char *abs);

/* ---- the stubs the real dialog would provide ------------------------------ */
static char g_next[1024];          /* what the "picker" will return next */
static int  g_cancel;
static int  g_reroot_calls;

int host_pick_path(int want_folder, char *out, int cap)
{
    (void)want_folder;
    if (g_cancel || !g_next[0]) return 0;
    snprintf(out, (size_t)cap, "%s", g_next);
    return 1;
}

/* the core's; here it only has to record that it was asked to re-root */
void uc_open_folder(int vol, const char *dir)
{
    (void)vol; (void)dir;
    g_reroot_calls++;
}

/* the core's document opener, recorded rather than performed - host_adopt_path
 * (UCD-19) calls it, and what this test proves is which (dir, name) a dropped
 * path resolves to, not that a buffer got loaded */
static char g_opened_dir[512], g_opened_name[256];
static int  g_open_calls;
int uc_doc_open(int vol, const char *dir, const char *name)
{
    (void)vol;
    snprintf(g_opened_dir, sizeof g_opened_dir, "%s", dir ? dir : "");
    snprintf(g_opened_name, sizeof g_opened_name, "%s", name ? name : "");
    g_open_calls++;
    return 0;
}

/* ---- harness --------------------------------------------------------------- */
static int g_fail;
static char WS[400];   /* short, so the compiler can see the joins fit */

static void ok(int cond, const char *what)
{
    printf("%s %s\n", cond ? "  ok  " : "  FAIL", what);
    if (!cond) g_fail++;
}

static void mk(const char *rel, const char *body)
{
    char p[1024];
    FILE *f;
    snprintf(p, sizeof p, "%s/%s", WS, rel);
    f = fopen(p, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", p); exit(2); }
    fputs(body, f);
    fclose(f);
}

static void mkd(const char *rel)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", WS, rel);
    t_mkdir(p);
}

static int listed(const char *dir, const char *want)
{
    char names[64][256];
    int n = uno_fs_list_dir(0, dir, names[0], 256, 64), i;
    for (i = 0; i < n; i++) if (!strcmp(names[i], want)) return 1;
    return 0;
}

int main(int argc, char **argv)
{
    char abs[1024], dir[128], name[64];
    int vol = -1;

    if (argc < 2) { fprintf(stderr, "usage: dialog_test <scratch>\n"); return 2; }
    t_mkdir(argv[1]);
    host_fs_abspath(argv[1], WS, sizeof WS);

    mkd("alpha");     mk("alpha/INSIDE.C",  "inside\n");
    mkd("alpha/sub"); mk("alpha/sub/DEEP.C", "deep\n");
    mkd("beta");      mk("beta/OTHER.PY",   "other\n");

    snprintf(abs, sizeof abs, "%s/alpha", WS);
    host_fs_add_volume("WORK", abs, 1);
    host_dialog_set_root(abs);
    host_recent_file(WS);
    host_recent_load();

    printf("1. a file INSIDE the workspace becomes a relative address\n");
    snprintf(g_next, sizeof g_next, "%s/alpha/INSIDE.C", WS);
    ok(pc64_shell_pick(0, &vol, dir, sizeof dir, name, sizeof name) == 1,
       "the pick is accepted");
    ok(vol == 0, "on the workspace volume");
    ok(!strcmp(dir, ""), "at the volume root");
    ok(!strcmp(name, "INSIDE.C"), "under its own name");
    ok(g_reroot_calls == 0, "and the workspace did NOT move");

    printf("2. a file in a SUBDIRECTORY keeps its directory\n");
    snprintf(g_next, sizeof g_next, "%s/alpha/sub/DEEP.C", WS);
    ok(pc64_shell_pick(0, &vol, dir, sizeof dir, name, sizeof name) == 1, "accepted");
    ok(!strcmp(dir, "sub"), "the directory is workspace-relative");
    ok(!strcmp(name, "DEEP.C"), "and the name is the name");

    printf("3. Open Folder RE-ROOTS the workspace volume, live\n");
    snprintf(g_next, sizeof g_next, "%s/beta", WS);
    ok(pc64_shell_pick(1, &vol, dir, sizeof dir, name, sizeof name) == 1, "accepted");
    ok(!strcmp(dir, ""), "the new folder is the volume root");
    ok(listed("", "OTHER.PY"), "the volume now lists the NEW folder's files");
    ok(!listed("", "INSIDE.C"), "and not the old folder's");
    {
        char cmp[1024];
        snprintf(cmp, sizeof cmp, "%s/beta", WS);
        ok(!strcmp(host_dialog_root(), cmp), "and the recorded root moved with it");
    }

    printf("4. a file OUTSIDE the workspace moves the workspace to reach it\n");
    g_reroot_calls = 0;
    snprintf(g_next, sizeof g_next, "%s/alpha/sub/DEEP.C", WS);   /* not under beta */
    ok(pc64_shell_pick(0, &vol, dir, sizeof dir, name, sizeof name) == 1, "accepted");
    ok(g_reroot_calls == 1, "the workspace was re-rooted to reach it");
    ok(!strcmp(name, "DEEP.C"), "and the file is addressed from the new root");
    ok(!strcmp(dir, ""), "which is its own folder");
    {
        unsigned char buf[64];
        long n = uno_fs_read(0, "DEEP.C", buf, sizeof buf - 1);
        if (n < 0) n = 0;
        buf[n] = 0;
        ok(!strcmp((char *)buf, "deep\n"), "and it READS as itself afterwards");
    }

    printf("5. a prefix that is not a directory boundary is not 'inside'\n");
    {
        /* "/alpha" must not swallow "/alphabet": a plain strncmp would */
        char sib[1024];
        snprintf(sib, sizeof sib, "%s/alphabet", WS);
        t_mkdir(sib);
        snprintf(sib, sizeof sib, "%s/alphabet/TRAP.C", WS);
        { FILE *f = fopen(sib, "wb"); if (f) { fputs("trap\n", f); fclose(f); } }
        snprintf(abs, sizeof abs, "%s/alpha", WS);
        host_fs_set_volume_root(0, abs);
        host_dialog_set_root(abs);
        g_reroot_calls = 0;
        snprintf(g_next, sizeof g_next, "%s/alphabet/TRAP.C", WS);
        ok(pc64_shell_pick(0, &vol, dir, sizeof dir, name, sizeof name) == 1, "accepted");
        ok(g_reroot_calls == 1, "it is treated as OUTSIDE, and re-roots");
        ok(!strcmp(name, "TRAP.C"), "and reaches the right file");
    }

    printf("6. cancelling changes nothing\n");
    g_cancel = 1;
    g_reroot_calls = 0;
    ok(pc64_shell_pick(0, &vol, dir, sizeof dir, name, sizeof name) == 0,
       "a cancelled pick reports failure");
    ok(g_reroot_calls == 0, "and does not move the workspace");
    g_cancel = 0;

    printf("7. the recent list is newest-first, deduplicated, and persists\n");
    {
        char a[1024], b[1024];
        snprintf(a, sizeof a, "%s/alpha", WS);
        snprintf(b, sizeof b, "%s/beta", WS);
        host_recent_add(a);
        host_recent_add(b);
        host_recent_add(a);                     /* again: must not duplicate */
        ok(!strcmp(host_recent_at(0), a), "the most recent is first");
        {
            int i, dup = 0;
            for (i = 1; i < host_recent_count(); i++)
                if (!strcmp(host_recent_at(i), a)) dup = 1;
            ok(!dup, "and appears exactly once");
        }
        /* reload from disk, as the next launch would */
        host_recent_load();
        ok(host_recent_count() >= 2, "the list survives a reload");
        ok(!strcmp(host_recent_at(0), a), "in the same order");
    }

    /* ---- dropped paths (UCD-19) --------------------------------------------
     * The drop handler is host_adopt_path(); SDL only hands it a string.  So
     * the whole of what a drop DOES is testable here, without a window - and
     * what matters is that it lands where the picker would. */
    printf("6. a dropped FILE inside the workspace opens in place\n");
    {
        int before;
        /* State the root rather than inheriting whatever the last test left -
         * the first draft of this assumed `beta` and test 5 had already moved
         * on, so it asserted "did not move" about a workspace that had every
         * reason to. */
        snprintf(abs, sizeof abs, "%s/beta", WS);
        host_fs_set_volume_root(0, abs);
        host_dialog_set_root(abs);
        before = g_reroot_calls;
        snprintf(abs, sizeof abs, "%s/beta/OTHER.PY", WS);
        g_open_calls = 0;
        ok(host_adopt_path(abs) == 1, "the drop is accepted");
        ok(g_open_calls == 1, "exactly one document was opened");
        ok(!strcmp(g_opened_name, "OTHER.PY"), "under its own name");
        ok(!strcmp(g_opened_dir, ""), "at the volume root");
        ok(g_reroot_calls == before, "and the workspace did NOT move");
    }

    printf("7. a dropped FOLDER becomes the workspace\n");
    {
        int before = g_reroot_calls;
        snprintf(abs, sizeof abs, "%s/alpha", WS);
        ok(host_adopt_path(abs) == 1, "the drop is accepted");
        ok(g_reroot_calls == before + 1, "the workspace was re-rooted once");
        ok(listed("", "INSIDE.C"), "and the volume lists the dropped folder");
        ok(!listed("", "OTHER.PY"), "not the one it replaced");
    }

    printf("8. a dropped file from OUTSIDE moves the workspace to it\n");
    {
        int before = g_reroot_calls;
        snprintf(abs, sizeof abs, "%s/beta/OTHER.PY", WS);   /* alpha is root now */
        g_open_calls = 0;
        ok(host_adopt_path(abs) == 1, "the drop is accepted");
        ok(g_reroot_calls == before + 1, "the workspace followed the file");
        ok(g_open_calls == 1 && !strcmp(g_opened_name, "OTHER.PY"),
           "and the file itself was opened");
        ok(listed("", "OTHER.PY"), "the volume is its folder now");
    }

    printf("9. a dropped path that does not exist is refused\n");
    {
        int before = g_reroot_calls;
        snprintf(abs, sizeof abs, "%s/nothing/here.txt", WS);
        ok(host_adopt_path(abs) == 0, "refused");
        ok(g_reroot_calls == before, "and nothing moved");
    }

    printf("\n%s: %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
