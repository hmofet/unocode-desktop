#!/bin/sh
# net_test.sh - net_test.c, with an untrusted TLS server to point it at.
#
# The check this exists for is a NEGATIVE one: a server whose certificate is
# signed by nobody must be refused.  That needs a server, a certificate and a
# port, none of which a C test can conjure, so they are made here and thrown
# away afterwards.
#
# The certificate is generated fresh into build/ every run rather than
# committed.  A checked-in test certificate expires, and the day it does, this
# test starts passing for the wrong reason - the handshake would fail on dates
# instead of on trust, and the assertion that we REJECT untrusted issuers
# would no longer be testing anything.
#
# Skips (rather than fails) with no openssl or no python3: it is a test that
# needs two tools the seam itself does not.
set -e
cd "$(dirname "$0")/.."
U=upstream/unodos          # sources.sh expects the caller to have set this
. ./sources.sh

CC="${CC:-gcc}"
PORT="${NET_TEST_PORT:-14443}"
D=build/net_test_tmp

command -v openssl >/dev/null 2>&1 || { echo "net_test: SKIPPED - no openssl" >&2; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "net_test: SKIPPED - no python3" >&2; exit 0; }

mkdir -p build "$D"
bearssl_a=build/bearssl.a
[ -f "$bearssl_a" ] || { echo "net_test: run ./build.sh first (needs $bearssl_a)" >&2; exit 1; }

# shellcheck disable=SC2086
$CC -O1 -g $WARN $DEFS $INC tools/net_test.c host/host_net.c $TLS \
    -o build/net_test "$bearssl_a" -lm

# A throwaway self-signed certificate.  Its subject deliberately does NOT
# matter: it is refused for having no chain to a trusted root, which is a
# verdict reached before any name is compared.
openssl req -x509 -newkey rsa:2048 -keyout "$D/k.pem" -out "$D/c.pem" \
    -days 2 -nodes -subj "/CN=unocode.test" >/dev/null 2>&1

python3 - "$D/c.pem" "$D/k.pem" "$PORT" <<'PY' &
import socket, ssl, sys
cert, key, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(cert, key)
srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", port))
srv.listen(4)
srv.settimeout(30)
# Serve until the client has had its go. Every handshake here is EXPECTED to
# fail - the client is supposed to reject us - so the errors are the point and
# are swallowed rather than reported.
for _ in range(4):
    try:
        c, _a = srv.accept()
        try:
            ctx.wrap_socket(c, server_side=True)
        except Exception:
            pass
        c.close()
    except Exception:
        break
srv.close()
PY
SRV=$!
# shellcheck disable=SC2064
trap "kill $SRV 2>/dev/null || true; rm -rf $D" EXIT

# Give the listener a moment to bind. The client's own retry loop covers the
# rest, so this only has to beat the first connect().
python3 -c "import time; time.sleep(0.6)"

./build/net_test 127.0.0.1 "$PORT"
