/* main.c - argument handling, the SDL window, and the event loop.
 *
 * SDL appears nowhere else in the program: everything the player does is
 * composited into one system-memory buffer here and handed to SDL as a single
 * streaming texture, and SDL events are translated into the library-agnostic
 * calls in app.h.
 */
#include <SDL.h>
#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "cache.h"
#include "color.h"
#include "draw.h"
#include "font.h"
#include "platform.h"
#include "reader.h"
#include "sequence.h"
#include "ui.h"
#include "util.h"

#define DEFAULT_MEM_LIMIT ((size_t)1024 * 1024 * 1024) /* 1 GB */
#define DEFAULT_FPS       30.0

static SDL_Window   *g_window;
static SDL_Renderer *g_renderer;
static SDL_Texture  *g_texture;
static Surface       g_surface;
static int           g_tex_w, g_tex_h;

static Uint32     g_wake_event = (Uint32)-1;
static atomic_int g_wake_pending;

/* Called from loader threads. Coalesces wake-ups so a burst of completing
 * frames cannot flood the event queue. */
static void wake_main_thread(void *ud)
{
    (void)ud;
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_wake_pending, &expected, 1)) return;
    if (g_wake_event == (Uint32)-1) return;

    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = g_wake_event;
    SDL_PushEvent(&e);
}

/* ---- presentation ------------------------------------------------------- */

static int ensure_surface(int w, int h)
{
    if (w <= 0 || h <= 0) return 0;
    if (g_texture && g_tex_w == w && g_tex_h == h) return 1;

    if (g_texture) SDL_DestroyTexture(g_texture);
    g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!g_texture) {
        rp_log("cannot create presentation texture: %s", SDL_GetError());
        return 0;
    }

    free(g_surface.px);
    g_surface.px = malloc((size_t)w * (size_t)h * sizeof(uint32_t));
    if (!g_surface.px) {
        rp_log("out of memory for the %dx%d window buffer", w, h);
        g_surface.w = g_surface.h = 0;
        return 0;
    }
    g_surface.w = w;
    g_surface.h = h;
    g_tex_w = w;
    g_tex_h = h;
    return 1;
}

static void present(App *app)
{
    int ow = 0, oh = 0;
    SDL_GetRendererOutputSize(g_renderer, &ow, &oh);
    if (!ensure_surface(ow, oh)) return;

    if (app->layout.window.w != ow || app->layout.window.h != oh)
        app_resize(app, ow, oh);

    ui_draw(&g_surface, app);

    SDL_UpdateTexture(g_texture, NULL, g_surface.px, g_surface.w * (int)sizeof(uint32_t));
    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, NULL, NULL);
    SDL_RenderPresent(g_renderer);
}

/* ---- event translation --------------------------------------------------- */

static AppKey translate_key(const SDL_KeyboardEvent *k)
{
    int shift = (k->keysym.mod & KMOD_SHIFT) != 0;
    switch (k->keysym.sym) {
    case SDLK_ESCAPE: case SDLK_q:      return KEY_QUIT;
    case SDLK_SPACE:                    return KEY_SPACE;
    case SDLK_j:                        return KEY_PLAY_REV;
    case SDLK_k:                        return KEY_PAUSE;
    case SDLK_l:                        return KEY_PLAY_FWD;
    case SDLK_i:                        return shift ? KEY_STEP5_BACK : KEY_LEFT;
    case SDLK_o:                        return shift ? KEY_STEP5_FWD : KEY_RIGHT;
    case SDLK_LEFT:                     return KEY_LEFT;
    case SDLK_RIGHT:                    return KEY_RIGHT;
    case SDLK_DOWN: case SDLK_PAGEUP:   return KEY_PAGE_BACK;
    case SDLK_UP:   case SDLK_PAGEDOWN: return KEY_PAGE_FWD;
    case SDLK_HOME:                     return KEY_HOME;
    case SDLK_END:                      return KEY_END;
    case SDLK_BACKSPACE:                return KEY_FIT;
    case SDLK_0: case SDLK_KP_0:        return KEY_ONE_TO_ONE;
    case SDLK_f:                        return KEY_GUIDE;
    case SDLK_s:                        return KEY_FILTER;
    case SDLK_h: case SDLK_F1: case SDLK_SLASH: return KEY_HELP;
    default:                            return KEY_NONE;
    }
}

/* Only stepping keys make sense to auto-repeat; repeating a toggle would make
 * it flutter while the key is held. */
static int key_repeats(AppKey k)
{
    return k == KEY_LEFT || k == KEY_RIGHT || k == KEY_STEP5_BACK || k == KEY_STEP5_FWD ||
           k == KEY_PAGE_BACK || k == KEY_PAGE_FWD;
}

/* SDL reports mouse positions in window coordinates; our buffer is in real
 * pixels, which differ on a HiDPI display. */
static void window_to_pixels(int *x, int *y)
{
    int ww = 0, wh = 0, ow = 0, oh = 0;
    SDL_GetWindowSize(g_window, &ww, &wh);
    SDL_GetRendererOutputSize(g_renderer, &ow, &oh);
    if (ww > 0 && ow > 0) *x = (int)((double)*x * ow / ww);
    if (wh > 0 && oh > 0) *y = (int)((double)*y * oh / wh);
}

static void handle_event(App *app, const SDL_Event *e)
{
    if (e->type == g_wake_event) {
        atomic_store(&g_wake_pending, 0);
        app->need_redraw = 1;
        return;
    }

    switch (e->type) {
    case SDL_QUIT:
        app->quit = 1;
        break;

    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
            e->window.event == SDL_WINDOWEVENT_EXPOSED ||
            e->window.event == SDL_WINDOWEVENT_RESTORED)
            app->need_redraw = 1;
        break;

    case SDL_KEYDOWN: {
        AppKey k = translate_key(&e->key);
        if (k == KEY_NONE) break;
        if (e->key.repeat && !key_repeats(k)) break;
        app_key(app, k);
        break;
    }

    case SDL_MOUSEBUTTONDOWN: {
        int x = e->button.x, y = e->button.y;
        window_to_pixels(&x, &y);
        app_mouse_down(app, x, y, e->button.button);
        break;
    }

    case SDL_MOUSEBUTTONUP: {
        int x = e->button.x, y = e->button.y;
        window_to_pixels(&x, &y);
        app_mouse_up(app, x, y, e->button.button);
        break;
    }

    case SDL_MOUSEMOTION: {
        int x = e->motion.x, y = e->motion.y;
        window_to_pixels(&x, &y);
        app_mouse_move(app, x, y, (int)(e->motion.state & (SDL_BUTTON_LMASK | SDL_BUTTON_MMASK)));
        break;
    }

    case SDL_MOUSEWHEEL: {
        /* The wheel event carries no position on every SDL2 we build
         * against, so ask for the pointer; it is where the wheel turned. */
        int notches = e->wheel.y;
        if (e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) notches = -notches;
        int x = 0, y = 0;
        SDL_GetMouseState(&x, &y);
        window_to_pixels(&x, &y);
        app_wheel(app, x, y, notches);
        break;
    }

    default:
        break;
    }
}

/* ---- command line -------------------------------------------------------- */

static void usage(FILE *out)
{
    fprintf(out,
        "ramplayer - load image sequences into RAM and scrub through them\n"
        "\n"
        "usage: ramplayer [options] <frame|pattern|directory> [more frames...]\n"
        "\n"
        "  A single frame of a sequence expands to the whole sequence, so\n"
        "  'ramplayer shot.0042.exr' plays every shot.####.exr beside it.\n"
        "  Patterns (shot.%%04d.exr, shot.####.exr) and directories work too.\n"
        "\n"
        "options:\n"
        "  --mem SIZE      RAM budget for cached frames (default 1G)\n"
        "  --fps RATE      playback rate (default 30)\n"
        "  --threads N     loader threads (default: one per core, less one)\n"
        "  --readers N     read whole files, at most N at a time; 1 keeps a\n"
        "                  spinning disk streaming (default 0: decode straight\n"
        "                  from the file, which suits SSDs and fast networks)\n"
        "  --scale N       UI scale factor for HiDPI displays\n"
        "  -h, --help      this message\n"
        "\n"
        "keys:\n"
        "  space play/pause      j/k/l play backwards / pause / play forwards\n"
        "  i/o step (shift: 5)   left/right step   home/end ends\n"
        "  wheel zoom  drag pan  backspace fit  0 actual size  f 1920x1080 guide\n"
        "  s smooth scaling      h help   q quit\n");
}

typedef struct {
    size_t mem_limit;
    double fps;
    int    fps_set; /* --fps given, so do not let the file override it */
    int    threads;
    int    readers;
    int    scale;
    char *const *inputs;
    int    n_inputs;
} Options;

static int parse_args(int argc, char **argv, Options *o)
{
    o->mem_limit = DEFAULT_MEM_LIMIT;
    o->fps       = DEFAULT_FPS;
    o->fps_set   = 0;
    o->threads   = 0;
    o->readers   = 0;
    o->scale     = 0;

    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;

        if (strcmp(a, "--") == 0) { i++; break; }
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout);
            rp_console_prompt();
            exit(0);
        }

        if (i + 1 >= argc) {
            rp_log("option %s needs a value", a);
            return 0;
        }
        const char *v = argv[++i];

        if (strcmp(a, "--mem") == 0) {
            uint64_t bytes;
            if (!rp_parse_bytes(v, &bytes) || bytes < 1024 * 1024) {
                rp_log("bad --mem value '%s' (try 1G, 512M, ...)", v);
                return 0;
            }
            o->mem_limit = (size_t)bytes;
        } else if (strcmp(a, "--fps") == 0) {
            o->fps = atof(v);
            if (!(o->fps > 0.0) || o->fps > 1000.0) {
                rp_log("bad --fps value '%s'", v);
                return 0;
            }
            o->fps_set = 1;
        } else if (strcmp(a, "--threads") == 0) {
            o->threads = atoi(v);
            if (o->threads < 1 || o->threads > 64) {
                rp_log("bad --threads value '%s'", v);
                return 0;
            }
        } else if (strcmp(a, "--readers") == 0) {
            o->readers = atoi(v);
            if (!isdigit((unsigned char)v[0]) || o->readers < 0 || o->readers > 64) {
                rp_log("bad --readers value '%s'", v);
                return 0;
            }
        } else if (strcmp(a, "--scale") == 0) {
            o->scale = atoi(v);
            if (o->scale < 1 || o->scale > 4) {
                rp_log("bad --scale value '%s'", v);
                return 0;
            }
        } else {
            rp_log("unknown option '%s'", a);
            return 0;
        }
    }

    o->inputs = &argv[i];
    o->n_inputs = argc - i;
    return o->n_inputs > 0;
}

/* ---- main ---------------------------------------------------------------- */

/* Leaves during startup, when a terminal may be waiting on our output. */
static int leave(int code)
{
    rp_console_prompt();
    return code;
}

static int default_thread_count(void)
{
    int n = SDL_GetCPUCount() - 1;
    return RP_CLAMP(n, 1, 16);
}

static void choose_window_size(const Sequence *seq, int *w, int *h)
{
    int iw = seq->width  > 0 ? seq->width  : 1280;
    int ih = seq->height > 0 ? seq->height : 720;

    int max_w = 1600, max_h = 900;
    SDL_DisplayMode dm;
    if (SDL_GetDesktopDisplayMode(0, &dm) == 0) {
        max_w = (int)(dm.w * 0.8);
        max_h = (int)(dm.h * 0.8);
    }

    int chrome = (58 + 22); /* transport + status bar, unscaled */
    double sc = 1.0;
    if (iw > max_w)            sc = (double)max_w / iw;
    if (ih * sc > max_h - chrome) sc = (double)(max_h - chrome) / ih;
    if (sc > 1.0) sc = 1.0;

    *w = RP_MAX(640, (int)(iw * sc));
    *h = RP_MAX(400, (int)(ih * sc) + chrome);
}

int main(int argc, char **argv)
{
    rp_console_attach();

    Options opt;
    if (!parse_args(argc, argv, &opt)) {
        usage(stderr);
        return leave(2);
    }

    char err[512];
    Sequence *seq = sequence_open(opt.inputs, opt.n_inputs, err, sizeof err);
    if (!seq) {
        rp_log("%s", err);
        return leave(1);
    }

    /* Probe a few frames rather than only the first: one damaged frame at the
     * head of a render should not stop the rest of it from playing. */
    double file_fps = 0.0;
    {
        int probe_idx[4] = { 0, seq->count / 2, seq->count - 1, seq->count / 4 };
        int ok = 0;
        for (int i = 0; i < RP_ARRAY_LEN(probe_idx) && !ok; i++) {
            int f = RP_CLAMP(probe_idx[i], 0, seq->count - 1);
            ok = reader_probe(seq->frames[f].path, &seq->width, &seq->height,
                              &file_fps, err, sizeof err);
            if (ok && i > 0)
                rp_log("note: '%s' is unreadable, sized from '%s' instead",
                       seq->frames[0].path, seq->frames[f].path);
        }
        if (!ok) {
            rp_log("cannot read '%s': %s", seq->frames[0].path, err);
            sequence_free(seq);
            return leave(1);
        }
    }
    if (!opt.fps_set && file_fps > 0.0 && file_fps <= 1000.0) opt.fps = file_fps;

    {
        char buf[32];
        rp_human_bytes(buf, sizeof buf, opt.mem_limit);
        size_t per = (size_t)seq->width * seq->height * 4;
        rp_log("%s: %d frames, %dx%d, %.4g fps, budget %s (about %zu frames)",
               seq->display, seq->count, seq->width, seq->height, opt.fps, buf,
               per ? opt.mem_limit / per : 0);
    }
    /* Startup printing is done; anything later is a rare failure. */
    rp_console_prompt();

    font_init();

    static ColorLUT lut;
    color_lut_init(&lut);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        rp_log("SDL_Init failed: %s", SDL_GetError());
        sequence_free(seq);
        return 1;
    }

    g_wake_event = SDL_RegisterEvents(1);
    atomic_init(&g_wake_pending, 0);

    int win_w, win_h;
    choose_window_size(seq, &win_w, &win_h);

    char title[512];
    snprintf(title, sizeof title, "ramplayer - %s", seq->display);
    g_window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                win_w, win_h,
                                SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!g_window) {
        rp_log("cannot create window: %s", SDL_GetError());
        SDL_Quit();
        sequence_free(seq);
        return 1;
    }

    g_renderer = SDL_CreateRenderer(g_window, -1,
                                    SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_renderer) g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    if (!g_renderer) {
        rp_log("cannot create renderer: %s", SDL_GetError());
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        sequence_free(seq);
        return 1;
    }

    if (opt.scale > 0) {
        ui_set_scale(opt.scale);
    } else {
        int ww = 0, ow = 0;
        SDL_GetWindowSize(g_window, &ww, NULL);
        SDL_GetRendererOutputSize(g_renderer, &ow, NULL);
        ui_set_scale((ww > 0 && ow >= ww * 3 / 2) ? 2 : 1);
    }

    int threads = opt.threads > 0 ? opt.threads : default_thread_count();
    reader_set_readers(opt.readers);
    Cache *cache = cache_create(seq, &lut, opt.mem_limit, threads,
                                (size_t)seq->width * seq->height * 4);
    if (!cache) {
        rp_log("cannot start the frame cache");
        SDL_DestroyRenderer(g_renderer);
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        sequence_free(seq);
        return 1;
    }
    cache_set_wakeup(cache, wake_main_thread, NULL);

    App app;
    app_init(&app, seq, cache, opt.fps);
    {
        int ow = 0, oh = 0;
        SDL_GetRendererOutputSize(g_renderer, &ow, &oh);
        app_resize(&app, ow, oh);
    }

    double wait = 0.25;
    while (!app.quit) {
        int ms = RP_CLAMP((int)(wait * 1000.0), 0, 250);

        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, ms)) {
            handle_event(&app, &e);
            while (SDL_PollEvent(&e)) handle_event(&app, &e);
        }
        if (app.quit) break;

        wait = app_tick(&app);
        app_update_shown(&app);

        if (app.need_redraw) {
            app.need_redraw = 0;
            present(&app);
        }
    }

    const char *cache_err = cache_last_error(cache);
    if (cache_err) rp_log("some frames failed to load: %s", cache_err);

    app_shutdown(&app);
    cache_destroy(cache);
    draw_shutdown();

    free(g_surface.px);
    if (g_texture) SDL_DestroyTexture(g_texture);
    SDL_DestroyRenderer(g_renderer);
    SDL_DestroyWindow(g_window);
    SDL_Quit();
    sequence_free(seq);
    return 0;
}
