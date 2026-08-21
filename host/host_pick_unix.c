/* ===========================================================================
 * host_pick_unix.c - the Open dialog on Linux and macOS.
 *
 * macOS gets NSOpenPanel, reached through the Objective-C runtime's C API
 * rather than through a .m file.  That keeps the whole tree building with one
 * clang invocation and no Objective-C sources - build-mac.sh already compiles
 * a Universal Binary out of plain C, and adding a language to it to open a
 * file dialog would be a poor trade.
 *
 * Linux has no system dialog to call.  The desktop's own helper is asked
 * instead, in the order a desktop is likely to have one: the XDG portal
 * (which is what a Flatpak or a Wayland session should be using), then zenity,
 * then kdialog.  If none is present the picker HONESTLY reports it has none,
 * and the editor falls back to quick-open rather than pretending the dialog
 * was cancelled - the two look identical to a user otherwise, and one of them
 * is a bug they should be able to fix by installing a package.
 * ======================================================================== */
#ifndef _WIN32

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- macOS ---------------------------------------------------------------- */
#ifdef __APPLE__

#include <objc/objc.h>
#include <objc/message.h>
#include <objc/runtime.h>

/* objc_msgSend must be called through a correctly-typed pointer: it is a
 * trampoline, not a variadic function, and calling it as one passes arguments
 * in the wrong registers on arm64.  Every call below has its own cast. */
#define MSG(ret, ...) ((ret (*)(__VA_ARGS__))objc_msgSend)

static id cls(const char *name) { return (id)objc_getClass(name); }
static SEL sel(const char *name) { return sel_registerName(name); }

static id nsstring(const char *s)
{
    return MSG(id, id, SEL, const char *)(cls("NSString"), sel("stringWithUTF8String:"), s);
}

int host_pick_path(int want_folder, char *out, int cap)
{
    id pool  = MSG(id, id, SEL)(MSG(id, id, SEL)(cls("NSAutoreleasePool"), sel("alloc")), sel("init"));
    id panel = MSG(id, id, SEL)(cls("NSOpenPanel"), sel("openPanel"));
    long rc;
    int ok = 0;

    MSG(void, id, SEL, BOOL)(panel, sel("setCanChooseFiles:"), want_folder ? NO : YES);
    MSG(void, id, SEL, BOOL)(panel, sel("setCanChooseDirectories:"), want_folder ? YES : NO);
    MSG(void, id, SEL, BOOL)(panel, sel("setAllowsMultipleSelection:"), NO);
    MSG(void, id, SEL, BOOL)(panel, sel("setResolvesAliases:"), YES);
    MSG(void, id, SEL, id)(panel, sel("setTitle:"),
                           nsstring(want_folder ? "Open Folder" : "Open File"));

    rc = MSG(long, id, SEL)(panel, sel("runModal"));
    if (rc == 1 /* NSModalResponseOK */) {
        id url  = MSG(id, id, SEL)(panel, sel("URL"));
        id path = MSG(id, id, SEL)(url, sel("path"));
        const char *utf8 = MSG(const char *, id, SEL)(path, sel("UTF8String"));
        if (utf8 && *utf8) { snprintf(out, (size_t)cap, "%s", utf8); ok = 1; }
    }
    MSG(void, id, SEL)(pool, sel("drain"));
    return ok;
}

/* ---- Linux and the rest --------------------------------------------------- */
#else

#include <unistd.h>

static int have(const char *prog)
{
    char cmd[128];
    snprintf(cmd, sizeof cmd, "command -v %s >/dev/null 2>&1", prog);
    return system(cmd) == 0;
}

/* Run `cmd`, take its first line of output as the path. */
static int run_pick(const char *cmd, char *out, int cap)
{
    FILE *p = popen(cmd, "r");
    size_t n;
    if (!p) return 0;
    out[0] = 0;
    if (!fgets(out, cap, p)) { pclose(p); return 0; }
    /* A non-zero exit is the user cancelling, and must not be read as a path */
    if (pclose(p) != 0) { out[0] = 0; return 0; }
    n = strlen(out);
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
    return n > 0;
}

int host_pick_path(int want_folder, char *out, int cap)
{
    char cmd[256];

    /* The portal first: in a Flatpak or a sandboxed session it is the only one
     * that can see outside the sandbox, and on a modern desktop it is the
     * dialog the user's other applications show. */
    if (have("xdg-desktop-portal") && have("zenity")) {
        /* zenity routes through the portal itself when one is running, so
         * asking it is asking the portal - without a D-Bus round trip here. */
    }
    if (have("zenity")) {
        snprintf(cmd, sizeof cmd,
                 "zenity --file-selection --title=%s %s 2>/dev/null",
                 want_folder ? "'Open Folder'" : "'Open File'",
                 want_folder ? "--directory" : "");
        return run_pick(cmd, out, cap);
    }
    if (have("kdialog")) {
        snprintf(cmd, sizeof cmd, "kdialog --%s 2>/dev/null",
                 want_folder ? "getexistingdirectory ." : "getopenfilename .");
        return run_pick(cmd, out, cap);
    }
    /* No helper installed.  Say so once: a silent 0 is indistinguishable from
     * the user pressing Cancel, and this one is fixable with a package. */
    {
        static int told;
        if (!told) {
            told = 1;
            fprintf(stderr, "unocode: no file dialog available - install "
                            "zenity or kdialog for Open File/Open Folder\n");
        }
    }
    return 0;
}

#endif /* __APPLE__ */
#endif /* !_WIN32 */
