/* ===========================================================================
 * rx_grammar.c - compile every regex in a real TextMate grammar and report
 * what the engine could not take (UCD-28).
 *
 * NOT part of the gate, and deliberately: it needs a published grammar file,
 * and shipping somebody else's 200 KB of JSON to test our regex engine would
 * be a licensing question answered for the wrong reason.  The gate's own
 * grammar test uses a small one written here.
 *
 * This exists because "the engine supports lookaround now" is a claim about a
 * feature, and the claim that MATTERS is about a file: how much of Microsoft's
 * TypeScript grammar does this editor actually load?  A rule whose regex will
 * not compile is silently dropped by uc_lang.c - the slot is left inert - so
 * the difference between 60% and 99% is invisible from inside the editor and
 * enormous from outside it.
 *
 *   gcc -O1 -Icore tools/rx_grammar.c core/uc_rx.c core/uc_json.c \
 *       core/uc_util.c -o build/rx_grammar
 *   ./build/rx_grammar TypeScript.tmLanguage.json
 * ======================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unocode.h"

static int g_total, g_ok;

#define REASONS 24
static struct { char msg[64]; int n; char sample[120]; } g_bad[REASONS];
static int g_nbad;

static void note_fail(const char *msg, const char *pat)
{
    int i;
    for (i = 0; i < g_nbad; i++)
        if (!strcmp(g_bad[i].msg, msg)) { g_bad[i].n++; return; }
    if (g_nbad >= REASONS) return;
    uc_scpy(g_bad[g_nbad].msg, msg, sizeof g_bad[0].msg);
    uc_scpy(g_bad[g_nbad].sample, pat, sizeof g_bad[0].sample);
    g_bad[g_nbad].n = 1;
    g_nbad++;
}

static void try_pattern(const char *pat)
{
    char err[80];
    UcRx *rx;
    if (!pat || !pat[0]) return;
    g_total++;
    rx = uc_rx_compile(pat, 0, err, sizeof err);
    if (rx) { g_ok++; uc_rx_free(rx); return; }
    note_fail(err[0] ? err : "(no reason given)", pat);
}

static void walk(UcJson *n)
{
    UcJson *c;
    if (!n) return;
    if (n->type == UJ_OBJ) {
        static const char *kKeys[] = { "match", "begin", "end", "while", 0 };
        int i;
        for (i = 0; kKeys[i]; i++) {
            UcJson *m = uc_json_member(n, kKeys[i]);
            if (m && m->type == UJ_STR) try_pattern(m->str);
        }
    }
    for (c = n->child; c; c = c->next) walk(c);
}

int main(int argc, char **argv)
{
    FILE *f;
    long len;
    char *src, err[160];
    UcJson *root;
    int i;

    if (argc < 2) { fprintf(stderr, "usage: rx_grammar <grammar.json>\n"); return 2; }
    f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
    src = (char *)malloc((unsigned long)len + 1);
    if (!src || fread(src, 1, (unsigned long)len, f) != (unsigned long)len) {
        fprintf(stderr, "cannot read %s\n", argv[1]); return 2;
    }
    src[len] = 0;
    fclose(f);

    root = uc_json_parse(src, (int)len, err, sizeof err);
    if (!root) { fprintf(stderr, "%s: %s\n", argv[1], err); return 1; }
    walk(root);
    uc_json_free(root);
    free(src);

    printf("%s\n", argv[1]);
    printf("  %d of %d patterns compile (%.1f%%)\n", g_ok, g_total,
           g_total ? 100.0 * g_ok / g_total : 0.0);
    for (i = 0; i < g_nbad; i++)
        printf("  %4d x %-34s e.g. %.70s\n", g_bad[i].n, g_bad[i].msg,
               g_bad[i].sample);
    return 0;
}
