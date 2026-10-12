# cfspeed (C)

A terminal speed test for Cloudflare's public speed endpoints, written in C.

Measures latency, download and upload; reports jitter, packet loss,
bufferbloat and a stability grade; renders five clickable tabs; and can emit a
single JSON object for scripting.

This is a C rewrite of [`msedge-projects/cfspeed`](https://github.com/msedge-projects/cfspeed),
which remains the more mature implementation. See [Status](#status).

## Status

**It works.** The released binary has been downloaded, checksum-verified,
extracted into an empty directory with no toolchain on `PATH`, and run.

Verified from the published `v0.1.0` Windows artifact:

```
colo        MAA
client IP   2001:4490:4e71:49eb:79f9:a61:568b:22fb
location    IN
DOWNLOAD    106.5 Mbps avg, p95 111.3
UPLOAD      113.2 Mbps avg, p95 196.4
idle        117.5 ms, loss 0%
grades      stability D, bufferbloat B
```

Those figures match an external measurement of the same link taken minutes
apart, which is the check that matters — a speed test that disagrees with
reality is worse than no speed test.

**Working**

- Live download and upload charts, drawn as bars and redrawn by cell diff
- Latency, jitter, packet loss, bufferbloat and stability grades
- Five tabs — LIVE, CHARTS, DETAILS, HISTORY, HELP — switchable by key **or by
  clicking the tab strip**
- Run history persisted to disk and restored on next launch
- Headless `--json` output, verified against a real JSON parser
- The client IP, the edge colo and the resolved edge addresses shown together
  on the main view, so it is always clear which is which
- Mouse click support, with the scroll wheel correctly distinguished from a click
- Uploads counted only on a 2xx response; a phase that moved no bytes is
  reported as an error rather than as 0 Mbps
- Transparent background — no cell is ever painted with an opaque colour except
  the one selected row, so a terminal theme shows through
- 49 unit tests covering `stats`, `chart`, `fmt` and `actions`, gating CI

**Known gaps**

- The `m` actions menu has a tested model but no renderer yet, so `m` does
  nothing.
- `--duration` is accepted but not yet applied to the engine.
- Only the Windows artifact has been executed. Linux and macOS are built and
  their artifacts are published, but they have not been run on a real network.
- The man page has been validated structurally but never rendered; there was no
  `groff` available to check it with.

## Install

Grab the archive for your platform from
[the releases page](https://github.com/msedge-projects/cfspeed-c/releases).

**Windows** — unzip anywhere and run `cfspeed.exe`. The archive is
self-contained: it carries the libcurl runtime and a CA bundle. Nothing needs
installing and nothing needs to be on your `PATH`.

**Linux / macOS** — untar and run the binary.

Verify what you downloaded:

```
sha256sum -c checksums.txt
```

## Why Windows ships a CA bundle

Windows keeps its trust store in a registry database. The libcurl that MSYS2
provides is backed by GnuTLS, which reads a trust list from a *file* and cannot
see that database. Without a bundle beside the binary, every HTTPS request
fails certificate verification and the app measures nothing at all.

The release workflow fetches the bundle and ships it. On Linux and macOS the
system store is found automatically and no bundle is needed.

Turning verification off instead would be a security regression on a tool whose
entire job is trusting a remote endpoint, so it is never an option.

## Usage

```
cfspeed                 run a full test in the TUI
cfspeed --ping          latency only
cfspeed --download      download only
cfspeed --upload        upload only

cfspeed --json          one JSON object on stdout, then exit
cfspeed --out r.json    also write that JSON to a file
cfspeed --quiet         no human-readable summary
cfspeed --iterations 3  run three times
cfspeed --duration 30   override the phase duration
cfspeed --no-history    do not record this run

cfspeed --help
```

Both `--flag value` and `--flag=value` are accepted.

### Keys

| Key | Action |
|---|---|
| `1`–`5`, `Tab`, `Shift+Tab` | switch tab |
| click the tab strip | switch tab |
| `space` | pause / resume the run |
| `+` `-` `=` | zoom the time axis / reset |
| `f` `p` `d` `u` | full / ping / download / upload test |
| `r` | rerun |
| `e` | export the current result as JSON |
| `x` | clear history |
| `?` `H` | help |
| `q`, `Ctrl+C` | quit |

## Size

The premise for rewriting this in C was to shrink it. Measured, that did not
quite hold on Windows, and it is worth being straight about:

| | Rust | C |
|---|---|---|
| Executable | 5.25 MB, single file | 152 KB |
| Total shipped | 5.25 MB | **6.49 MB** (exe + 17 DLLs + CA bundle) |
| Idle CPU | 0.00% | 0.00% |
| Peak RAM | 4.6 MB | ~0.1 MB |

The C build is far smaller in memory and dramatically smaller on Linux and
macOS, where no DLL chain has to ship. On Windows it comes out marginally
larger than the self-contained Rust binary, because libcurl on Windows pulls in
a GnuTLS stack of roughly 6 MB. Linking a schannel-backed or mbedTLS libcurl
would change that.

## Architecture

| File | Owns |
|---|---|
| `src/main.c` | event loop, key dispatch, history recording |
| `src/engine.c/.h` | phase state machine, sampling, statistics |
| `src/net.c/.h` | the only file that talks to libcurl |
| `src/term.c/.h` | raw mode, cell buffer, diff renderer, input decoding |
| `src/ui.c/.h` | the LIVE tab and the tab dispatcher |
| `src/views.c/.h` | CHARTS / DETAILS / HISTORY / HELP |
| `src/history.c/.h` | run history persistence |
| `src/cli.c/.h` | flag parsing, JSON and summary output |
| `src/keymap.c/.h` | the key-to-action table |
| `src/actions.c/.h` | the actions menu model |
| `src/stats.c/.h` | percentiles, jitter, CV, grades |
| `src/chart.c/.h` | chart scaling and bucketing |
| `src/fmt.c/.h` | number, rate and duration formatting |
| `src/test.c/.h` | the test harness |

The engine never talks to libcurl directly. Everything network-facing goes
through `net.c`, which is what makes the measurement logic testable and keeps
the TLS and threading concerns in one place.

## Building

Everything is built by GitHub Actions; there is no supported local build.
Grab an artifact from the [CI runs](https://github.com/msedge-projects/cfspeed-c/actions)
with:

```
gh run download <run-id> --repo msedge-projects/cfspeed-c --name cfspeed-windows-latest
```

Requires a C11 compiler and libcurl headers. On Windows that means MSYS2 with
the `MINGW64` subsystem.

## Licence

See [LICENSE](LICENSE).