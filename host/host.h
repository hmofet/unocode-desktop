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

/* ---- frame/dirty (host_shell.c) ------------------------------------------ */
int  host_take_dirty(void);        /* 1 if a repaint was requested since last */
void host_mark_dirty(void);

/* monotonic milliseconds; main.c owns the clock (SDL) */
unsigned long host_ms(void);

#endif
