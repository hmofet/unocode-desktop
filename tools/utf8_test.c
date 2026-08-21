/* ===========================================================================
 * utf8_test.c - pc64/uno_utf8.h, the decoder every UTF-8 road depends on.
 *
 * Worth its own test because the whole of UCD-03 rests on one property: that
 * uno_u8_get() never reports consuming more bytes than it was handed, and
 * never reports 0 for a non-empty string.  Break either and the editor's
 * offsets stop matching the file's bytes - which is the failure mode where an
 * editor silently rewrites a document.
 *
 *   cc tools/utf8_test.c -I upstream/unodos/pc64 -o utf8_test
 * ======================================================================== */
#include <stdio.h>
#include <string.h>

#include "uno_utf8.h"

static int g_fail;

static void ok(int cond, const char *what)
{
    printf("%s %s\n", cond ? "  ok  " : "  FAIL", what);
    if (!cond) g_fail++;
}

/* decode `s` whole and assert the sequence of (codepoint, length) pairs */
static void seq(const char *s, const int *want, int nwant, const char *what)
{
    int i = 0, k = 0, n = (int)strlen(s), good = 1;
    while (i < n && k < nwant) {
        int cp, len = uno_u8_get(s + i, n - i, &cp);
        if (len <= 0 || len > n - i) { good = 0; break; }
        if (cp != want[k * 2] || len != want[k * 2 + 1]) { good = 0; break; }
        i += len; k++;
    }
    if (i != n || k != nwant) good = 0;
    ok(good, what);
}

int main(void)
{
    printf("1. well-formed sequences decode to the right codepoint and length\n");
    { int w[] = { 'A', 1 };            seq("A", w, 1, "ASCII"); }
    { int w[] = { 0xE9, 2 };           seq("\xc3\xa9", w, 1, "U+00E9 e-acute (2 bytes)"); }
    { int w[] = { 0x4E2D, 3 };         seq("\xe4\xb8\xad", w, 1, "U+4E2D CJK (3 bytes)"); }
    { int w[] = { 0x1F642, 4 };        seq("\xf0\x9f\x99\x82", w, 1, "U+1F642 emoji (4 bytes)"); }
    { int w[] = { 'a', 1, 0xE9, 2, 0x4E2D, 3, 0x1F642, 4, 'z', 1 };
      seq("a\xc3\xa9\xe4\xb8\xad\xf0\x9f\x99\x82z", w, 5, "a mixed line, in order"); }

    printf("2. malformed input costs exactly ONE byte, never more\n");
    {
        /* the property the whole design rests on: a bad byte must not swallow
         * the good ones after it, or every offset past it shifts */
        int cp, n;
        n = uno_u8_get("\xff", 1, &cp);
        ok(n == 1 && cp == UNO_CP_BAD, "a byte that starts nothing: 1 byte, U+FFFD");
        n = uno_u8_get("\x80", 1, &cp);
        ok(n == 1 && cp == UNO_CP_BAD, "a stray continuation byte: 1 byte");
        n = uno_u8_get("\xc3", 1, &cp);
        ok(n == 1 && cp == UNO_CP_BAD, "a lead byte with nothing after it");
        n = uno_u8_get("\xc3z", 2, &cp);
        ok(n == 1 && cp == UNO_CP_BAD, "a lead byte followed by the wrong thing");
        n = uno_u8_get("\xc0\xaf", 2, &cp);
        ok(n == 1 && cp == UNO_CP_BAD, "an over-long '/' is refused, not decoded");
        n = uno_u8_get("\xe0\x80\xaf", 3, &cp);
        ok(n == 1 && cp == UNO_CP_BAD, "an over-long 3-byte form too");
        n = uno_u8_get("\xed\xa0\x80", 3, &cp);
        ok(n == 1 && cp == UNO_CP_BAD, "a UTF-16 surrogate is not a codepoint");
        n = uno_u8_get("\xf5\x80\x80\x80", 4, &cp);
        ok(n == 1 && cp == UNO_CP_BAD, "past U+10FFFF is refused");
        /* a truncated tail at the END of a buffer must not read past it */
        n = uno_u8_get("\xe4\xb8", 2, &cp);
        ok(n == 1 && cp == UNO_CP_BAD, "a 3-byte lead with only 2 bytes available");
        n = uno_u8_get("", 0, &cp);
        ok(n == 0, "an empty buffer consumes nothing");
    }

    printf("3. a bad byte does not lose the text after it\n");
    {
        int w[] = { 'a', 1, UNO_CP_BAD, 1, 'b', 1 };
        /* split the literal: "\xffb" would be ONE greedy hex escape */
        seq("a\xff" "b", w, 3, "a<bad>b decodes as three characters");
    }

    printf("4. encode is decode's inverse\n");
    {
        static const int cps[] = { 0x24, 0x7F, 0x80, 0xE9, 0x7FF, 0x800, 0x4E2D,
                                   0xFFFD, 0xFFFF, 0x10000, 0x1F642, 0x10FFFF };
        int i, good = 1, lens = 1;
        for (i = 0; i < (int)(sizeof cps / sizeof cps[0]); i++) {
            char buf[4];
            int cp, n = uno_u8_put(cps[i], buf);
            if (uno_u8_len(cps[i]) != n) lens = 0;
            if (uno_u8_get(buf, n, &cp) != n || cp != cps[i]) good = 0;
        }
        ok(good, "every codepoint survives put -> get");
        ok(lens, "uno_u8_len agrees with what uno_u8_put wrote");
    }
    {
        char buf[4];
        int cp;
        ok(uno_u8_put(0xD800, buf) == 3 && uno_u8_get(buf, 3, &cp) == 3 &&
           cp == UNO_CP_BAD, "encoding a surrogate yields U+FFFD, not a surrogate");
    }

    printf("5. aligning back lands on a character start\n");
    {
        const char *s = "a\xe4\xb8\xad" "b";     /* a [3-byte CJK] b */
        ok(uno_u8_align(s, 0) == 0, "offset 0 is already a start");
        ok(uno_u8_align(s, 1) == 1, "the CJK lead byte is a start");
        ok(uno_u8_align(s, 2) == 1, "its first continuation aligns back to it");
        ok(uno_u8_align(s, 3) == 1, "its second continuation does too");
        ok(uno_u8_align(s, 4) == 4, "the byte after it is a start again");
        /* a wall of continuation bytes must not walk off the front */
        ok(uno_u8_align("\x80\x80\x80\x80\x80", 4) == 1,
           "a run of stray continuations is bounded, not unbounded");
    }

    printf("6. widths: the ones the grid arithmetic depends on\n");
    ok(uno_cp_width('A') == 1, "ASCII is one cell");
    ok(uno_cp_width(0xE9) == 1, "e-acute is one cell");
    ok(uno_cp_width(0x4E2D) == 2, "a CJK ideograph is two");
    ok(uno_cp_width(0x3042) == 2, "hiragana is two");
    ok(uno_cp_width(0xAC00) == 2, "a Hangul syllable is two");
    ok(uno_cp_width(0x1F642) == 2, "an emoji is two");
    ok(uno_cp_width(0x0301) == 0, "a combining acute is none");
    ok(uno_cp_width(0xFE0F) == 0, "a variation selector is none");
    ok(uno_cp_width(0x2500) == 1, "a box-drawing character is one");

    printf("\n%s: %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
