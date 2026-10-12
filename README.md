# cfspeed (C)

A terminal speed test against Cloudflare's public endpoints
(`https://speed.cloudflare.com`), written in C. This is a rewrite of the Rust
project at [`msedge-projects/cfspeed`](https://github.com/msedge-projects/cfspeed);
**it is not feature-complete and does not yet reach parity with the Rust
version.** See [Status](#status) before you rely on it.

## Status

This is a work in progress. What is true today:

**Works**

- The **LIVE** view: header, route strip (your IP, ASN, Cloudflare colo, the
  edge IPv4/IPv6 addresses it resolved), paired download/upload bar charts, and
  the summary tiles (idle latency, loaded latency, jitter, loss, min, bufferbloat
  grade, stability grade, DNS, TLS).
- All four test modes: full, latency-only, download-only, upload-only
  (`MODE_FULL` / `MODE_LATENCY` / `MODE_DOWNLOAD` / `MODE_UPLOAD`).
- Restart, mode switching, tab navigation, quit.
- Terminal resize handling, plus a size fallback so it does not collapse to one
  cell when there is no console (a pipe, a file, a CI log).
- Differential rendering — only cells that actually changed are written, so an
  idle screen costs nothing.

**Does not work yet**

- **The CHARTS / DETAILS / HISTORY / HELP tabs are not implemented.** The tab
  bar renders and you can select a tab, but `ui_draw()` in `src/ui.c` ignores
  its `tab` argument except to move the highlight. All five tabs currently
  render the same LIVE content. `src/views.c` and `src/history.c` implement
  these views but are not wired in.
- **Only three command-line flags are actually parsed** — `--ping`/`-p`,
  `--download`/`-d`, `--upload`/`-u`. They are matched inline in `src/main.c`.
- **`--json`, `--quiet`, `--iterations`, `--duration`, `--out`,
  `--no-history`, `--version` and `--help` do nothing.** `src/cli.c` implements
  all of them, but `main.c` never calls `cli_parse()`. There is no headless or
  JSON mode, so this is **not yet usable in CI or a pipeline**.
- `src/cli.c`, `src/views.c` and `src/history.c` are not in the `Makefile`'s
  `SRC` list, so they are not compiled at all.
- The footer hint line advertises `space` (pause), `+`/`-` (zoom) and `m`
  (actions). None of those are handled yet. `Tab`, `1`-`5` and `Esc` work but
  are not listed there.

Keys are currently handled by a `switch` in `src/main.c`. A richer keymap layer
(`src/keymap.h`, plus `actions.h`, `fmt.h` and a small test harness in
`test.h`) has landed alongside the other unwired modules, but nothing includes
it yet, so the table below is the one that actually works.

## Installing from a GitHub Actions artifact

Builds happen in CI. There is no supported local build path, and nothing is
published to a package manager.

**Release artifacts** (built on a `v*` tag, fully packaged):

| Platform | Artifact name |
|---|---|
| Linux x86_64 | `release-linux-x86_64` |
| macOS aarch64 | `release-macos-aarch64` |
| Windows x86_64 | `release-windows-x86_64` |

```sh
# list the runs, find the id for the v* tag you want
gh run list --workflow release.yml

gh run download <run-id> -n release-linux-x86_64 -D ./cfspeed
tar -xzf ./cfspeed/cfspeed-linux-x86_64.tar.gz
./cfspeed-linux-x86_64
```

On Windows the artifact is a zip; the DLLs sit next to `cfspeed.exe` inside it
and must stay there.

**CI artifacts** (every push to `main` and every PR) are named
`cfspeed-<os>`, e.g. `cfspeed-windows-latest`:

```sh
gh run download <run-id> -n cfspeed-ubuntu-latest -D ./out
```

These are the bare binary only. The Windows one **will not start** without the
DLLs from a release artifact — prefer the release artifact on Windows.

## Running it

```sh
cfspeed                 # full test, TUI
cfspeed --ping          # latency only, skips the transfer phases
cfspeed --download      # download only
cfspeed --upload        # upload only
```

Anything else you pass is currently ignored.

Set `CFS_DEBUG=1` to trace every HTTP request to stderr (URL, curl return code,
HTTP status, byte counter, chunk size). Off by default — it is a TUI and a wall
of curl messages would wreck it.

### What a run looks like

Illustrative layout only — these are the real labels, but the values shown are
placeholders, not a captured run:

```
 cfspeed · FRA · DE                                     0:22  COMPLETE
────────────────────────────────────────────────────────────────────────
  YOU   203.0.113.4   Example ISP · AS64496 · DE    ┃ EDGE  FRA   198.51.100.7 …
────────────────────────────────────────────────────────────────────────
 ↓ DOWNLOAD                     ↑ UPLOAD
 …                             …
  ████████████████████         █████████████
 …                             …
 avg …  p95 …  max …            avg …  p95 …  max …
────────────────────────────────────────────────────────────────────────
 IDLE …ms  LOADED …ms  JITTER …ms  LOSS 0.0%  MIN …ms  BLOAT A +…ms  …
────────────────────────────────────────────────────────────────────────
 LIVE  CHARTS  DETAILS  HISTORY  HELP                  f test  p ping  …
```

A run is roughly 2 s of idle latency, 10 s of download and 10 s of upload.

One message you may genuinely see, quoted verbatim from `src/engine.c`:

```
Download transferred no data - Cloudflare refused every request.
This is rate limiting from repeated testing, not a speed result.
```

## Windows prerequisites

**This is not a standalone Windows build, and it is not the small one.**

- The executable is about **72 KB**. It links libcurl dynamically, so it needs
  `libcurl-4.dll` and that DLL's own dependency chain sitting in the same
  directory — **roughly 31 MB** in total. The release workflow copies them
  beside the exe for you; do not move the exe away from them.
- You also need a **CA bundle named `cfspeed-ca.pem` next to `cfspeed.exe`**.
  A Windows curl built against GnuTLS or OpenSSL has no store of its own and
  every request fails with `CURLE_PEER_FAILED_VERIFICATION` (curl error 60)
  without one. `src/net.c` looks for that exact filename next to the
  executable. Verification is never disabled — that would be a security
  regression on a tool whose entire job is trusting a remote endpoint.
  **Note: the release workflow packages the DLLs but does not fetch or ship the
  `.pem`, so a release artifact will measure nothing until you add it yourself:**

  ```powershell
  # fetches https://curl.se/ca/cacert.pem and validates it before installing
  .\scripts\fetch-ca.ps1 -OutPath .\cfspeed-ca.pem
  ```

  The script fails hard on a truncated download, because a zero-byte CA file is
  worse than a missing one: it looks present, so the failure goes silent instead
  of loud. `scripts/fetch-ca.sh` is the equivalent for the Linux and macOS legs.
- On Linux and macOS the system CA store is found automatically; there are three
  common paths checked in order before falling back to the libcurl build default.

So the honest comparison, since binary size was the original motivation for the
rewrite:

| | Rust original | This C port |
|---|---|---|
| Executable | ~5.25 MB, single file | ~72 KB |
| Total shipped | ~5.25 MB | **~31 MB** (exe + DLL chain) |

It is much faster to compile and far simpler to read. It is **larger** to ship.

## Keys

| Key | Action |
|---|---|
| `1`–`5`, `?` | select tab (only LIVE has content so far) |
| `Tab` / `Shift+Tab` | next / previous tab |
| `f` `p` `d` `u` | run full / latency / download / upload |
| `r` | restart the current test |
| `q`, `Esc` | quit |

These are the keys handled by `src/main.c` today. `src/keymap.h` defines a
richer intended set — pause, zoom, menu, export, mouse — but it is not wired in.

Restarting cancels the run in flight first, so a superseded test stops competing
for bandwidth instead of interleaving its samples into the new one.

## How the numbers are produced

The part that matters, and the thing that was wrong in an early version:

**Upload is counted only when the server answers 2xx.** The first implementation
streamed an endless request body and counted bytes as the HTTP client pulled
them off the socket. That measures bytes handed to the kernel, not bytes
delivered — it reported momentary rates over 1 Gbps on a link that sustains
about 130 Mbps. A fixed 1 MiB body counted on a 2xx response is ground truth.

**A phase that moved no bytes is an error, not a 0 Mbps result.** Cloudflare
rate-limits by payload size and answers HTTP 429 under sustained testing. The
workers treat a non-2xx as a reason to back off rather than an error, which is
right mid-phase, so a run in which every request was refused would otherwise
finish as a confident zero. `finish()` in `src/engine.c` catches that case and
fails the run explicitly.

**Download requests adapt.** They start at 1 MB, halve on refusal down to a
100 KB floor, and grow 50% back toward a 4 MB ceiling after eight consecutive
successes — shrinking fast when refused, growing back slowly when willing.

**Latency is measured in two windows.** Idle and under-load are kept apart
(`idle` and `loaded_down`); mixing them would corrupt both the baseline and the
bufferbloat figure.

**Rendering is diffed.** The renderer keeps a front buffer and only writes cells
that changed. With nothing new to draw the frame is skipped and no bytes go out,
which is what makes the idle cost zero rather than a few percent of a core. The
main loop wakes at 33 ms while a test is animating and 250 ms once it is done.

## Architecture

| File | Owns |
|---|---|
| `src/main.c` | the three real flags, the frame loop, key handling, tab index |
| `src/engine.c/.h` | measurement state machine, phase sequencing, result assembly |
| `src/net.c/.h` | all HTTP, behind a transport seam — meta/DNS/TLS, latency probe, download & upload worker threads, CA bundle lookup |
| `src/ui.c/.h` | the LIVE view: header, route strip, charts, tiles, tab bar |
| `src/term.c/.h` | terminal setup, input decoding, front buffer, frame diff and present |
| `src/theme.h` | colours and the grade→colour mapping |
| `src/stats.h`, `src/stats.c` | rate/latency statistics helpers |
| `src/env_size.h` | minimum screen size and the no-console size fallback |
| `src/cli.c/.h` | *not compiled, not called* — the full flag set, JSON and summary output |
| `src/views.c/.h` | *not compiled, not called* — CHARTS / DETAILS / HISTORY / HELP |
| `src/history.c/.h` | *not compiled, not called* — run history persistence |
| `src/keymap.c/.h` | *not compiled, not called* — key→action table for the HELP tab |
| `src/actions.c/.h` | *not compiled, not called* — the action/menu layer |
| `src/fmt.c/.h`, `src/test.c/.h` | *not compiled* — formatting helpers, test harness |
| `scripts/fetch-ca.ps1`, `scripts/fetch-ca.sh` | download and validate a CA bundle (not yet called by CI) |
| `scripts/smoke.sh` | behaviour gate: runs the binary and fails if it measures nothing |

The engine runs as a state machine stepped from the UI's frame loop, so there is
no second thread that can race the renderer. Only the per-connection workers are
threads, and each does exactly one thing: accumulate a byte counter.

## Contributing

[`msedge-projects/cfspeed-c`](https://github.com/msedge-projects/cfspeed-c).
CI builds on Linux, macOS and Windows (MSYS2/MINGW64) for every push and PR.

The single most useful thing you can do is help finish the wiring: call
`cli_parse()` from `main.c` and add `cli.c` to `SRC` in the Makefile to get the
headless/JSON mode, and add `views.c` + `history.c` and dispatch on the `tab`
argument in `ui_draw()` to get the other four tabs.

Please do not document features as working before they are — a README that
overstates the state of the port costs more than a short one.

## Licence

MIT. See [LICENSE](LICENSE).
