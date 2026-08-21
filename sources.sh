# ===========================================================================
# sources.sh - the compile list, in ONE place.
#
# Sourced by build.sh and build-mac.sh, which is the point: they used to carry
# a copy each, under a comment reading "identical list to build.sh", and the
# copies stopped being identical the moment a file was added.  The way that
# fails is the worst available - the Linux and Windows builds are green, and
# the mac one dies at the LINKER with a wall of undefined symbols, hours later
# on a different machine.
#
# Expects $U to be set to the upstream submodule root.
# ===========================================================================

# The editor core lives HERE, in core/, and this repository is its home: see
# core/README.md.  UnoDOS vendors it, not the other way round.  Its three
# foundations DO come from the submodule, because they belong to that OS and
# have many other consumers there: unoui (the toolkit), unojs (the extension
# host's engine), and fb + pc64_font (software rendering).  One theme comes
# along for the window-metrics hook.
UC=$(ls core/uc_*.c)
UNOUI="$U/unoui/unoui.c $U/unoui/unoui_input.c $U/unoui/unoui_anim.c \
       $U/unoui/unoui_wmanim.c $U/unoui/themes/theme_unodos.c"
UNOJS=$(ls "$U"/unojs/ujs_*.c)
FB="$U/pc64/fb.c $U/pc64/pc64_font.c"

# The host shim.  host_pick_win.c and host_pick_unix.c are BOTH listed and both
# compile everywhere: each is wrapped in the #ifdef for its platform, so the
# one that does not apply contributes an empty object file.  That is deliberate
# - a platform-conditional source LIST is a second place for the two builds to
# disagree, which is exactly the bug this file exists to prevent.
HOST="host/main.c host/host_fs.c host/host_shell.c host/host_clip.c \
      host/host_win.c host/host_dialog.c host/host_pick_win.c \
      host/host_pick_unix.c"

INC="-Ihost/compat -I$U/pc64 -Icore -I$U/unoui -I$U/unojs -Ihost"
DEFS="-DUNO_PC64"
WARN="-Wall -Wno-unused-parameter -Werror=implicit-function-declaration \
      -Werror=incompatible-pointer-types"
