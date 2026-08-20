/* ===========================================================================
 * host/compat/uno_appdesc.h - shadow the app-descriptor macro on the desktop.
 *
 * WHY THIS EXISTS.  UNO_APP_DESC() parks a launcher-metadata block in a named
 * section (".unodesc") so pc64's module loader can read an app's name, icon
 * and category out of a .UNO file WITHOUT loading it.  The desktop has no
 * module loader - the editor is the executable - so the block is dead weight
 * here, and on Mach-O it is worse than dead: a bare ".unodesc" is a hard error,
 * because Apple's assembler requires "__SEGMENT,__section".
 *
 * This header is found BEFORE upstream's (host/compat is first on the include
 * path), pulls the real one in with #include_next so every declaration in it
 * stays live, and then neutralises just the one macro.  It is not a patch:
 * upstream is untouched and a new declaration added there arrives here for
 * free.  All three desktop platforms use it, so none of them compiles a
 * different uc_main.c than the others - which is how this bug reached macOS
 * after Windows and Linux had both been green.
 *
 * Upstream fix, filed as a roadmap item: spell the section per-target inside
 * uno_appdesc.h, so the freestanding builds keep their block and hosted ones
 * need no shim.
 * ======================================================================== */
#ifndef UNOCODE_HOST_APPDESC_SHIM_H
#define UNOCODE_HOST_APPDESC_SHIM_H

#include_next "uno_appdesc.h"

#undef  UNO_APP_DESC
#define UNO_APP_DESC(body) /* no module loader on the desktop */

#endif
