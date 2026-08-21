/* ===========================================================================
 * host_secret.c - the uc_secret seam on a desktop OS (UCD-48).
 *
 * Three platforms, three honest answers:
 *
 *   Windows  DPAPI.  Each value is sealed with CryptProtectData under the
 *            logged-in user's credentials and the SEALED blob goes in the
 *            file; the file is useless on another machine or account.
 *   macOS    the Keychain, as a generic-password item (service "UnoCode",
 *            account = the secret's name).  No file of ours at all.
 *   Linux    the file, hex-encoded but NOT encrypted, created 0600.  The OS
 *            offers no user-credential vault without a desktop daemon we will
 *            not depend on, so the honest answer is tight permissions and a
 *            UI that says "file", not "vault".  uc_secret_plaintext() = 1.
 *
 * The file (Windows and Linux) is <state dir>/SECRETS.DAT, one record per
 * line: `name SP hex NL`.  Rewrites go through SECRETS.TMP + rename, so an
 * interrupted save leaves the old file whole rather than half of one.
 *
 * Like host_pick_*.c, every branch of this file is compiled on every platform
 * - the inapplicable ones under #ifdef produce nothing - because a
 * platform-conditional SOURCE LIST is a second place for the builds to
 * disagree.
 * ======================================================================== */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "uc_secret.h"
#include "host.h"

static char g_dir[1024];            /* the per-user state directory          */

void host_secret_dir(const char *home)
{
    snprintf(g_dir, sizeof g_dir, "%s", home ? home : ".");
}

#if defined(__APPLE__)
/* ---- macOS: the Keychain --------------------------------------------------
 * SecItem's C API, so no .m file and no ARC: a generic-password item per
 * secret.  The first store may prompt the user to allow UnoCode access; that
 * prompt is the Keychain doing its job, not a bug. */
#include <Security/Security.h>
#include <CoreFoundation/CoreFoundation.h>

static CFMutableDictionaryRef mac_query(const char *name)
{
    CFMutableDictionaryRef q = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef acct = CFStringCreateWithCString(NULL, name,
                                                 kCFStringEncodingUTF8);
    CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(q, kSecAttrService, CFSTR("UnoCode"));
    CFDictionarySetValue(q, kSecAttrAccount, acct);
    CFRelease(acct);
    return q;
}

int uc_secret_set(const char *name, const char *value)
{
    CFMutableDictionaryRef q = mac_query(name);
    CFDataRef data = CFDataCreate(NULL, (const UInt8 *)value,
                                  (CFIndex)strlen(value));
    OSStatus st;
    SecItemDelete(q);                     /* replace = delete + add          */
    CFDictionarySetValue(q, kSecValueData, data);
    st = SecItemAdd(q, NULL);
    CFRelease(data);
    CFRelease(q);
    return st == errSecSuccess;
}

int uc_secret_get(const char *name, char *out, int cap)
{
    CFMutableDictionaryRef q = mac_query(name);
    CFTypeRef result = NULL;
    OSStatus st;
    int n;
    if (cap > 0) out[0] = 0;
    CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);
    st = SecItemCopyMatching(q, &result);
    CFRelease(q);
    if (st != errSecSuccess || !result) return 0;
    n = (int)CFDataGetLength((CFDataRef)result);
    if (n > cap - 1) n = cap - 1;
    if (n > 0) memcpy(out, CFDataGetBytePtr((CFDataRef)result), (size_t)n);
    if (cap > 0) out[n] = 0;
    CFRelease(result);
    return 1;
}

int uc_secret_del(const char *name)
{
    CFMutableDictionaryRef q = mac_query(name);
    OSStatus st = SecItemDelete(q);
    CFRelease(q);
    return st == errSecSuccess || st == errSecItemNotFound;
}

const char *uc_secret_store_name(void) { return "the macOS Keychain"; }
int uc_secret_plaintext(void) { return 0; }

#else /* Windows and Linux: the file, with a per-platform seal ------------- */

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#define SEC_NAME_MAX 64
#define SEC_HEX_MAX  2600     /* a DPAPI blob for UC_SECRET_MAX, hex-encoded */
#define SEC_ENTRIES  32

static struct { char name[SEC_NAME_MAX]; char hex[SEC_HEX_MAX]; } g_e[SEC_ENTRIES];
static int g_n;

static void sec_path(char *out, int cap, const char *base)
{
    snprintf(out, (size_t)cap, "%s/%s", g_dir[0] ? g_dir : ".", base);
}

static int hex_enc(const unsigned char *in, int n, char *out, int cap)
{
    static const char d[] = "0123456789abcdef";
    int i;
    if (n * 2 + 1 > cap) return 0;
    for (i = 0; i < n; i++) {
        out[i * 2]     = d[in[i] >> 4];
        out[i * 2 + 1] = d[in[i] & 15];
    }
    out[n * 2] = 0;
    return 1;
}

static int hex_dec(const char *in, unsigned char *out, int cap)
{
    int n = 0;
    while (in[0] && in[1]) {
        int hi, lo;
        if (n >= cap) return -1;
        hi = (in[0] >= 'a') ? in[0] - 'a' + 10 : in[0] - '0';
        lo = (in[1] >= 'a') ? in[1] - 'a' + 10 : in[1] - '0';
        if (hi < 0 || hi > 15 || lo < 0 || lo > 15) return -1;
        out[n++] = (unsigned char)(hi << 4 | lo);
        in += 2;
    }
    return n;
}

static void sec_load(void)
{
    char path[1200], line[SEC_NAME_MAX + SEC_HEX_MAX + 4];
    FILE *f;
    g_n = 0;
    sec_path(path, sizeof path, "SECRETS.DAT");
    f = fopen(path, "r");
    if (!f) return;
    while (g_n < SEC_ENTRIES && fgets(line, sizeof line, f)) {
        char *sp = strchr(line, ' ');
        char *nl = strchr(line, '\n');
        if (!sp || sp == line) continue;
        if (nl) *nl = 0;
        *sp = 0;
        if (strlen(line) >= SEC_NAME_MAX || strlen(sp + 1) >= SEC_HEX_MAX)
            continue;
        strcpy(g_e[g_n].name, line);
        strcpy(g_e[g_n].hex, sp + 1);
        g_n++;
    }
    fclose(f);
}

static int sec_save(void)
{
    char tmp[1200], path[1200];
    FILE *f;
    int i;
    sec_path(tmp, sizeof tmp, "SECRETS.TMP");
    sec_path(path, sizeof path, "SECRETS.DAT");
#ifdef _WIN32
    f = fopen(tmp, "w");
#else
    {   /* 0600 from the first byte - never world-readable, even briefly */
        int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        f = (fd >= 0) ? fdopen(fd, "w") : 0;
    }
#endif
    if (!f) return 0;
    for (i = 0; i < g_n; i++)
        fprintf(f, "%s %s\n", g_e[i].name, g_e[i].hex);
    if (fclose(f) != 0) { remove(tmp); return 0; }
    remove(path);                   /* Windows rename() refuses to replace  */
    return rename(tmp, path) == 0;
}

static int sec_find(const char *name)
{
    int i;
    for (i = 0; i < g_n; i++)
        if (!strcmp(g_e[i].name, name)) return i;
    return -1;
}

/* seal/unseal: value bytes <-> the bytes that go in the file (before hex) */
#ifdef _WIN32
static int seal(const char *value, unsigned char *out, int cap)
{
    DATA_BLOB in, enc;
    int n;
    in.pbData = (BYTE *)value;
    in.cbData = (DWORD)strlen(value);
    if (!CryptProtectData(&in, L"UnoCode secret", NULL, NULL, NULL,
                          CRYPTPROTECT_UI_FORBIDDEN, &enc))
        return -1;
    n = (int)enc.cbData;
    if (n > cap) n = -1; else memcpy(out, enc.pbData, enc.cbData);
    LocalFree(enc.pbData);
    return n;
}
static int unseal(const unsigned char *blob, int n, char *out, int cap)
{
    DATA_BLOB in, dec;
    int m;
    in.pbData = (BYTE *)blob;
    in.cbData = (DWORD)n;
    if (!CryptUnprotectData(&in, NULL, NULL, NULL, NULL,
                            CRYPTPROTECT_UI_FORBIDDEN, &dec))
        return -1;
    m = (int)dec.cbData;
    if (m > cap - 1) m = cap - 1;
    memcpy(out, dec.pbData, (size_t)m);
    out[m] = 0;
    LocalFree(dec.pbData);
    return m;
}
const char *uc_secret_store_name(void) { return "Windows DPAPI"; }
int uc_secret_plaintext(void) { return 0; }
#else
static int seal(const char *value, unsigned char *out, int cap)
{
    int n = (int)strlen(value);
    if (n > cap) return -1;
    memcpy(out, value, (size_t)n);
    return n;
}
static int unseal(const unsigned char *blob, int n, char *out, int cap)
{
    if (n > cap - 1) n = cap - 1;
    memcpy(out, blob, (size_t)n);
    out[n] = 0;
    return n;
}
const char *uc_secret_store_name(void)
{ return "a file only you can read (SECRETS.DAT)"; }
int uc_secret_plaintext(void) { return 1; }
#endif

int uc_secret_set(const char *name, const char *value)
{
    unsigned char blob[SEC_HEX_MAX / 2];
    char hex[SEC_HEX_MAX];
    int n, i;
    if (!name || !name[0] || !value || strlen(name) >= SEC_NAME_MAX ||
        strlen(value) >= UC_SECRET_MAX)
        return 0;
    n = seal(value, blob, sizeof blob);
    if (n < 0 || !hex_enc(blob, n, hex, sizeof hex)) return 0;
    sec_load();
    i = sec_find(name);
    if (i < 0) {
        if (g_n >= SEC_ENTRIES) return 0;
        i = g_n++;
        strcpy(g_e[i].name, name);
    }
    strcpy(g_e[i].hex, hex);
    return sec_save();
}

int uc_secret_get(const char *name, char *out, int cap)
{
    unsigned char blob[SEC_HEX_MAX / 2];
    int i, n;
    if (cap > 0) out[0] = 0;
    if (!name || cap <= 0) return 0;
    sec_load();
    i = sec_find(name);
    if (i < 0) return 0;
    n = hex_dec(g_e[i].hex, blob, sizeof blob);
    if (n < 0) return 0;
    return unseal(blob, n, out, cap) >= 0;
}

int uc_secret_del(const char *name)
{
    int i;
    sec_load();
    i = sec_find(name);
    if (i < 0) return 1;
    for (; i < g_n - 1; i++) g_e[i] = g_e[i + 1];
    g_n--;
    return sec_save();
}

#endif /* file-backed platforms */
