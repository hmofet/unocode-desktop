/* ===========================================================================
 * main.c - UnoCode Desktop: the SDL2 shell around the unmodified editor core.
 *
 * The pc64 shell owns a desktop of windows; this host owns exactly one, and
 * runs UnoCode the way unoui already supports for games: FULLSCREEN, where the
 * module's canvas is handed the whole framebuffer and every input event.  The
 * result looks like a native application because, structurally, it is one -
 * there is no in-window desktop, no taskbar, no unoui chrome.
 *
 * INPUT takes the same two roads as on pc64 (see uc_main.c's header):
 *   1. canvas events  - mouse, wheel, navigation keys, printable characters,
 *                       each with the full SDL modifier mask;
 *   2. the key hook   - Ctrl chords and function keys, where Shift travels as
 *                       the shifted character itself (Ctrl+Shift+P = 'P').
 * SDL cleanly separates the two: TEXTINPUT feeds road 1's characters, KEYDOWN
 * feeds road 1's navigation keys and everything on road 2.
 *
 * The frame loop is dirty-driven: uc_frame() asks for a repaint on the caret's
 * cadence and input handlers ask on change, so an idle editor renders about
 * twice a second and an active one at the display's pace.
 *
 *   unocode [folder]           edit a folder (default: the current directory)
 *   unocode --open <file>      also open that file in an editor tab
 *   unocode --shot <out.ppm>   headless: render the workbench, write a PPM,
 *                              exit - the CI eye that needs no display
 *   unocode --type <text>      headless: feed <text> in as typing first
 *   unocode --keys <LRUDHEBX>  headless: navigation keys, after --type
 *   unocode --save             headless: save the active editor after --type
 *   unocode --lsp <ms>         headless: run real frames for <ms> before and
 *                              after the typing, then print what the language
 *                              -server client did, traffic and all
 *   unocode --set k=v           override one setting for this run only
 *   unocode --suggest          ask for completions at the caret and print them
 *   unocode --hover            ask for a hover at the caret and print it
 *   unocode --def              go to definition, then Alt+Left and Alt+Right
 *   unocode --refs             find all references and print them
 *   unocode --rename <name>    rename the symbol at the caret
 *   unocode --format           format the document
 *
 * The language-feature hands all print rather than paint, because what they
 * have to prove is CONTENT - which completion, at which column, in whose order
 * - and a screenshot cannot answer any of that.  --hover is the exception, and
 * the gate diffs two runs to check it is drawn at all.
 *
 * --type/--keys/--save are the gate's hands, as --shot is its eyes: a
 * screenshot can say the workbench painted, but only typing and saving can say
 * that what went in came back out of the file byte for byte.
 *
 * --lsp is there because a language server is a CHILD PROCESS, and the five
 * frames the shot path runs prove nothing about one - it has not finished
 * starting yet.  The only honest test lets the clock run and then looks.
 * ======================================================================== */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "fb.h"
#include "unoui.h"
#include "unoui_theme.h"
#include "uno_uuiapp.h"
#include "uno_utf8.h"
#include "host.h"

/* the runtime desktop size fb.h expects the platform to own (UNO_PC64) */
int uno_fb_w = 1280, uno_fb_h = 800;

extern int host_workarea_w, host_workarea_h;     /* host_shell.c */

static unoui_ui     UI;
static unoui_window WIN;
static const UnoUuiApp *APP;

/* the module entry (uno_uuiapp.h only declares the loader-side typedef; on
 * pc64 this symbol is found through the module header, not a prototype) */
extern const UnoUuiApp *uno_app_main(void *reserved);
extern const struct unoui_theme *pc64_shell_theme(void);   /* host_shell.c */
extern int uc_doc_open(int vol, const char *dir, const char *name);
extern void uc_open_folder(int vol, const char *dir);

/* UcDoc is an anonymous struct in unocode.h, which is subsystem-internal and
 * this host may not include, so the handle travels as void * - it is only ever
 * handed straight back to the core that made it. */
extern void *uc_doc_active(void);
extern int   uc_doc_save(void *d);

/* The language-server client's introspection, in the same opaque dialect and
 * for the same reason: a UcLsp * is only ever handed back to the core. */
extern int         uc_lsp_count(void);
extern void       *uc_lsp_at(int i);
extern int         uc_lsp_state(void *s);
extern const char *uc_lsp_name(void *s);
extern int         uc_lsp_restarts(void *s);
extern int         uc_output_channel(const char *name);
extern int         uc_output_lines(int ch);
extern const char *uc_output_line(int ch, int i);
extern void        uc_lsp_trace_on(void);
extern int         uc_problems_total(void);
extern void       *uc_problem_at(int i);
extern int         uc_problem_line(void *p);
extern int         uc_problem_col(void *p);
extern int         uc_problem_endcol(void *p);
extern int         uc_problem_sev(void *p);
extern const char *uc_problem_msg(void *p);
extern const char *uc_problem_file(void *p);
extern void        uc_suggest_open(void *d, int explicit_req);
extern int         uc_suggest_count(void);
extern int         uc_suggest_from_server(void);
extern const char *uc_suggest_label(int i);
extern const char *uc_suggest_detail(int i);
extern const char *uc_suggest_insert(int i);
extern int         uc_suggest_kind_at(int i);
extern void        uc_hover_at(void *d, int off, int px, int py);
extern int         uc_hover_active(void);
extern const char *uc_hover_text(void);
extern int         uc_doc_caret(void *d);
extern void        uc_goto_definition(void *d);
extern void        uc_find_references(void *d);
extern int         uc_nav_back(void);
extern int         uc_nav_forward(void);
extern int         uc_results_count(void);
extern const char *uc_results_path(int i);
extern int         uc_results_line(int i);
extern const char *uc_results_text(int i);
extern void        uc_rename_symbol(void *d);
extern void        uc_format_document(void *d);
extern int         uc_save_with_format(void *d);
extern void        uc_cfg_override(const char *key, const char *val);
extern int         uc_lang_by_id(const char *id);
extern int         uc_lang_load_grammar(int lang, int vol, const char *path);
extern int         uc_line_count(void *d);
extern int         uc_line_scopes(void *d, int line, short *out, int cap);
extern const char *uc_scope_name(int id);
extern int         uc_ws_vol(void);
extern unsigned long uc_hl_rx_calls(void);
extern int         uc_quick_key(int key, int mods, int ch);
extern int         uc_doc_title(void *d, char *out, int cap);
extern int         uc_line_of(void *d, int off);
extern int         uc_col_of(void *d, int off);

/* re-derive the editor's font metrics after a UI-scale change (uc_edit.c) */
extern void uc_metrics_init(void);
extern void uno_font_set_ui_scale(int pct);
extern int  uc_host_dirty_count(void);
extern int  uc_host_save_all(void);

static const char *g_open_file;    /* --open, resolved after the app is up */
static const char *g_type_text;    /* --type, fed in as character events   */
static const char *g_keys;         /* --keys, navigation keys after --type */
static int         g_do_save;      /* --save, after --type                 */
/* --lsp <ms>: run the frame loop for this long before and after the typing,
 * then print what the language-server client did.  A server is a CHILD PROCESS
 * that takes a second or two to start and answer, so the five frames the shot
 * path runs prove nothing about it - the only honest test is to let the clock
 * run and then look. */
static int         g_lsp_ms;
/* --suggest: after the language server has settled, ask for completions at the
 * caret and print what came back.  The suggestion widget is the one piece of
 * UI whose correctness a screenshot cannot show - the question is what is IN
 * the list and in what order, not whether a box appeared. */
static int         g_suggest;
/* --hover: ask the server about the symbol at the caret and print the answer.
 * At the CARET, not at the pointer: a headless run has no pointer to rest. */
static int         g_hover;
/* --def / --refs: drive UCD-26 headlessly.  --def also presses Alt+Left and
 * Alt+Right afterwards, because the navigation stack is the half of "go to
 * definition" that a user actually feels and it has no other way to be seen. */
static int         g_def, g_refs;
/* --rename <name> / --format: drive UCD-27.  Both write their result to disk
 * afterwards, because the claim worth checking is what the FILES say - an edit
 * count is satisfied by edits applied in the wrong order. */
static const char *g_rename;
static int         g_format;
/* --grammar <langid>=<file> loads a TextMate grammar over a built-in language,
 * and --scopes prints the scope of every character of every line.  Together
 * they are the only way to ask the tokenizer the question that matters - "what
 * is this character, given the twelve lines above it" - because the answer
 * depends on the document's cached cross-line state, not on one line. */
static const char *g_grammar;
static int         g_scopes;
/* --set key=jsonvalue, repeatable: override a setting for this run only, so a
 * test can exercise one without editing the user's real SETTINGS.JSN. */
static const char *g_set[8];
static int         g_nset;
static float       g_wheel_acc;    /* sub-notch trackpad scroll, UCD-10     */
static HostGeom    G;              /* window geometry + last session        */
static const char *g_workdir = ".";

unsigned long host_ms(void) { return (unsigned long)SDL_GetTicks64(); }

/* ---- volume setup --------------------------------------------------------- */

static void setup_volumes(const char *workdir)
{
    char *base = SDL_GetBasePath();          /* dir of the executable */
    char res[900], home[900];
    const char *env;

    {   /* absolute, so the volume survives a chdir and the session-restore
         * comparison against last run's folder can actually match */
        char abs[900];
        host_fs_abspath(workdir, abs, sizeof abs);
        host_fs_add_volume("WORK", abs, 1);
        host_dialog_set_root(abs);
    }

    /* bundled resources: <exe>/res in an installed layout, ./res in a dev
     * tree.  Registered read-only: nothing the core does may edit the app. */
    {
        struct stat st;
        res[0] = 0;
        if (base) {
            snprintf(res, sizeof res, "%sres", base);
            SDL_free(base);
        }
        if (!res[0] || stat(res, &st) != 0)
            snprintf(res, sizeof res, "res");
        host_fs_add_volume("APP", res, 0);
    }

    /* Per-user data: where settings and keybindings live.  Created here,
     * because the core can only mkdir INSIDE a volume and this is the volume's
     * own root; and made the preferred volume, so UNOCODE\SETTINGS.JSN stops
     * being written into whatever folder is being edited. */
    {
        int vol;
#ifdef _WIN32
        env = getenv("APPDATA");
        snprintf(home, sizeof home, "%s\\UnoCode", env ? env : ".");
#else
        env = getenv("HOME");
        snprintf(home, sizeof home, "%s/.unocode", env ? env : ".");
#endif
        host_fs_mkpath(home);
        vol = host_fs_add_volume("HOME", home, 1);
        if (vol >= 0) host_fs_set_pref_vol(vol);
        host_state_dir(home);
        host_secret_dir(home);
        host_recent_file(home);
        host_recent_load();
    }
}

/* ---- HiDPI ----------------------------------------------------------------
 * SDL reports two sizes for one window: POINTS (what the window manager and
 * every mouse event use) and PIXELS (what the renderer actually has).  On a
 * Retina or 150%-scaled display they differ, and phase 0 sized the framebuffer
 * in points - so the editor was rendered at half resolution and stretched,
 * which on a Mac is the first thing anyone notices.
 *
 * The framebuffer is sized in PIXELS now.  Input stays in points and is scaled
 * on the way in, and the UI scale is driven from the same ratio so a 2x
 * display draws text at native resolution at the same physical size, rather
 * than drawing it small and magnifying it. */
static int g_dpi = 100;                 /* pixels per point, percent */

static int to_px(int pt) { return pt * g_dpi / 100; }

static void sync_dpi(SDL_Window *win, SDL_Renderer *ren, SDL_Texture **tex)
{
    int pw = 0, ph = 0, lw = 0, lh = 0, dpi;
    SDL_GetRendererOutputSize(ren, &pw, &ph);
    SDL_GetWindowSize(win, &lw, &lh);
    if (pw <= 0 || ph <= 0 || lw <= 0) return;

    dpi = pw * 100 / lw;
    if (dpi < 100) dpi = 100;           /* a renderer smaller than the window */
    if (dpi > 400) dpi = 400;
    g_dpi = dpi;

    if (pw > FB_MAX_W) pw = FB_MAX_W;
    if (ph > FB_MAX_H) ph = FB_MAX_H;
    if (pw == uno_fb_w && ph == uno_fb_h && *tex) return;

    uno_fb_w = pw;
    uno_fb_h = ph;
    UI.screen_w = pw;
    UI.screen_h = ph;
    UI.work = (unoui_rect){ 0, 0, pw, ph };
    host_workarea_w = pw;
    host_workarea_h = ph;

    /* uno_font_set_ui_scale clamps to 100..200; past 2x the glyphs stay at 2x
     * and the extra pixels buy sharpness rather than size, which is the right
     * way round to run out. */
    uno_font_set_ui_scale(dpi);
    uc_metrics_init();

    if (*tex) SDL_DestroyTexture(*tex);
    *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888,
                             SDL_TEXTUREACCESS_STREAMING, pw, ph);
    host_mark_dirty();
}

/* ---- input translation ---------------------------------------------------- */

static int ui_mods(SDL_Keymod m)
{
    int r = 0;
    if (m & KMOD_SHIFT) r |= UI_MOD_SHIFT;
    if (m & KMOD_CTRL)  r |= UI_MOD_CTRL;
    if (m & KMOD_ALT)   r |= UI_MOD_ALT;
    if (m & KMOD_GUI)   r |= UI_MOD_GUI;
    return r;
}

static int nav_key(SDL_Keycode k)
{
    switch (k) {
    case SDLK_LEFT:      return UI_KEY_LEFT;
    case SDLK_RIGHT:     return UI_KEY_RIGHT;
    case SDLK_UP:        return UI_KEY_UP;
    case SDLK_DOWN:      return UI_KEY_DOWN;
    case SDLK_HOME:      return UI_KEY_HOME;
    case SDLK_END:       return UI_KEY_END;
    case SDLK_PAGEUP:    return UI_KEY_PGUP;
    case SDLK_PAGEDOWN:  return UI_KEY_PGDN;
    case SDLK_BACKSPACE: return UI_KEY_BACKSPACE;
    case SDLK_DELETE:    return UI_KEY_DELETE;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:  return UI_KEY_ENTER;
    case SDLK_TAB:       return UI_KEY_TAB;
    case SDLK_ESCAPE:    return UI_KEY_ESC;
    default:             return 0;
    }
}

/* the shifted spelling of a US-layout key, for the chord road's Shift-as-
 * character convention (Ctrl+Shift+P travels as 'P') */
static int us_shift(int c)
{
    static const char *plain = "`1234567890-=[]\\;',./";
    static const char *shift = "~!@#$%^&*()_+{}|:\"<>?";
    const char *p;
    if (c >= 'a' && c <= 'z') return c - 32;
    p = strchr(plain, c);
    return p ? shift[p - plain] : c;
}

static void feed(const unoui_event *e)
{
    unoui_handle(&UI, e);
}

static void on_key_down(const SDL_KeyboardEvent *ke)
{
    SDL_Keycode sym = ke->keysym.sym;
    int mods = ui_mods((SDL_Keymod)ke->keysym.mod);
    int nk = nav_key(sym);
    unoui_event e;

    /* function keys ride the chord road, as EFI scan codes (K_F1 = 0x0B) */
    if (sym >= SDLK_F1 && sym <= SDLK_F12) {
        APP->key(0, 0x0B + (int)(sym - SDLK_F1), (mods & UI_MOD_CTRL) ? 1 : 0);
        return;
    }

    /* navigation keys ride the canvas road with the full modifier mask */
    if (nk) {
        memset(&e, 0, sizeof e);
        e.kind = UI_EV_KEY;
        e.key  = nk;
        e.mods = mods;
        feed(&e);
        return;
    }

    /* Ctrl chords ride the chord road; printable keys shift-spell themselves */
    if ((mods & UI_MOD_CTRL) && sym >= 32 && sym < 127) {
        int uni = (mods & UI_MOD_SHIFT) ? us_shift((int)sym) : (int)sym;
        /* belt and braces for the one chord where a stale clipboard is
         * visible: SDL_CLIPBOARDUPDATE and focus-gained have both already had
         * their chance, and this pull is a no-op when neither was missed */
        if (sym == SDLK_v) host_clip_pull();
        APP->key(uni, 0, 1);
        return;
    }

    /* Alt+letter as a canvas key event (the road that carries Alt) */
    if ((mods & UI_MOD_ALT) && sym >= 32 && sym < 127) {
        memset(&e, 0, sizeof e);
        e.kind = UI_EV_KEY;
        e.key  = (int)sym;
        e.mods = mods;
        feed(&e);
    }
}

/* SDL_TEXTINPUT delivers UTF-8; the canvas road's e.ch is a CODEPOINT, so
 * decode rather than walking bytes.  Phase 0 dropped every byte >= 0x80, which
 * made the editor unusable outside ASCII - an accented letter, a curly quote
 * and a box-drawing character were all simply unavailable. */
static void on_text(const char *text)
{
    unoui_event e;
    const char *p = text;
    int left = (int)strlen(text);
    SDL_Keymod m = SDL_GetModState();
    if (m & (KMOD_CTRL | KMOD_ALT)) return;      /* chords are not typing */
    while (left > 0) {
        int cp, n = uno_u8_get(p, left, &cp);
        if (n <= 0) break;
        p += n; left -= n;
        memset(&e, 0, sizeof e);
        e.kind = UI_EV_CHAR;
        e.ch   = cp;
        e.mods = ui_mods(m);
        feed(&e);
    }
}

/* ---- closing --------------------------------------------------------------
 * Phase 0 discarded unsaved editors SILENTLY on window close.  That is data
 * loss, which is why this sits in Tier 0 rather than among the polish.
 *
 * SDL's own message box is used rather than a drawn-in-canvas prompt: it is
 * modal to the window on every platform, it takes Escape and Return without
 * this loop having to route them, and it cannot be missed behind the editor.
 * Returns 1 if it is safe to close. */
static int confirm_close(SDL_Window *win)
{
    static const SDL_MessageBoxButtonData btn[] = {
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "Save"    },
        { 0,                                       1, "Discard" },
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 2, "Cancel"  },
    };
    SDL_MessageBoxData box;
    char msg[128];
    int n = uc_host_dirty_count(), id = 2;

    if (n <= 0) return 1;
    snprintf(msg, sizeof msg, "%d unsaved %s.\nSave before closing?",
             n, n == 1 ? "editor" : "editors");

    memset(&box, 0, sizeof box);
    box.flags      = SDL_MESSAGEBOX_WARNING;
    box.window     = win;
    box.title      = "UnoCode";
    box.message    = msg;
    box.numbuttons = 3;
    box.buttons    = btn;

    /* If the dialog cannot be shown at all, treat it as Cancel: refusing to
     * close is recoverable, closing over unsaved work is not. */
    if (SDL_ShowMessageBox(&box, &id) != 0) return 0;
    if (id == 1) return 1;                       /* discard */
    if (id != 0) return 0;                       /* cancel, or the window's X */
    if (uc_host_save_all() == 0) return 1;
    /* Only untitled editors can still be dirty here, and they have nowhere to
     * be saved to without a dialog this task does not own yet. */
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "UnoCode",
                             "Some editors are untitled and were not saved.\n"
                             "Use Save As, or close again to discard them.",
                             win);
    return 0;
}

/* ---- render --------------------------------------------------------------- */

static void render_frame(void) { unoui_render_ui(&UI); }

static int write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    int i, n = FB_W * FB_H;
    if (!f) { perror(path); return 0; }
    fprintf(f, "P6\n%d %d\n255\n", FB_W, FB_H);
    for (i = 0; i < n; i++) {
        unsigned px = fb[i];
        unsigned char rgb[3] = { px & 0xFF, (px >> 8) & 0xFF, (px >> 16) & 0xFF };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return 1;
}

static void boot_app(void)
{
    APP = uno_app_main(0);
    unoui_ui_init(&UI, pc64_shell_theme(), FB_W, FB_H);
    APP->build(&WIN);
    unoui_ui_add(&UI, &WIN);
    unoui_fullscreen(&UI, &WIN);
    if (APP->opened) APP->opened();

    /* State the workspace explicitly.  The core opens the PREFERRED volume as
     * its workspace at boot, which is right on pc64 - there the user's stick is
     * both - and wrong here the moment UCD-05 pointed the preferred volume at
     * HOME: the folder on the command line vanished and the explorer showed the
     * settings directory instead.  On a desktop the two are separate ideas, so
     * say which is which rather than letting one imply the other. */
    uc_open_folder(0, "");

    /* --open: a path relative to the workspace volume. Split off the directory
     * the way the core's own openers do, since uc_doc_open takes them apart. */
    if (g_open_file) {
        char dir[512];
        const char *name = g_open_file, *p;
        size_t dn;
        for (p = g_open_file; *p; p++)
            if (*p == '/' || *p == '\\') name = p + 1;
        dn = (size_t)(name - g_open_file);
        if (dn >= sizeof dir) dn = sizeof dir - 1;
        memcpy(dir, g_open_file, dn);
        while (dn && (dir[dn - 1] == '/' || dir[dn - 1] == '\\')) dn--;
        dir[dn] = 0;
        uc_doc_open(0, dir, name);
        host_mark_dirty();
    }
}

/* --type: feed UTF-8 text in as canvas character events, exactly as
 * SDL_TEXTINPUT would.  This is the headless half of the input road, and it
 * exists so the gate can assert things a screenshot cannot: that what was
 * typed is what reached the buffer, and that saving put those same bytes back
 * on disk.  Without it "UTF-8 round-trips" is a claim, not a check. */
static void type_text(const char *text)
{
    unoui_event e;
    const char *p = text;
    int left = (int)strlen(text);
    while (left > 0) {
        int cp, n = uno_u8_get(p, left, &cp);
        if (n <= 0) break;
        p += n; left -= n;
        memset(&e, 0, sizeof e);
        e.kind = UI_EV_CHAR;
        e.ch   = cp;
        feed(&e);
    }
}

/* --keys: navigation keys, one letter each, on the canvas road.
 *   L R U D  arrows      H E  Home/End      B  Backspace   X  Delete
 * Enough to check the thing typing cannot: that a caret STEP is a character
 * and not a byte.  "LLB" on a line of accented text has to remove one whole
 * character three back, and leave valid UTF-8 behind. */
static void press_keys(const char *keys)
{
    unoui_event e;
    for (; *keys; keys++) {
        int k;
        switch (*keys) {
        case 'L': k = UI_KEY_LEFT;      break;
        case 'R': k = UI_KEY_RIGHT;     break;
        case 'U': k = UI_KEY_UP;        break;
        case 'D': k = UI_KEY_DOWN;      break;
        case 'H': k = UI_KEY_HOME;      break;
        case 'E': k = UI_KEY_END;       break;
        case 'B': k = UI_KEY_BACKSPACE; break;
        case 'X': k = UI_KEY_DELETE;    break;
        default:  continue;
        }
        memset(&e, 0, sizeof e);
        e.kind = UI_EV_KEY;
        e.key  = k;
        feed(&e);
    }
}

/* headless: boot, run a few frames, snapshot, exit.  The whole editor core
 * renders in software into fb[], so "can it draw the workbench" needs no
 * display server - this is the build gate on a bare CI box. */
/* Run real frames for real milliseconds.  SDL_Delay rather than a spin so the
 * server gets the CPU: it is doing the work we are waiting for. */
static void lsp_settle(int ms)
{
    /* The CLOCK, not a count of delays.  Counting 300 x SDL_Delay(10) as three
     * seconds is wrong by however long the frames took, and on a cold first run
     * - where clangd is being paged in from disk and is the slowest thing on
     * the machine - the frames are exactly what gets slow.  That reading a
     * three-second wait as a sixty-second one, and made the client's own
     * initialize timeout fire and look like a bug in the client. */
    unsigned long end = host_ms() + (unsigned long)ms;
    while (host_ms() < end) {
        APP->frame();
        UI.ticks++;
        SDL_Delay(10);
    }
}

static void lsp_report(void)
{
    int ch = uc_output_channel("Language Server");
    int n = uc_output_lines(ch), i;
    static const char *kState[] = { "off", "starting", "ready", "dead" };
    printf("lsp: %d server(s)\n", uc_lsp_count());
    for (i = 0; i < uc_lsp_count(); i++) {
        void *s = uc_lsp_at(i);
        int st = uc_lsp_state(s);
        printf("lsp: [%s] state=%s restarts=%d\n", uc_lsp_name(s),
               (st >= 0 && st < 4) ? kState[st] : "?", uc_lsp_restarts(s));
    }
    {   /* What actually reached the editor's Problems model - the half the
         * traffic log cannot show.  A publishDiagnostics arriving is not the
         * same claim as a squiggle being placeable. */
        int np = uc_problems_total(), j;
        printf("lsp: %d problem(s)\n", np);
        for (j = 0; j < np; j++) {
            void *p = uc_problem_at(j);
            printf("lsp# %s:%d:%d-%d sev=%d %s\n", uc_problem_file(p),
                   uc_problem_line(p), uc_problem_col(p), uc_problem_endcol(p),
                   uc_problem_sev(p), uc_problem_msg(p));
        }
    }
    printf("lsp: --- %d traffic lines ---\n", n);
    for (i = 0; i < n; i++) printf("lsp| %s\n", uc_output_line(ch, i));
}

static void suggest_report(void)
{
    static const char *kKind[] = { "text", "method", "function", "variable",
                                   "class", "keyword", "snippet", "file",
                                   "property" };
    int n = uc_suggest_count(), i, k;
    printf("sug: %d item(s), source=%s\n", n,
           uc_suggest_from_server() ? "server" : "local");
    for (i = 0; i < n; i++) {
        k = uc_suggest_kind_at(i);
        printf("sug# %-28s [%s] %-8s | %s\n", uc_suggest_label(i),
               uc_suggest_insert(i),
               (k >= 0 && k < 9) ? kKind[k] : "?", uc_suggest_detail(i));
    }
}

/* Where the caret is now, as file:line:col - the only way a headless run can
 * say that a jump landed, and the only way it can say that Alt+Left came back
 * to the place it left rather than merely to the same file. */
static void where(const char *tag)
{
    void *d = uc_doc_active();
    char title[64];
    int off;
    if (!d) { printf("nav: %s no document\n", tag); return; }
    title[0] = 0;
    uc_doc_title(d, title, sizeof title);
    off = uc_doc_caret(d);
    printf("nav: %s %s:%d:%d\n", tag, title,
           uc_line_of(d, off) + 1, uc_col_of(d, off) + 1);
}

/* Apply every --set, after boot (which loads the real settings) and before
 * anything reads one. */
static void apply_overrides(void)
{
    int i;
    for (i = 0; i < g_nset; i++) {
        const char *eq = strchr(g_set[i], '=');
        char key[64];
        int n;
        if (!eq) continue;
        n = (int)(eq - g_set[i]);
        if (n <= 0 || n >= (int)sizeof key) continue;
        memcpy(key, g_set[i], (unsigned long)n);
        key[n] = 0;
        uc_cfg_override(key, eq + 1);
    }
}

/* --grammar <langid>=<path> */
static void load_grammar(void)
{
    const char *eq = g_grammar ? strchr(g_grammar, '=') : 0;
    char id[24];
    int n, lang;
    if (!eq) { printf("gram: expected <langid>=<file>\n"); return; }
    n = (int)(eq - g_grammar);
    if (n <= 0 || n >= (int)sizeof id) { printf("gram: bad language id\n"); return; }
    memcpy(id, g_grammar, (unsigned long)n);
    id[n] = 0;
    lang = uc_lang_by_id(id);
    if (lang < 0) { printf("gram: no language %s\n", id); return; }
    printf("gram: %s <- %s: %s\n", id, eq + 1,
           uc_lang_load_grammar(lang, uc_ws_vol(), eq + 1) ? "loaded" : "FAILED");
    {   /* what the loader wrote into the Log channel: the counters that say
         * how much of the grammar actually survived */
        int ch = uc_output_channel("Log");
        int n = uc_output_lines(ch), j;
        for (j = 0; j < n; j++) {
            const char *l = uc_output_line(ch, j);
            if (l[0]) printf("gram| %s\n", l);
        }
    }
}

/* Every line's colouring, as runs.  Runs rather than per character because the
 * claim under test is "this stretch is inside the outer rule", and a per
 * character dump makes that claim unreadable. */
static void scopes_report(void)
{
    void *d = uc_doc_active();
    short sc[2048];        /* UC_HL_MAXLINE, which this host may not include */
    int line, n = d ? uc_line_count(d) : 0;
    unsigned long rx0 = uc_hl_rx_calls();
    printf("scp: %d line(s)\n", n);
    for (line = 0; line < n; line++) {
        int len = uc_line_scopes(d, line, sc, 2048);
        int i = 0;
        printf("scp# %d", line + 1);
        while (i < len) {
            int j = i;
            while (j < len && sc[j] == sc[i]) j++;
            printf("  %d-%d:%s", i, j - 1,
                   sc[i] ? uc_scope_name(sc[i]) : "-");
            i = j;
        }
        printf("\n");
    }
    /* The COST, as a count rather than a clock.  A missing match cache took a
     * real grammar from a millisecond a line to twenty-five, and every scope
     * assertion and every screenshot was identical either way. */
    printf("scp= %lu regex executions for %d line(s)\n",
           uc_hl_rx_calls() - rx0, n);
}

static int shot_mode(const char *out)
{
    int i;
    boot_app();
    apply_overrides();
    if (g_grammar) load_grammar();
    /* Lay the workbench out BEFORE typing into it.  uc_edit_reveal() scrolls
     * the caret into view against the editor's rect, and until something has
     * painted, that rect is empty - so it computes "one column fits" and
     * scrolls the whole typed line off to the left.  A real user types after
     * the window exists; the headless hands have to do the same. */
    render_frame();
    if (g_lsp_ms) { uc_lsp_trace_on(); lsp_settle(g_lsp_ms); }  /* start, init, didOpen */
    if (g_type_text) { type_text(g_type_text); host_mark_dirty(); }
    if (g_keys)      { press_keys(g_keys);    host_mark_dirty(); }
    if (g_lsp_ms) lsp_settle(g_lsp_ms);       /* the debounce, then didChange */
    if (g_do_save) {
        void *d = uc_doc_active();
        /* Through the SAME path Ctrl+S takes, so editor.formatOnSave is
         * exercised by the hand as well as by the key.  A test hand that
         * bypassed the command would test a save nobody ever performs. */
        if (d && !uc_save_with_format(d)) uc_doc_save(d);
    }
    if (g_suggest) {
        void *d = uc_doc_active();
        if (d) uc_suggest_open(d, 1);
        /* the request goes out here; the reply lands a few frames later */
        lsp_settle(g_lsp_ms ? g_lsp_ms : 2000);
        suggest_report();
    }
    if (g_hover) {
        void *d = uc_doc_active();
        if (d) uc_hover_at(d, uc_doc_caret(d), 400, 200);
        lsp_settle(g_lsp_ms ? g_lsp_ms : 2000);
        printf("hov: %s\n", uc_hover_active() ? "shown" : "nothing");
        if (uc_hover_active()) {
            const char *t = uc_hover_text();
            printf("hov| ");
            for (; *t; t++) { putchar(*t == '\n' ? '\n' : *t); if (*t == '\n') printf("hov| "); }
            printf("\n");
        }
    }
    if (g_def) {
        void *d = uc_doc_active();
        if (d) uc_goto_definition(d);
        lsp_settle(g_lsp_ms ? g_lsp_ms : 2000);
        where("def");
        /* and back, and forward again: the stack is what makes the jump usable */
        if (uc_nav_back()) { lsp_settle(300); where("back"); }
        else printf("nav: back refused\n");
        if (uc_nav_forward()) { lsp_settle(300); where("fwd"); }
        else printf("nav: forward refused\n");
    }
    if (g_refs) {
        void *d = uc_doc_active();
        if (d) uc_find_references(d);
        lsp_settle(g_lsp_ms ? g_lsp_ms : 2000);
        {
            int n = uc_results_count(), j;
            printf("ref: %d result(s)\n", n);
            for (j = 0; j < n; j++)
                printf("ref# %s:%d  %s\n", uc_results_path(j),
                       uc_results_line(j), uc_results_text(j));
        }
    }
    if (g_rename) {
        void *d = uc_doc_active();
        int k;
        if (d) uc_rename_symbol(d);
        /* The box opens PRE-FILLED with the symbol as it stands, so typing
         * would append to it.  Clear it first, then type, then Enter - which
         * is what a user does too. */
        for (k = 0; k < 80; k++) uc_quick_key(UI_KEY_BACKSPACE, 0, 0);
        type_text(g_rename);
        uc_quick_key(UI_KEY_ENTER, 0, 0);
        lsp_settle(g_lsp_ms ? g_lsp_ms : 3000);
        {   /* uc_host_save_all() returns what it could NOT save, so 0 is
             * success.  Both numbers are printed because "3 files changed" and
             * "0 of them refused to write" are different claims. */
            int edited = uc_host_dirty_count();
            int left = uc_host_save_all();
            printf("ren: %d file(s) edited, %d unsaved\n", edited, left);
        }
    }
    if (g_format) {
        void *d = uc_doc_active();
        if (d) uc_format_document(d);
        lsp_settle(g_lsp_ms ? g_lsp_ms : 3000);
        {
            int edited = uc_host_dirty_count();
            int left = uc_host_save_all();
            printf("fmt: %d file(s) edited, %d unsaved\n", edited, left);
        }
    }
    if (g_scopes) scopes_report();
    if (g_lsp_ms) { lsp_settle(g_lsp_ms); lsp_report(); }
    for (i = 0; i < 5; i++) { APP->frame(); UI.ticks++; }
    render_frame();
    if (!write_ppm(out)) return 1;
    fprintf(stderr, "shot: %dx%d -> %s\n", FB_W, FB_H, out);
    if (APP->closed) APP->closed();
    return 0;
}

/* ---- main ----------------------------------------------------------------- */

int main(int argc, char **argv)
{
    SDL_Window   *win;
    SDL_Renderer *ren;
    SDL_Texture  *tex;
    const char   *workdir = ".";
    const char   *shot = 0;
    int i, running = 1;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--open") && i + 1 < argc) g_open_file = argv[++i];
        else if (!strcmp(argv[i], "--type") && i + 1 < argc) g_type_text = argv[++i];
        else if (!strcmp(argv[i], "--keys") && i + 1 < argc) g_keys = argv[++i];
        else if (!strcmp(argv[i], "--save")) g_do_save = 1;
        else if (!strcmp(argv[i], "--lsp") && i + 1 < argc) g_lsp_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--suggest")) g_suggest = 1;
        else if (!strcmp(argv[i], "--hover")) g_hover = 1;
        else if (!strcmp(argv[i], "--def")) g_def = 1;
        else if (!strcmp(argv[i], "--refs")) g_refs = 1;
        else if (!strcmp(argv[i], "--rename") && i + 1 < argc) g_rename = argv[++i];
        else if (!strcmp(argv[i], "--format")) g_format = 1;
        else if (!strcmp(argv[i], "--grammar") && i + 1 < argc) g_grammar = argv[++i];
        else if (!strcmp(argv[i], "--scopes")) g_scopes = 1;
        else if (!strcmp(argv[i], "--set") && i + 1 < argc) {
            if (g_nset < 8) g_set[g_nset++] = argv[++i];
            else i++;
        }
        else workdir = argv[i];
    }

    g_workdir = workdir;
    if (SDL_Init(shot ? SDL_INIT_TIMER : SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    setup_volumes(workdir);

    if (shot) return shot_mode(shot);

    host_geom_load(&G);
    win = SDL_CreateWindow("UnoCode", G.x, G.y, G.w, G.h,
                           SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                           (G.maximized ? SDL_WINDOW_MAXIMIZED : 0));
    if (!win) { fprintf(stderr, "SDL window: %s\n", SDL_GetError()); return 1; }
    SDL_SetWindowMinimumSize(win, 700, 460);            /* the app's own min */
    /* Drop events are NOT on by default in SDL2 - asking for them is what
     * makes UCD-19 arrive at all, and the absence looks exactly like a
     * window manager that refuses drops. */
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    SDL_SetWindowMaximumSize(win, FB_MAX_W, FB_MAX_H);  /* fb ceiling        */

    ren = SDL_CreateRenderer(win, -1, 0);
    if (!ren) { fprintf(stderr, "SDL renderer: %s\n", SDL_GetError()); return 1; }
    tex = 0;

    boot_app();
    apply_overrides();
    sync_dpi(win, ren, &tex);       /* AFTER boot: it re-derives font metrics */
    /* Reopen last session's editors, but only for the SAME folder: the tab
     * list is workspace-relative, and replaying it against a different tree
     * would open whatever happened to share those names. */
    if (G.ntab && G.folder[0] && !strcmp(G.folder, host_dialog_root()))
        host_geom_reopen(&G);
    host_geom_note(win);
    host_clip_init();
    host_cursors_init();
    host_title_update(win);
    host_recent_add(host_dialog_root());
    SDL_StartTextInput();

    while (running) {
        SDL_Event ev;
        unoui_event e;

        if (SDL_WaitEventTimeout(&ev, 15)) {
          do {
            switch (ev.type) {
            case SDL_QUIT:
                running = confirm_close(win) ? 0 : 1;
                break;
            case SDL_DROPFILE:
                /* SDL hands us a string it allocated; freeing it is ours to do
                 * (UCD-19).  One event per file, so a multi-file drop arrives
                 * as several of these and each opens its own tab. */
                if (ev.drop.file) {
                    host_adopt_path(ev.drop.file);
                    SDL_free(ev.drop.file);
                    host_mark_dirty();
                }
                break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                    ev.window.event == SDL_WINDOWEVENT_MOVED) {
                    /* Ask SDL for the sizes rather than trusting the event's:
                     * data1/data2 are POINTS, and dragging between a 1x and a
                     * 2x monitor changes the pixel size without changing them
                     * at all. */
                    sync_dpi(win, ren, &tex);
                    host_geom_note(win);
                } else if (ev.window.event == SDL_WINDOWEVENT_EXPOSED) {
                    host_mark_dirty();
                } else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                    /* to copy in another application you had to focus it, so
                     * coming back is the trigger that cannot be missed even
                     * where SDL_CLIPBOARDUPDATE is unreliable */
                    host_clip_pull();
                }
                break;
            case SDL_CLIPBOARDUPDATE:
                host_clip_pull();
                break;
            case SDL_MOUSEMOTION:
                memset(&e, 0, sizeof e);
                e.kind = UI_EV_MOUSE_MOVE;
                e.x = to_px(ev.motion.x); e.y = to_px(ev.motion.y);
                e.mods = ui_mods(SDL_GetModState());
                feed(&e);
                host_cursor_update(e.x, e.y);
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    memset(&e, 0, sizeof e);
                    e.kind = (ev.type == SDL_MOUSEBUTTONDOWN)
                             ? UI_EV_MOUSE_DOWN : UI_EV_MOUSE_UP;
                    e.x = to_px(ev.button.x); e.y = to_px(ev.button.y);
                    e.mods = ui_mods(SDL_GetModState());
                    feed(&e);
                }
                break;
            case SDL_MOUSEWHEEL: {
                /* Trackpads report FRACTIONS of a notch.  Translating the
                 * integer field 1:1, as phase 0 did, threw those away - so
                 * two-finger scrolling moved in jumps or, below one notch,
                 * not at all - and passed a fast wheel spin straight through
                 * as a page jump.  Accumulate the precise value and spend it
                 * a notch at a time, capped so one flick cannot leap the
                 * document. */
                int mx, my, notches;
                float precise = ev.wheel.preciseY;
                if (precise == 0.0f) precise = (float)ev.wheel.y;
                g_wheel_acc += precise;
                notches = (int)g_wheel_acc;
                if (!notches) break;
                g_wheel_acc -= (float)notches;
                if (notches > 5) notches = 5;
                if (notches < -5) notches = -5;
                SDL_GetMouseState(&mx, &my);
                memset(&e, 0, sizeof e);
                e.kind = UI_EV_WHEEL;
                e.x = to_px(mx); e.y = to_px(my);
                e.wheel = -notches;             /* unoui: positive = down */
                feed(&e);
                break;
            }
            case SDL_KEYDOWN:
                on_key_down(&ev.key);
                break;
            case SDL_TEXTINPUT:
                on_text(ev.text.text);
                break;
            }
          } while (SDL_PollEvent(&ev));
          /* a copy or cut in this batch - by key, palette, menu or extension -
           * reaches the OS clipboard here, on the one road all of them share */
          host_clip_push();
        }

        APP->frame();
        UI.ticks++;

        if (host_take_dirty()) {
            host_title_update(win);      /* the dirty marker changes with it */
            render_frame();
            SDL_UpdateTexture(tex, 0, fb, uno_fb_w * (int)sizeof(fb_px));
            SDL_RenderClear(ren);
            SDL_RenderCopy(ren, tex, 0, 0);
            SDL_RenderPresent(ren);
        }
    }

    /* the CURRENT root, which Open Folder may have moved since launch */
    host_geom_save(win, host_dialog_root());
    if (APP->closed) APP->closed();
    SDL_Quit();
    return 0;
}
