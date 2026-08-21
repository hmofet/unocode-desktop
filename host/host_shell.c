/* ===========================================================================
 * host_shell.c - the five pc64_shell_* hooks and the clock, desktop edition.
 *
 * On pc64 these come from the shell that owns the desktop.  Here the SDL main
 * loop is the shell, and each hook is the smallest honest translation:
 *
 *   pc64_shell_dirty      -> a repaint flag the frame loop drains
 *   pc64_shell_theme      -> the stock UnoDOS unoui theme (chrome metrics
 *                            only - fullscreen never draws window chrome)
 *   pc64_shell_font_mono  -> slot 2, "Mono" in pc64_font's slot table
 *   pc64_shell_run_user   -> honest refusal, with the reason in the terminal;
 *                            a real process spawn is the phase-2 terminal work
 *   pc64_browser_open_path-> not yet wired (needs a sanctioned way to hand a
 *                            path to the host's default handler)
 * ======================================================================== */
#include "host.h"
#include "unoui_theme.h"

static int g_dirty = 1;               /* first frame always paints */

void host_mark_dirty(void) { g_dirty = 1; }

int host_take_dirty(void)
{
    int d = g_dirty;
    g_dirty = 0;
    return d;
}

void pc64_shell_dirty(void) { g_dirty = 1; }

const struct unoui_theme *pc64_shell_theme(void) { return &theme_unodos; }

int pc64_shell_font_mono(void) { return 2; }

/* uc_term prints pc64_shell_py_error() when a run fails, so the reason a
 * desktop build cannot launch a .UNO lands where the user is looking. */
static const char *g_run_err = "";

int pc64_shell_run_user(int vol, const char *path)
{
    (void)vol; (void)path;
    g_run_err = "running programs is not wired up on the desktop yet";
    return -1;
}

/* ... and the capability flag saying so UP FRONT, so an assistant can leave
 * the run tool out entirely instead of offering one that always refuses. */
int pc64_shell_can_run(void) { return 0; }

const char *pc64_shell_py_error(void) { return g_run_err; }

void pc64_browser_open_path(const char *path) { (void)path; }

/* main.c updates these from the live SDL window size */
int host_workarea_w = 1280, host_workarea_h = 800;
int pc64_shell_workarea_w(void) { return host_workarea_w; }
int pc64_shell_workarea_h(void) { return host_workarea_h; }

unsigned long uno_dbg_uptime_ms(void) { return host_ms(); }
