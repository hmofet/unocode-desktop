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
      host/host_pick_unix.c host/host_net.c"

# TLS.  BearSSL comes out of the submodule and so does the trust store, which
# is the point: tls_ca.c is generated, self-contained and includes only
# bearssl.h, so the fourteen roots this validates against are the SAME fourteen
# the device validates against.  A certificate that works there works here.
#
# The eight excluded files are the ones pc64 excludes, and the list must stay
# identical.  Seven pull CPU intrinsics.  The eighth, sysrng.c, would be the
# obvious one to keep on a hosted build - and it is dead here anyway, because
# upstream's bearssl/src/config.h forces BR_USE_URANDOM and BR_USE_WIN32_RAND
# to 0.  host_net.c supplies br_prng_seeder_system() and injects OS entropy
# itself, exactly as pc64's tls.c does.  Turning those flags on instead would
# mean patching a shared upstream file so that a hosted build gets different
# crypto configuration from the device, which is how two builds stop being the
# same TLS.
BSSL_SKIP="ghash_pclmul sysrng aes_x86ni aes_x86ni_cbcdec aes_x86ni_cbcenc \
           aes_x86ni_ctr aes_x86ni_ctrcbc chacha20_sse2"
BSSL=""
for _c in $(find "$U/pc64/bearssl/src" -name '*.c' | sort); do
    _b=$(basename "$_c" .c)
    case " $BSSL_SKIP " in *" $_b "*) continue;; esac
    BSSL="$BSSL $_c"
done
TLS="$U/pc64/tls_ca.c"

INC="-Ihost/compat -I$U/pc64 -Icore -I$U/unoui -I$U/unojs -Ihost \
     -I$U/pc64/bearssl/inc -I$U/pc64/bearssl/src"
DEFS="-DUNO_PC64"
WARN="-Wall -Wno-unused-parameter -Werror=implicit-function-declaration \
      -Werror=incompatible-pointer-types"
