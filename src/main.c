/* cfspeed - a TUI speed test against Cloudflare's public endpoints.
 *
 * The loop draws only when something on screen actually changed. Painting
 * unconditionally costs a full repaint thirty times a second for as long as the
 * app is open, which measured at around 5% of a core sitting on a finished
 * result doing nothing at all. Diffing makes the idle cost zero.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "env_size.h"

#include "cli.h"
#include "engine.h"
#include "history.h"
#include "keymap.h"
#include "term.h"
#include "ui.h"

#define BASE_URL "https://speed.cloudflare.com"
#define FRAME_MS 33
#define IDLE_MS  250

/* Engine has no notion of suspending a phase mid-flight, so "pause" is a
 * cancel and "resume" is a restart of the same mode. That is honest about what
 * it is: the partial samples are kept, so the charts still show what was
 * measured before the stop. */
#if defined(_WIN32)
#  include <windows.h>
#  include <io.h>
#  include <fcntl.h>
#else
#  include <time.h>
#  include <unistd.h>
#endif

static void sleep_ms(int ms) {
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

/* Run cli_json straight into `path` by swapping stdout underneath it.
 *
 * Re-serialising the result a second time to write it would be simpler, but
 * then the file and the stream are produced by two code paths and will drift.
 * Redirecting means there is exactly one writer, so --out and stdout can never
 * disagree. */
static int json_to_file(const Engine *e, const char *path) {
#if defined(_WIN32)
    int saved = _dup(_fileno(stdout));
#else
    fflush(stdout);
    int saved = dup(fileno(stdout));
#endif
    if (saved < 0) return -1;

    FILE *f = fopen(path, "wb");
    if (!f) {
#if defined(_WIN32)
        _close(saved);
#else
        close(saved);
#endif
        return -1;
    }
#if defined(_WIN32)
    _dup2(_fileno(f), _fileno(stdout));
#else
    dup2(fileno(f), fileno(stdout));
#endif
    cli_json(e);

#if defined(_WIN32)
    _dup2(saved, _fileno(stdout));
    _close(saved);
    fclose(f);
#else
    fflush(stdout);
    dup2(saved, fileno(stdout));
    close(saved);
    fclose(f);
#endif
    return 0;
}

/* Run to completion with no terminal at all. This is the path --json uses, and
 * the only one that works in a CI log where there is no size to query. */
static int run_headless(Engine *e, const Options *o) {
    for (int i = 0; i < o->iterations; i++) {
        if (i > 0) engine_restart(e, o->mode);
        while (!engine_finished(e)) {
            engine_poll(e);
            sleep_ms(20);
        }
        if (!engine_result(e)->valid) {
            fprintf(stderr, "cfspeed: %s\n", engine_error(e));
            return 2;
        }
    }

    if (o->out[0] && json_to_file(e, o->out) != 0) {
        fprintf(stderr, "cfspeed: cannot write %s\n", o->out);
        return 2;
    }
    if (!o->quiet) cli_summary(e);
    if (o->json) {
        cli_json(e);
        fputc('\n', stdout);
    }
    return 0;
}

int main(int argc, char **argv) {
    Options opt;
    cli_parse(argc, argv, &opt);
    int rc = cli_validate(&opt);
    if (rc) return rc;

    if (net_init() != NET_OK) {
        fprintf(stderr, "cfspeed: could not initialise the HTTP client\n");
        return 1;
    }

    Engine *e = engine_new(BASE_URL, opt.mode);
    if (!e) {
        net_shutdown();
        return 1;
    }

    if (opt.headless) {
        int hrc = run_headless(e, &opt);
        engine_free(e);
        net_shutdown();
        return hrc;
    }

    if (term_init() != 0) {
        fprintf(stderr, "cfspeed: could not set up the terminal\n");
        engine_free(e);
        net_shutdown();
        return 1;
    }

    int w = 0, h = 0;
    /* Without a console - a pipe, a file, a CI log - there is no size to ask
     * for, and falling back to zero would clamp the screen to a single cell. */
    if (term_size(&w, &h) != 0 || w < MIN_SCREEN_W || h < MIN_SCREEN_H) {
        env_size(&w, &h);
        if (w < MIN_SCREEN_W) w = 120;
        if (h < MIN_SCREEN_H) h = 40;
    }
    Screen *scr = screen_create(w, h);
    if (!scr) {
        term_shutdown();
        engine_free(e);
        net_shutdown();
        return 1;
    }

    /* A History holds MAX_RUNS RunResults, each carrying its sample arrays -
     * around 4 MB. On the stack that is a guaranteed overflow. */
    History *hist = NULL;
    if (!opt.no_history) {
        hist = (History *)calloc(1, sizeof *hist);
        if (hist) history_load(hist);
    }

    int g_view = 0;
    int g_zoom = 1;
    int dirty = 1;
    int running = 1;
    /* engine_finished() stays true once a run ends and is only cleared by
     * engine_restart, so it cannot by itself mean "just finished". Without this
     * the history file is rewritten from end to end on every single frame that
     * follows a completed test - which measured as a pegged core and a hot disk
     * for as long as the app stayed open. */
    int recorded = 0;
    int paused = 0;
    uint64_t last_tick = now_ms();

    while (running) {
        if (dirty) {
            ui_draw(scr, e, hist, g_view, g_zoom);
            frame_present(scr);
            dirty = 0;
        }

        Event ev = term_poll();
        if (ev.type == EV_RESIZE) {
            if (term_size(&w, &h) != 0) continue;
            Screen *ns = screen_create(w, h);
            if (ns) {
                screen_free(scr);
                scr = ns;
            }
            frame_invalidate();
            dirty = 1;
            continue;
        }
        /* Resolved before the keymap: a click needs the screen size to know
         * where the tab strip is, and the strip is on the last row. */
        if (ev.type == EV_MOUSE && ev.mbtn == 0) {
            int tab = ui_hit_tab(ev.mx, ev.my, w, h);
            if (tab >= 0) {
                g_view = tab;
                dirty = 1;
                continue;
            }
        }
        if (ev.type == EV_KEY || ev.type == EV_MOUSE) {
            TermKey k = keymap_from_event(ev);
            switch (keymap_action(k, ev.mx, ev.my)) {
                case ACT_TAB_NEXT: g_view = (g_view + 1) % 5; dirty = 1; break;
                case ACT_TAB_PREV: g_view = (g_view + 4) % 5; dirty = 1; break;
                case ACT_TAB_1: g_view = 0; dirty = 1; break;
                case ACT_TAB_2: g_view = 1; dirty = 1; break;
                case ACT_TAB_3: g_view = 2; dirty = 1; break;
                case ACT_TAB_4: g_view = 3; dirty = 1; break;
                case ACT_TAB_5: case ACT_HELP: g_view = 4; dirty = 1; break;
                case ACT_ZOOM_IN:
                    if (g_zoom < 16) g_zoom *= 2;
                    dirty = 1;
                    break;
                case ACT_ZOOM_OUT:
                    if (g_zoom > 1) g_zoom /= 2;
                    dirty = 1;
                    break;
                case ACT_ZOOM_RESET: g_zoom = 1; dirty = 1; break;
                case ACT_PAUSE:
                    /* The engine cannot suspend a phase mid-flight, so this
                     * cancels the run and remembers that it did. Without the
                     * flag the key is a one-way door: the transfer stops but
                     * the engine stays in PH_DOWNLOAD reporting nothing until
                     * its deadline, and only 'r' gets out. */
                    if (paused) {
                        engine_restart(e, engine_mode(e));
                        paused = 0;
                        recorded = 0;
                    } else {
                        engine_cancel(e);
                        paused = 1;
                    }
                    dirty = 1;
                    break;
                case ACT_MODE_FULL:  engine_restart(e, MODE_FULL);     recorded = 0; paused = 0; dirty = 1; break;
                case ACT_MODE_PING:  engine_restart(e, MODE_LATENCY);  recorded = 0; paused = 0; dirty = 1; break;
                case ACT_MODE_DOWN:  engine_restart(e, MODE_DOWNLOAD); recorded = 0; paused = 0; dirty = 1; break;
                case ACT_MODE_UP:    engine_restart(e, MODE_UPLOAD);   recorded = 0; paused = 0; dirty = 1; break;
                case ACT_RERUN:
                    /* Restarting cancels the run in flight first, so a superseded
                     * test stops competing for bandwidth rather than interleaving
                     * its samples into the new one. */
                    engine_restart(e, engine_mode(e));
                    recorded = 0;
                    paused = 0;
                    dirty = 1;
                    break;
                case ACT_EXPORT: cli_json(e); break;
                case ACT_CLEAR_HISTORY:
                    if (hist) history_clear(hist);
                    ui_set_history_sel(0);
                    dirty = 1;
                    break;
                case ACT_QUIT: running = 0; break;
                default: break;
            }
            continue;
        }

        uint64_t t = now_ms();
        int live = engine_poll(e);
        if (live) dirty = 1;
        if (engine_finished(e) && !recorded) {
            recorded = 1;
            /* Recording on finish rather than on start means a cancelled run
             * is not written as though it completed. */
            if (hist && engine_result(e)->valid && !opt.no_history) {
                if (hist->count < MAX_RUNS)
                    hist->runs[hist->count++] = *engine_result(e);
                history_save(hist);
            }
            dirty = 1;
        }

        /* A test in flight animates, so tick fast. Once it finishes nothing on
         * screen moves, and waking thirty times a second to redraw an identical
         * screen is the cost this loop used to pay. */
        uint64_t period = live ? FRAME_MS : IDLE_MS;
        if (t - last_tick >= period) last_tick = t;
    }

    free(hist);
    screen_free(scr);
    engine_free(e);
    term_shutdown();
    net_shutdown();
    return 0;
}