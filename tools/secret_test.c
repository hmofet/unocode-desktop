/* ===========================================================================
 * secret_test.c - uc_secret.h's contract, against the real platform store
 * (UCD-48).
 *
 * Links host_secret.c alone; no SDL, no display.  On Linux this drives the
 * 0600 file and ASSERTS THE MODE, because "a file only this user can read"
 * is a permission bits claim and permission bits are checkable.  On Windows
 * it drives DPAPI and asserts the VALUE IS NOT IN THE FILE, because "sealed"
 * is a claim about what a grep of the disk can find.  On macOS it drives the
 * real Keychain and cleans up after itself.
 *
 *   cc tools/secret_test.c host/host_secret.c -o secret_test
 *   ./secret_test <scratch-dir>
 * ======================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uc_secret.h"
#include "host.h"

#ifndef _WIN32
#include <sys/stat.h>
#endif

static int g_fail;

static void ok(int cond, const char *what)
{
    printf("%s  %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) g_fail = 1;
}

/* read the backing file, if this platform keeps one */
static long slurp(const char *dir, char *out, long cap)
{
    char path[1200];
    FILE *f;
    long n;
    snprintf(path, sizeof path, "%s/SECRETS.DAT", dir);
    f = fopen(path, "rb");
    if (!f) return -1;
    n = (long)fread(out, 1, (size_t)(cap - 1), f);
    fclose(f);
    out[n] = 0;
    return n;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    char buf[UC_SECRET_MAX];
    char file[16384];
    host_secret_dir(dir);

    printf("store: %s (plaintext=%d)\n",
           uc_secret_store_name(), uc_secret_plaintext());
    ok(uc_secret_store_name()[0] != 0, "the store names itself");

    /* a clean slate, in case an earlier run died */
    uc_secret_del("test.alpha");
    uc_secret_del("test.beta");

    ok(!uc_secret_get("test.alpha", buf, sizeof buf) && buf[0] == 0,
       "an absent secret reads as absent, and the buffer is emptied");

    ok(uc_secret_set("test.alpha", "sk-ant-EXAMPLE-0123456789"),
       "a secret stores");
    ok(uc_secret_get("test.alpha", buf, sizeof buf) &&
       !strcmp(buf, "sk-ant-EXAMPLE-0123456789"),
       "and reads back byte-identical");

    ok(uc_secret_set("test.alpha", "replaced"), "a secret replaces");
    ok(uc_secret_get("test.alpha", buf, sizeof buf) && !strcmp(buf, "replaced"),
       "and the replacement is what reads back");

    ok(uc_secret_set("test.beta", "a value with spaces\nand a newline"),
       "a value holding a space and a newline stores");
    ok(uc_secret_get("test.beta", buf, sizeof buf) &&
       !strcmp(buf, "a value with spaces\nand a newline"),
       "and survives the file format intact");
    ok(uc_secret_get("test.alpha", buf, sizeof buf) && !strcmp(buf, "replaced"),
       "storing beta did not disturb alpha");

    /* the length contract, both sides of the line */
    {
        char big[UC_SECRET_MAX + 8];
        memset(big, 'k', sizeof big);
        big[UC_SECRET_MAX] = 0;            /* strlen == UC_SECRET_MAX      */
        ok(!uc_secret_set("test.alpha", big),
           "a value at the UC_SECRET_MAX fence is refused");
        big[UC_SECRET_MAX - 1] = 0;        /* strlen == UC_SECRET_MAX - 1  */
        ok(uc_secret_set("test.alpha", big) &&
           uc_secret_get("test.alpha", buf, sizeof buf) && !strcmp(buf, big),
           "a value one under it round-trips");
        ok(uc_secret_set("test.alpha", "replaced"), "restore for later checks");
    }

    /* what the DISK holds, on the platforms that hold one */
    if (slurp(dir, file, sizeof file) >= 0) {
        if (uc_secret_plaintext())
            ok(1, "plaintext store: no concealment claimed, none checked");
        else
            ok(!strstr(file, "replaced"),
               "the sealed file does not contain the value in the clear");
#ifndef _WIN32
        {
            struct stat st;
            char path[1200];
            snprintf(path, sizeof path, "%s/SECRETS.DAT", dir);
            ok(stat(path, &st) == 0 && (st.st_mode & 0077) == 0,
               "the file is readable by this user alone (0600)");
        }
#endif
    } else if (!uc_secret_plaintext()) {
        ok(1, "no backing file of ours (the OS keychain holds it)");
    }

    ok(uc_secret_del("test.alpha"), "a secret deletes");
    ok(!uc_secret_get("test.alpha", buf, sizeof buf), "and is gone");
    ok(uc_secret_del("test.alpha"), "deleting the absent succeeds - one outcome");
    ok(uc_secret_get("test.beta", buf, sizeof buf),
       "deleting alpha did not take beta with it");
    uc_secret_del("test.beta");

    printf(g_fail ? "== secret_test: FAILED ==\n"
                  : "== secret_test: all passed ==\n");
    return g_fail;
}
