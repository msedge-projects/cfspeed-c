#!/usr/bin/env bash
# Collect every runtime DLL cfspeed.exe needs on Windows, beside it.
#
# ldd reports only DIRECT imports. cfspeed.exe imports libcurl, and libcurl
# imports GnuTLS, which imports nettle, which imports six more libraries. One
# pass therefore finds a fraction of what has to ship, and the package dies on
# the user's machine with a missing-DLL error.
#
# Each round rescans everything collected so far and copies what it finds, so
# no state is carried between rounds. An earlier version piped ldd into a
# `while read` loop that queued its finds in a shell variable; that loop body
# runs in a subshell, every update died with it, and the walk silently
# collected exactly one level. A bounded number of independent rounds cannot
# fail that way.
#
# Only the toolchain's own DLLs are copied. Anything under /c/Windows is a
# system library already present on the user's machine.
set -uo pipefail

BIN=${BIN:-./cfspeed.exe}
STAGE=${STAGE:-./stage}
MAX_ROUNDS=8
ROOT=${MINGW_ROOT:-/mingw64}

if [ ! -f "$BIN" ]; then
  echo "collect-dlls: $BIN not found - build first" >&2
  exit 1
fi

rm -rf "$STAGE"
mkdir -p "$STAGE"
cp "$BIN" "$STAGE/"

for _ in $(seq 1 "$MAX_ROUNDS"); do
  : > found.txt
  for f in "$STAGE"/*.dll "$STAGE"/*.exe; do
    [ -f "$f" ] || continue
    ldd "$f" 2>/dev/null \
      | sed -n 's|.*=>[[:space:]]*/mingw64/\([^[:space:]]*\.dll\).*|\1|p' >> found.txt
  done

  [ -s found.txt ] || break

  added=0
  while read -r dep; do
    [ -n "$dep" ] || continue
    base=$(basename "$dep")
    if [ ! -f "$STAGE/$base" ]; then
      added=$((added + 1))
    fi
    cp -f "$ROOT/$dep" "$STAGE/" 2>/dev/null || true
  done < found.txt

  # Nothing new turned up, so the tree is fully covered.
  [ "$added" -eq 0 ] && break
done

rm -f found.txt

if [ -f cfspeed-ca.pem ]; then
  cp cfspeed-ca.pem "$STAGE/"
else
  echo "collect-dlls: cfspeed-ca.pem missing - run scripts/fetch-ca.sh first" >&2
  exit 1
fi

n=$(ls "$STAGE"/*.dll 2>/dev/null | wc -l)
echo "collected $n runtime DLLs"

# A package that cannot start must not be published, so this fails loudly
# rather than shipping something the user discovers is broken.
if [ ! -f "$STAGE/libcurl-4.dll" ]; then
  echo "collect-dlls: libcurl-4.dll missing from the package" >&2
  exit 1
fi
if [ "$n" -lt 10 ]; then
  echo "collect-dlls: only $n DLLs collected; the walk is probably shallow again" >&2
  exit 1
fi

exit 0