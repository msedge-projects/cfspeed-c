#!/usr/bin/env bash
#
# WHY this script exists
# ---------------------
# The Windows build links MSYS2's libcurl, which is GnuTLS-backed. GnuTLS reads
# its trust anchors from a *file*. Windows has no such file: its trust store is a
# registry-backed database that only schannel-backed curl can consult. So on
# Windows every HTTPS request dies with CURLE_PEER_FAILED_VERIFICATION (curl
# error 60) and cfspeed measures nothing at all.
#
# src/net.c therefore looks for a bundle named `cfspeed-ca.pem` sitting next to
# the executable and points CURLOPT_CAINFO at it. CI has to fetch one and put it
# there. Turning verification off instead would be a security regression on a
# tool whose entire job is trusting a remote endpoint, so that is not an option.
#
# WHY this script fails hard
# --------------------------
# A truncated or zero-byte CA file is *worse* than no file. It looks present to
# the loader, so the failure stops being loud ("no bundle found") and starts
# being silent (certificates that cannot be validated get dropped one at a time).
# So the download is validated in a temp file and only moved into place once it
# has been proven to be a real PEM bundle. A hard failure here is the good
# outcome.

set -euo pipefail

CA_URL='https://curl.se/ca/cacert.pem'

log()  { printf '%s\n' "$*" >&2; }
fail() { printf 'fetch-ca: error: %s\n' "$*" >&2; exit 1; }

# Destination is $1. "-" streams the bundle to stdout (validation still happens
# first, through the same temp file); with no argument the bundle lands in
# ./cfspeed-ca.pem, which is the exact name src/net.c probes for.
out="${1:-cfspeed-ca.pem}"
to_stdout=0
[ "$out" = '-' ] && to_stdout=1

tmp="$(mktemp "${TMPDIR:-/tmp}/cfspeed-ca.XXXXXX")" || fail 'cannot create a temp file'
trap 'rm -f "$tmp"' EXIT HUP INT TERM

# curl is the primary; wget is the fallback when curl is missing or unusable.
# Returns non-zero rather than exiting, so the caller owns the error message.
fetch() {
    if command -v curl >/dev/null 2>&1; then
        if curl -fsSL "$CA_URL" -o "$tmp"; then
            return 0
        fi
        log 'fetch-ca: curl could not retrieve the bundle; falling back to wget'
    else
        log 'fetch-ca: curl not found; falling back to wget'
    fi

    if command -v wget >/dev/null 2>&1; then
        if wget -q -O "$tmp" "$CA_URL"; then
            return 0
        fi
    fi
    return 1
}

if ! fetch; then
    fail "could not download the CA bundle from $CA_URL (tried curl, then wget)"
fi

# --- validation, before anything is installed ---------------------------------

[ -f "$tmp" ] || fail 'the download did not produce a file'

if [ ! -s "$tmp" ]; then
    fail 'the download is empty - an empty CA bundle silently disables verification'
fi

if ! grep -q 'BEGIN CERTIFICATE' "$tmp"; then
    fail 'the download contains no BEGIN CERTIFICATE marker - almost certainly an error page, not a bundle'
fi

# A 10-byte slice of a bundle starts with "-----BEGIN" and would otherwise sail
# through the check above. Requiring the closing marker catches that.
if ! grep -q 'END CERTIFICATE' "$tmp"; then
    fail 'the download has BEGIN CERTIFICATE but no END CERTIFICATE - it looks truncated'
fi

bytes="$(wc -c <"$tmp" | tr -d '[:space:]')"
certs="$(grep -c 'BEGIN CERTIFICATE' "$tmp" || true)"

# --- install ------------------------------------------------------------------

if [ "$to_stdout" -eq 1 ]; then
    cat "$tmp"
    log "fetch-ca: $certs certificates ($bytes bytes) -> stdout"
else
    # Only now, with a proven-good file in hand, is the destination touched. An
    # existing good bundle survives every failure above.
    mv -f "$tmp" "$out" || fail "validated the bundle but could not install it at $out"
    log "fetch-ca: $certs certificates ($bytes bytes) -> $out"
fi