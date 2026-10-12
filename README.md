# cfspeed (C)

A TUI speed test against Cloudflare's public endpoints, written in C.

Same measurement rules as the Rust original, same UI. The reason it exists is
binary size: the Rust build ships at ~5.5 MB and this one is a fraction of
that.

## Build

    make

Needs a C11 compiler and libcurl. That is the only dependency.

## Keys

| Key | Action |
|-----|--------|
| `1`–`5` | live / charts / details / history / help |
| `space` | pause the charts |
| `f` `p` `d` `u` | full / ping / download / upload only |
| `r` | restart the current test |
| `q` | quit |

## How the numbers are produced

The part that matters, and the thing that was wrong in an early version:

**Upload is counted only when the server answers 2xx.** The first implementation
streamed an endless request body and counted bytes as the HTTP client pulled
them off the socket. That measures bytes handed to the kernel, not bytes
delivered, and it reported momentary rates over 1 Gbps on a link that sustains
about 110 Mbps. A fixed 1 MiB body counted on a 2xx response is ground truth.

**A phase that moved no bytes is an error, not a 0 Mbps result.** Cloudflare
rate-limits by payload size and answers 429 under sustained testing. The
workers back off rather than fail, which is right mid-phase, so a run in which
every request was refused would otherwise finish as a confident zero.

**Download requests adapt.** They start at 1 MiB, halve when refused, and grow
back slowly when the endpoint is willing.

**Latency is measured in two windows.** Idle and under-load are kept apart;
mixing them would corrupt both the baseline and the bufferbloat figure.

## Rendering

The renderer keeps a front buffer and only writes cells that changed. With
nothing new to draw the frame is skipped entirely and no bytes go out, which is
what makes the idle cost zero rather than a few percent of a core.
