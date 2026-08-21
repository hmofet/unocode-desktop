/* ===========================================================================
 * UnoCode Desktop - the host shim's own surface.
 *
 * The editor core (upstream/unodos/pc64/unocode) is consumed UNMODIFIED: it
 * reaches the outside world only through the ~30 externs it declares in
 * unocode.h (filesystem, fonts, timing, five pc64_shell_* hooks).  This tree
 * provides those symbols on top of SDL2 + the host OS, exactly the way the
 * pc64 shell provides them on top of UEFI.  Nothing here is included by
 * upstream code; the contract is the SYMBOLS, not this header.
 * ======================================================================== */
#ifndef UNOCODE_HOST_H
#define UNOCODE_HOST_H

/* ---- volumes (host_fs.c) --------------------------------------------------
 * UnoDOS addresses storage as small integer volumes; the host maps each to a
 * directory.  Phase 0 ships three:
 *   0  WORK  the folder being edited (argv[1] or cwd)   - preferred volume
 *   1  APP   bundled resources next to the exe (fonts at the root, EXT\)
 *   2  HOME  per-user data dir (reserved; settings move here in phase 0.5)
 * Paths cross the seam in FAT spelling ('\\' separators, case-insensitive);
 * host_fs resolves them case-insensitively so a case-sensitive host fs (Linux)
 * behaves like the FAT the core was written against. */
int  host_fs_add_volume(const char *name, const char *root, int writable);

/* Which volume per-user state (settings, keybindings) belongs on.  Defaults to
 * 0, the workspace, which is exactly wrong for a desktop: main.c moves it to
 * HOME. */
void host_fs_set_pref_vol(int vol);

/* mkdir -p on a host path, for the per-user directory: nothing else creates
 * it, and uno_fs_mkdir cannot - it needs the volume root to exist already. */
int  host_fs_mkpath(const char *path);

/* Canonical absolute path, '/'-separated.  Everything downstream COMPARES
 * paths, and "." matches nothing - not even itself after a chdir. */
int  host_fs_abspath(const char *in, char *out, int cap);

/* Re-point a volume at a different directory, live.  Open Folder does this to
 * volume 0 instead of restarting, so the explorer, quick open and search all
 * re-root together - every one of them addresses the volume, not a path. */
int  host_fs_set_volume_root(int vol, const char *root);

/* ---- Open File / Open Folder (host_dialog.c + host_pick_*.c) --------------
 * The picker is native, because a user's places, bookmarks and network mounts
 * live in their file manager and an in-app list has none of them.  The editor
 * reaches it through pc64_shell_pick(), the same way it reaches every other
 * shell service; host_dialog.c is what turns an absolute path back into the
 * (volume, directory, name) the core addresses files by. */
void        host_dialog_set_root(const char *root);
const char *host_dialog_root(void);
int         host_pick_path(int want_folder, char *out, int cap);
void        host_recent_file(const char *home);
void        host_recent_load(void);
void        host_recent_add(const char *path);
int         host_recent_count(void);
const char *host_recent_at(int i);

/* ---- clipboard (host_clip.c) ----------------------------------------------
 * The core's clipboard and the OS's are SYNCHRONISED, not intercepted: copy,
 * cut and paste are reachable from the palette, the menus and an extension as
 * well as from Ctrl+C/X/V, and only a synchronising mirror serves all of them.
 * pull is cheap when nothing changed, so calling it on every plausible trigger
 * costs nothing. */
void host_clip_init(void);         /* seed both sides; adopt a pre-launch copy */
void host_clip_pull(void);         /* OS   -> core (clipboard/focus events)    */
void host_clip_push(void);         /* core -> OS   (after an event batch)      */

/* ---- window: title, pointer, geometry (host_win.c) ------------------------
 * What the OS knows about this window that the editor does not.  The editor is
 * ASKED rather than guessed at - uc_host_title() and uc_host_cursor_at() come
 * from the module - so the pointer cannot promise a resize where a click does
 * nothing. */
#define HOST_TABS 24

typedef struct {
    int  x, y, w, h, maximized;
    char folder[512];                  /* the workspace that was open       */
    char tab[HOST_TABS][192];          /* its open editors, workspace-relative */
    int  ntab;
} HostGeom;

struct SDL_Window;
void host_state_dir(const char *home);            /* where WINDOW.STA lives  */
void host_secret_dir(const char *home);           /* where SECRETS.DAT lives
                                                     (host_secret.c, UCD-48) */
void host_title_update(struct SDL_Window *win);   /* file - folder, + dirty  */
void host_cursors_init(void);
void host_cursor_update(int x, int y);            /* canvas pixels           */
void host_geom_defaults(HostGeom *g);
void host_geom_load(HostGeom *g);
void host_geom_note(struct SDL_Window *win);      /* on move/resize          */
void host_geom_save(struct SDL_Window *win, const char *folder);
void host_geom_reopen(const HostGeom *g);         /* after the app is up     */

/* ---- frame/dirty (host_shell.c) ------------------------------------------ */
int  host_take_dirty(void);        /* 1 if a repaint was requested since last */
void host_mark_dirty(void);

/* monotonic milliseconds; main.c owns the clock (SDL) */
unsigned long host_ms(void);

#endif
