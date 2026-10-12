/* cfspeed - a TUI speed test against Cloudflare's public endpoints.
 *
 * The loop draws only when something on screen actually changed. Painting
 * unconditionally costs a full repaint thirty times a second for as long as the
 * app is open, which measured at around 5% of a core sitting on a finished
 * result doing nothing at all. Ratatui-free diffing makes the idle cost zero.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "env_size.h"

#include "engine.h"
#include "term.h"
#include "ui.h"

#define BASE_URL "https://speed.cloudflare.com"
#define FRAME_MS 33
#define IDLE_MS  250

static int g_view = 0;

static void view_next(int d) {
    g_view = (g_view + d + 5) % 5;
}

int main(int argc, char **argv) {
    Mode mode = MODE_FULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ping") == 0 || strcmp(argv[i], "-p") == 0) mode = MODE_LATENCY;
        else if (strcmp(argv[i], "--download") == 0 || strcmp(argv[i], "-d") == 0) mode = MODE_DOWNLOAD;
        else if (strcmp(argv[i], "--upload") == 0 || strcmp(argv[i], "-u") == 0) mode = MODE_UPLOAD;
    }

    if (net_init() != NET_OK) {
        fprintf(stderr, "cfspeed: could not initialise the HTTP client\n");
        return 1;
    }
    if (term_init() != 0) {
        fprintf(stderr, "cfspeed: could not set up the terminal\n");
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
    Engine *e = engine_new(BASE_URL, mode);
    if (!scr || !e) {
        term_shutdown();
        net_shutdown();
        return 1;
    }

    int dirty = 1;
    int running = 1;
    uint64_t last_tick = now_ms();

    while (running) {
        if (dirty) {
            ui_draw(scr, e, g_view);
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
        if (ev.type == EV_KEY) {
            if (ev.key == KEY_CHAR) {
                switch (ev.ch) {
                    case 'q': running = 0; break;
                    case '1': g_view = 0; dirty = 1; break;
                    case '2': g_view = 1; dirty = 1; break;
                    case '3': g_view = 2; dirty = 1; break;
                    case '4': g_view = 3; dirty = 1; break;
                    case '5': case '?': g_view = 4; dirty = 1; break;
                    case 'f': engine_restart(e, MODE_FULL); dirty = 1; break;
                    case 'p': engine_restart(e, MODE_LATENCY); dirty = 1; break;
                    case 'd': engine_restart(e, MODE_DOWNLOAD); dirty = 1; break;
                    case 'u': engine_restart(e, MODE_UPLOAD); dirty = 1; break;
                    case 'r':
                        /* Restarting cancels the run in flight first, so a
                         * superseded test stops competing for bandwidth rather
                         * than interleaving its samples into the new one. */
                        engine_restart(e, engine_mode(e));
                        dirty = 1;
                        break;
                    default: break;
                }
            } else if (ev.key == KEY_TAB) {
                view_next(ev.mods & 1 ? -1 : 1);
                dirty = 1;
            } else if (ev.key == KEY_ESC) {
                running = 0;
            }
            continue;
        }

        uint64_t t = now_ms();
        int live = engine_poll(e);
        if (live) dirty = 1;
        if (engine_finished(e)) dirty = 1;

        /* A test in flight animates, so tick fast. Once it finishes nothing on
         * screen moves, and waking thirty times a second to redraw an identical
         * screen is the cost this loop used to pay. */
        uint64_t period = live ? FRAME_MS : IDLE_MS;
        if (t - last_tick >= period) last_tick = t;
    }

    screen_free(scr);
    engine_free(e);
    term_shutdown();
    net_shutdown();
    return 0;
}