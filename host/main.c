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

static const char *g_open_file;    /* --open, resolved after the app is up */

unsigned long host_ms(void) { return (unsigned long)SDL_GetTicks64(); }

/* ---- volume setup --------------------------------------------------------- */

static void setup_volumes(const char *workdir)
{
    char *base = SDL_GetBasePath();          /* dir of the executable */
    char res[900], home[900];
    const char *env;

    host_fs_add_volume("WORK", workdir, 1);

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

    /* per-user data dir - reserved now, the settings home in phase 0.5 */
#ifdef _WIN32
    env = getenv("APPDATA");
    snprintf(home, sizeof home, "%s\\UnoCode", env ? env : ".");
#else
    env = getenv("HOME");
    snprintf(home, sizeof home, "%s/.unocode", env ? env : ".");
#endif
    host_fs_add_volume("HOME", home, 1);
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

static void on_text(const char *text)
{
    unoui_event e;
    const unsigned char *p;
    SDL_Keymod m = SDL_GetModState();
    if (m & (KMOD_CTRL | KMOD_ALT)) return;      /* chords are not typing */
    for (p = (const unsigned char *)text; *p; p++) {
        if (*p >= 0x80) continue;                /* ASCII core for phase 0 */
        memset(&e, 0, sizeof e);
        e.kind = UI_EV_CHAR;
        e.ch   = *p;
        e.mods = ui_mods(m);
        feed(&e);
    }
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

/* headless: boot, run a few frames, snapshot, exit.  The whole editor core
 * renders in software into fb[], so "can it draw the workbench" needs no
 * display server - this is the build gate on a bare CI box. */
static int shot_mode(const char *out)
{
    int i;
    boot_app();
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
        else workdir = argv[i];
    }

    if (SDL_Init(shot ? SDL_INIT_TIMER : SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    setup_volumes(workdir);

    if (shot) return shot_mode(shot);

    win = SDL_CreateWindow("UnoCode",
                           SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           uno_fb_w, uno_fb_h,
                           SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) { fprintf(stderr, "SDL window: %s\n", SDL_GetError()); return 1; }
    SDL_SetWindowMinimumSize(win, 700, 460);            /* the app's own min */
    SDL_SetWindowMaximumSize(win, FB_MAX_W, FB_MAX_H);  /* fb ceiling        */

    ren = SDL_CreateRenderer(win, -1, 0);
    if (!ren) { fprintf(stderr, "SDL renderer: %s\n", SDL_GetError()); return 1; }
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888,
                            SDL_TEXTUREACCESS_STREAMING, uno_fb_w, uno_fb_h);

    boot_app();
    SDL_StartTextInput();

    while (running) {
        SDL_Event ev;
        unoui_event e;

        if (SDL_WaitEventTimeout(&ev, 15)) do {
            switch (ev.type) {
            case SDL_QUIT:
                running = 0;
                break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    int w = ev.window.data1, h = ev.window.data2;
                    if (w > FB_MAX_W) w = FB_MAX_W;
                    if (h > FB_MAX_H) h = FB_MAX_H;
                    if (w > 0 && h > 0 && (w != uno_fb_w || h != uno_fb_h)) {
                        uno_fb_w = w;
                        uno_fb_h = h;
                        UI.screen_w = w;
                        UI.screen_h = h;
                        UI.work = (unoui_rect){ 0, 0, w, h };
                        host_workarea_w = w;
                        host_workarea_h = h;
                        SDL_DestroyTexture(tex);
                        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888,
                                                SDL_TEXTUREACCESS_STREAMING, w, h);
                        host_mark_dirty();
                    }
                } else if (ev.window.event == SDL_WINDOWEVENT_EXPOSED) {
                    host_mark_dirty();
                }
                break;
            case SDL_MOUSEMOTION:
                memset(&e, 0, sizeof e);
                e.kind = UI_EV_MOUSE_MOVE;
                e.x = ev.motion.x; e.y = ev.motion.y;
                e.mods = ui_mods(SDL_GetModState());
                feed(&e);
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    memset(&e, 0, sizeof e);
                    e.kind = (ev.type == SDL_MOUSEBUTTONDOWN)
                             ? UI_EV_MOUSE_DOWN : UI_EV_MOUSE_UP;
                    e.x = ev.button.x; e.y = ev.button.y;
                    e.mods = ui_mods(SDL_GetModState());
                    feed(&e);
                }
                break;
            case SDL_MOUSEWHEEL: {
                int mx, my;
                SDL_GetMouseState(&mx, &my);
                memset(&e, 0, sizeof e);
                e.kind = UI_EV_WHEEL;
                e.x = mx; e.y = my;
                e.wheel = -ev.wheel.y;          /* unoui: positive = down */
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

        APP->frame();
        UI.ticks++;

        if (host_take_dirty()) {
            render_frame();
            SDL_UpdateTexture(tex, 0, fb, uno_fb_w * (int)sizeof(fb_px));
            SDL_RenderClear(ren);
            SDL_RenderCopy(ren, tex, 0, 0);
            SDL_RenderPresent(ren);
        }
    }

    if (APP->closed) APP->closed();
    SDL_Quit();
    return 0;
}
