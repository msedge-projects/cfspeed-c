#!/usr/bin/env bash
#
# WHY this script exists
# ----------------------
# "make" exiting 0 proves the code compiles. It proves nothing about whether the
# thing measures anything. The GnuTLS-on-Windows trust-store problem is exactly
# the failure mode that compiles cleanly and then returns
# CURLE_PEER_FAILED_VERIFICATION (curl error 60) on every request: a perfect
# binary that reports no speed, no colo, no error worth reading.
#
# So this is a gate on behaviour, not on compilation. It runs the built binary,
# parses its output with a real JSON parser, and fails when the numbers are
# missing or hollow. A binary that runs but measures nothing must NOT pass.
#
# Grepping the output for a substring would defeat the purpose: malformed JSON
# can contain any substring you like, and the whole value of this gate is that
# the document parses.

set -euo pipefail

log()  { printf '%s\n' "$*" >&2; }
fail() { printf 'smoke: FAIL: %s\n' "$*" >&2; exit 1; }

# --- locate the binary --------------------------------------------------------

bin="${1:-}"
if [ -z "$bin" ]; then
    for cand in ./cfspeed ./cfspeed.exe cfspeed cfspeed.exe; do
        if [ -f "$cand" ]; then
            bin="$cand"
            break
        fi
    done
fi
[ -n "$bin" ] || fail 'no binary given, and neither ./cfspeed nor ./cfspeed.exe exists'
[ -f "$bin" ] || fail "no such binary: $bin"

# --- locate a real python -----------------------------------------------------

# `command -v python3` alone is not proof on Windows: the Microsoft Store
# install puts a python3.exe execution alias on PATH that prints "Python was not
# found" and exits non-zero. So the candidate has to actually run.
pick_python() {
    for cand in python3 python; do
        if command -v "$cand" >/dev/null 2>&1 && "$cand" -c '' >/dev/null 2>&1; then
            printf '%s\n' "$cand"
            return 0
        fi
    done
    return 1
}

py="$(pick_python)" || fail 'neither python3 nor python is available to parse the JSON output'

# --- run ----------------------------------------------------------------------

work="$(mktemp -d "${TMPDIR:-/tmp}/cfspeed-smoke.XXXXXX")" || fail 'cannot create a temp dir'
trap 'rm -rf "$work"' EXIT HUP INT TERM

stdout_file="$work/stdout.json"
stderr_file="$work/stderr.txt"

log "smoke: running $bin --json with $py for parsing"
rc=0
"$bin" --json >"$stdout_file" 2>"$stderr_file" || rc=$?

if [ "$rc" -ne 0 ]; then
    log '--- stderr ---'
    sed 's/^/  /' "$stderr_file" >&2 || true
    log '--- stdout ---'
    sed 's/^/  /' "$stdout_file" >&2 || true
    fail "$bin --json exited $rc (expected 0)"
fi

if [ ! -s "$stdout_file" ]; then
    log '--- stderr ---'
    sed 's/^/  /' "$stderr_file" >&2 || true
    fail "$bin --json wrote nothing to stdout"
fi

# --- validate the document ----------------------------------------------------

"$py" - "$stdout_file" "$bin" <<'PY'
import json
import sys

path, binary = sys.argv[1], sys.argv[2]


def die(message):
    sys.stderr.write("smoke: FAIL: %s\n" % message)
    raise SystemExit(1)


try:
    with open(path, "r", encoding="utf-8", errors="strict") as handle:
        raw = handle.read()
except UnicodeDecodeError:
    die("stdout is not valid UTF-8 text")
except OSError as exc:
    die("cannot read captured stdout (%s)" % exc)

if not raw.strip():
    die("%s --json produced no output at all" % binary)

# json.loads rejects trailing content, so two concatenated objects or a pretty
# print followed by a log line fail here instead of passing as "one object".
try:
    doc = json.loads(raw)
except ValueError as exc:
    snippet = raw[:200].replace("\n", "\\n")
    die("stdout is not a single valid JSON object (%s)\n         got: %s" % (exc, snippet))

if not isinstance(doc, dict):
    die("expected a JSON object at the top level, got %s" % type(doc).__name__)

download = doc.get("download")
if not isinstance(download, dict):
    die("no \"download\" object in the output - the binary ran but measured nothing")

if "avg" not in download:
    die("the \"download\" object has no \"avg\" field")

avg = download["avg"]
if isinstance(avg, bool) or not isinstance(avg, (int, float)):
    die("download.avg is not a number: %r" % (avg,))

if avg <= 0:
    die(
        "download.avg is %r - the run produced no throughput, which is what a "
        "trust-store failure looks like from here" % (avg,)
    )

# colo lives under meta in src/cli.c's emit_meta; the top-level fallback keeps
# this gate honest if the schema ever flattens.
meta = doc.get("meta")
colo = meta.get("colo") if isinstance(meta, dict) else None
if colo is None:
    colo = doc.get("colo")

if not isinstance(colo, str) or not colo.strip():
    die("no non-empty \"colo\" in the output - the metadata lookup never resolved")

if doc.get("error"):
    sys.stderr.write("smoke: note: the document carries an error field: %r\n" % (doc["error"],))

sys.stdout.write(
    "smoke: ok - valid JSON object, download.avg=%.2f B/s, colo=%s\n" % (avg, colo)
)
PY