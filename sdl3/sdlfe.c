/*
 * sdlfe.c: a from-scratch SDL3 front end for the puzzle collection.
 *
 * Milestone 1: drive a single (hard-coded) puzzle end to end -- create a
 * midend, implement the drawing_api on top of SDL3's 2D renderer, and
 * translate SDL input into midend_process_key(). Menus, thumbnails, the
 * on-screen keypad, supersampling, touch and the web/PWA build come in
 * later milestones.
 *
 * The puzzle is drawn into an offscreen render-target texture
 * (fe->puzzle_tex); each frame we blit that texture to the window. This
 * decouples the puzzle's incremental drawing (and blitter-based
 * animation) from frame presentation, and is where supersampling will
 * later hook in.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#ifdef NO_TGMATH_H
#  include <math.h>
#else
#  include <tgmath.h>
#endif

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_ttf/SDL_ttf.h>

#include "puzzles.h"
#include "puzzle-metadata.h"          /* generated: puzzle_metadata[] */
#include "storage.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
/* Browser history integration: push an entry when navigating into a game
 * or preset screen, so the browser/Android Back button returns to the
 * previous screen (via popstate -> web_popstate) instead of leaving the
 * page. */
EM_JS(void, js_history_push, (void), { history.pushState(null, ""); });
EM_JS(void, js_history_back, (void), { history.back(); });
static struct frontend *g_fe;          /* for the JS popstate bridge */
#endif

#ifndef PUZZLES_ASSET_DIR
#define PUZZLES_ASSET_DIR "sdl3/assets"
#endif
#ifndef PUZZLES_ICON_DIR
#define PUZZLES_ICON_DIR "icons"
#endif

#define DEFAULT_WINDOW_W 720
#define DEFAULT_WINDOW_H 900

/* Supersampling factor: the puzzle is drawn into an offscreen texture
 * this many times larger in each axis, then downscaled with linear
 * filtering at present time, giving cheap anti-aliasing. */
#define SUPERSAMPLE 2

/* Top-level UI state. */
enum { ST_MENU, ST_PLAY, ST_PRESETS };

/* Toolbar actions (the button->id values in ST_PLAY). */
enum { TB_MENU, TB_NEW, TB_RESTART, TB_UNDO, TB_REDO, TB_SOLVE, TB_TYPE };

/* ---------------------------------------------------------------------
 * Front end state.
 * ------------------------------------------------------------------- */

struct font {
    int type;                          /* FONT_FIXED or FONT_VARIABLE */
    int size;
    TTF_Font *ttf;
};

struct blitter {
    SDL_Texture *tex;
    int w, h;
    int x, y;                          /* where it was saved from */
};

/* A single on-screen keypad button. */
struct keyrect {
    SDL_FRect r;
    int button;
    char *label;
};

/* A generic clickable UI button (menu cells, toolbar, preset list). */
struct uibtn {
    SDL_FRect r;                       /* content coords (add scroll if scrolled) */
    char *label;
    int id;                            /* game index / TB_* / preset index */
    game_params *params;               /* for preset buttons (borrowed) */
};

struct frontend {
    SDL_Window *window;
    SDL_Renderer *renderer;

    int state;                         /* ST_MENU / ST_PLAY / ST_PRESETS */

    midend *me;                        /* the active puzzle (NULL in ST_MENU) */
    const game *ourgame;
    int game_idx;                      /* index into gamelist[] */

    SDL_Color *colours;                /* active colour table */
    int ncolours;

    int puzzle_w, puzzle_h;            /* puzzle pixel size (device px) */
    int ss;                            /* extra supersample factor for AA */
    float dpr;                         /* device pixels per layout point */
    SDL_Texture *puzzle_tex;           /* offscreen render target (ss-scaled) */
    SDL_Texture *render_target;        /* what start_draw() targets */

    struct font *fonts;
    int nfonts, fontsize;
    char *font_path, *mono_path;

    /* Window layout (logical window pixels). */
    int win_w, win_h;
    int toolbar_h;
    int play_x, play_y;                /* where puzzle_tex is presented */
    int status_y, status_h;
    int keypad_y, keypad_h;

    /* Toolbar (ST_PLAY). */
    struct uibtn *toolbar;
    int ntoolbar;

    /* On-screen keypad, derived from midend_request_keys(). */
    key_label *keys;
    int nkeys;
    struct keyrect *keyrects;
    int nkeyrects;

    /* Menu grid (ST_MENU). */
    struct uibtn *menu_cells;
    int nmenu_cells;
    int menu_content_h;
    int menu_scroll;
    SDL_Texture **thumbs;              /* gamecount entries, lazily rendered */

    /* Preset picker (ST_PRESETS). */
    struct uibtn *preset_items;
    int npreset_items;
    int preset_content_h;
    int preset_scroll;

    bool mouse_down;                   /* a mouse button is held in the play area */
    bool scrolling;                    /* dragging to scroll a list */
    float scroll_last_y;
    float scroll_vel;                  /* momentum (device px/frame) after release */

    SDL_Cursor *cur_default, *cur_hand;
    bool hand_on;

    /* Touch state machine (one active finger). */
    bool touch_active;
    SDL_FingerID touch_id;
    float touch_x0, touch_y0;          /* start position (window pixels) */
    Uint64 touch_start;
    bool touch_dragging;               /* a press has been emitted */
    bool touch_longpress;              /* long-press right-click fired */
    bool touch_scrolled;               /* finger moved enough to be a scroll */

    bool timer_active;
    Uint64 last_ticks;

    /* Persistence: save the active game's state after each settled
     * interaction, and remember which game is active across launches. */
    bool persist_enabled;              /* off for screenshot/test modes */
    bool dirty;                        /* game state changed since last save */

    char *statustext;

    /* Headless one-frame screenshot mode (PUZZLES_SHOT=<path>). */
    char *shot_path;
    int frames;
};

#define LONGPRESS_MS 450
#define TOUCH_MOVE_SLOP 12.0f          /* window px before a tap becomes a drag */

/* The drawing handle is the frontend pointer. */
#define FE_FROM_DR(dr) ((struct frontend *)((dr)->handle))

/* ---------------------------------------------------------------------
 * Colour helpers.
 * ------------------------------------------------------------------- */

static SDL_Color float_to_colour(const float *rgb)
{
    SDL_Color c;
    c.r = (Uint8)(rgb[0] * 255.0f + 0.5f);
    c.g = (Uint8)(rgb[1] * 255.0f + 0.5f);
    c.b = (Uint8)(rgb[2] * 255.0f + 0.5f);
    c.a = 255;
    return c;
}

static void set_draw_colour(struct frontend *fe, int colour)
{
    SDL_Color c = { 0, 0, 0, 255 };
    if (colour >= 0 && colour < fe->ncolours)
        c = fe->colours[colour];
    SDL_SetRenderDrawColor(fe->renderer, c.r, c.g, c.b, c.a);
}

/* ---------------------------------------------------------------------
 * Font handling (SDL_ttf).
 * ------------------------------------------------------------------- */

static TTF_Font *get_font(struct frontend *fe, int type, int size)
{
    int i;
    const char *path;
    TTF_Font *f;

    for (i = 0; i < fe->nfonts; i++)
        if (fe->fonts[i].type == type && fe->fonts[i].size == size)
            return fe->fonts[i].ttf;

    path = (type == FONT_FIXED) ? fe->mono_path : fe->font_path;
    f = TTF_OpenFont(path, (float)size);
    if (!f)
        return NULL;
    /* Disable kerning: DejaVu tucks letters under a leading 'T' (Type,
     * Towers, Tatham), which reads as a glitch in short UI labels. */
    TTF_SetFontKerning(f, false);

    if (fe->nfonts >= fe->fontsize) {
        fe->fontsize = fe->nfonts + 8;
        fe->fonts = sresize(fe->fonts, fe->fontsize, struct font);
    }
    fe->fonts[fe->nfonts].type = type;
    fe->fonts[fe->nfonts].size = size;
    fe->fonts[fe->nfonts].ttf = f;
    fe->nfonts++;
    return f;
}

/* Draw text directly to the current render target (used for window chrome:
 * status bar and keypad labels), in logical window pixels. */
static void draw_chrome_text(struct frontend *fe, int x, int y, int size,
                             int align, SDL_Color c, const char *text)
{
    TTF_Font *font;
    SDL_Surface *surf;
    SDL_Texture *tex;
    SDL_FRect dst;

    if (!text || !*text)
        return;
    /* Chrome is laid out in device pixels, so render fonts at device size. */
    size = (int)(size * fe->dpr);
    font = get_font(fe, FONT_VARIABLE, size);
    if (!font)
        return;
    surf = TTF_RenderText_Blended(font, text, strlen(text), c);
    if (!surf)
        return;
    tex = SDL_CreateTextureFromSurface(fe->renderer, surf);
    if (tex) {
        dst.w = (float)surf->w;
        dst.h = (float)surf->h;
        if (align & ALIGN_HCENTRE)      dst.x = x - surf->w / 2.0f;
        else if (align & ALIGN_HRIGHT)  dst.x = (float)(x - surf->w);
        else                            dst.x = (float)x;
        if (align & ALIGN_VCENTRE)      dst.y = y - surf->h / 2.0f;
        else                            dst.y = (float)y;
        SDL_RenderTexture(fe->renderer, tex, NULL, &dst);
        SDL_DestroyTexture(tex);
    }
    SDL_DestroySurface(surf);
}

/* ---------------------------------------------------------------------
 * drawing_api implementation.
 * ------------------------------------------------------------------- */

static void sdl_draw_text(drawing *dr, int x, int y, int fonttype,
                          int fontsize, int align, int colour,
                          const char *text)
{
    struct frontend *fe = FE_FROM_DR(dr);
    int ss = fe->ss;
    TTF_Font *font;
    SDL_Surface *surf;
    SDL_Texture *tex;
    SDL_Color c = { 0, 0, 0, 255 };
    SDL_FRect dst;
    int px = x * ss, py = y * ss, ascent;

    if (!text || !*text)
        return;
    font = get_font(fe, fonttype, fontsize * ss);
    if (!font)
        return;
    if (colour >= 0 && colour < fe->ncolours)
        c = fe->colours[colour];

    surf = TTF_RenderText_Blended(font, text, strlen(text), c);
    if (!surf)
        return;
    tex = SDL_CreateTextureFromSurface(fe->renderer, surf);
    if (!tex) {
        SDL_DestroySurface(surf);
        return;
    }

    dst.w = (float)surf->w;
    dst.h = (float)surf->h;

    if (align & ALIGN_HCENTRE)
        dst.x = px - surf->w / 2.0f;
    else if (align & ALIGN_HRIGHT)
        dst.x = (float)(px - surf->w);
    else
        dst.x = (float)px;

    ascent = TTF_GetFontAscent(font);
    if (align & ALIGN_VCENTRE)
        dst.y = py - surf->h / 2.0f;
    else
        dst.y = (float)(py - ascent);  /* y is the text baseline */

    SDL_RenderTexture(fe->renderer, tex, NULL, &dst);

    SDL_DestroyTexture(tex);
    SDL_DestroySurface(surf);
}

static void sdl_draw_rect(drawing *dr, int x, int y, int w, int h, int colour)
{
    struct frontend *fe = FE_FROM_DR(dr);
    int ss = fe->ss;
    SDL_FRect r;
    r.x = (float)(x * ss); r.y = (float)(y * ss);
    r.w = (float)(w * ss); r.h = (float)(h * ss);
    set_draw_colour(fe, colour);
    SDL_RenderFillRect(fe->renderer, &r);
}

/* A 1px logical line must cover ss device pixels: for the right visual
 * weight, and -- crucially -- so polygon fills (drawn by
 * draw_polygon_fallback as one logical scanline per row) leave no undrawn
 * device rows between scanlines.
 *
 * Axis-aligned lines are aligned to match draw_rect: a line at logical
 * coordinate c occupies device pixels [c*ss, c*ss+ss), the same span as a
 * rectangle edge there. This matters because some games (e.g. Tents) draw
 * their grid with draw_line on every redraw while the per-tile draw_rect
 * draws the same lines; if the two disagreed by a sub-pixel the lines would
 * thicken once incremental redraws stopped repainting every tile. Endpoints
 * keep a half-width square cap so corners join and fills reach edges.
 * Diagonals stay centred (no rectangle to match) for direction-independent
 * weight. */
static void sdl_draw_line(drawing *dr, int x1, int y1, int x2, int y2,
                          int colour)
{
    struct frontend *fe = FE_FROM_DR(dr);
    int ss = fe->ss;
    float w = (float)ss, hw = w / 2.0f;
    float X1 = (float)(x1 * ss), Y1 = (float)(y1 * ss);
    float X2 = (float)(x2 * ss), Y2 = (float)(y2 * ss);

    if (y1 == y2) {                    /* horizontal: top-aligned thick rect */
        float xa = (X1 < X2 ? X1 : X2) - hw, xb = (X1 > X2 ? X1 : X2) + hw;
        SDL_FRect r = { xa, Y1, xb - xa, w };
        set_draw_colour(fe, colour);
        SDL_RenderFillRect(fe->renderer, &r);
    } else if (x1 == x2) {             /* vertical: left-aligned thick rect */
        float ya = (Y1 < Y2 ? Y1 : Y2) - hw, yb = (Y1 > Y2 ? Y1 : Y2) + hw;
        SDL_FRect r = { X1, ya, w, yb - ya };
        set_draw_colour(fe, colour);
        SDL_RenderFillRect(fe->renderer, &r);
    } else {                           /* diagonal: centred thick quad */
        float dx = X2 - X1, dy = Y2 - Y1, len = sqrt(dx * dx + dy * dy);
        float ex = dx / len * hw, ey = dy / len * hw;   /* square cap */
        float nx = -dy / len * hw, ny = dx / len * hw;  /* perpendicular */
        float ax = X1 - ex, ay = Y1 - ey, bx = X2 + ex, by = Y2 + ey;
        SDL_Color col = { 0, 0, 0, 255 };
        SDL_FColor fc;
        SDL_Vertex v[4];
        int idx[6] = { 0, 1, 2, 0, 2, 3 }, i;
        if (colour >= 0 && colour < fe->ncolours)
            col = fe->colours[colour];
        fc.r = col.r / 255.0f; fc.g = col.g / 255.0f;
        fc.b = col.b / 255.0f; fc.a = col.a / 255.0f;
        v[0].position = (SDL_FPoint){ ax + nx, ay + ny };
        v[1].position = (SDL_FPoint){ ax - nx, ay - ny };
        v[2].position = (SDL_FPoint){ bx - nx, by - ny };
        v[3].position = (SDL_FPoint){ bx + nx, by + ny };
        for (i = 0; i < 4; i++) {
            v[i].color = fc;
            v[i].tex_coord.x = v[i].tex_coord.y = 0;
        }
        SDL_RenderGeometry(fe->renderer, NULL, v, 4, idx, 6);
    }
}

static void sdl_draw_polygon(drawing *dr, const int *coords, int npoints,
                             int fillcolour, int outlinecolour)
{
    /* draw_polygon_fallback() rasterises via our draw_line(). */
    draw_polygon_fallback(dr, coords, npoints, fillcolour, outlinecolour);
}

static void plot_circle_points(SDL_Renderer *r, int cx, int cy, int x, int y)
{
    SDL_RenderPoint(r, (float)(cx + x), (float)(cy + y));
    SDL_RenderPoint(r, (float)(cx - x), (float)(cy + y));
    SDL_RenderPoint(r, (float)(cx + x), (float)(cy - y));
    SDL_RenderPoint(r, (float)(cx - x), (float)(cy - y));
    SDL_RenderPoint(r, (float)(cx + y), (float)(cy + x));
    SDL_RenderPoint(r, (float)(cx - y), (float)(cy + x));
    SDL_RenderPoint(r, (float)(cx + y), (float)(cy - x));
    SDL_RenderPoint(r, (float)(cx - y), (float)(cy - x));
}

static void sdl_draw_circle(drawing *dr, int cx, int cy, int radius,
                            int fillcolour, int outlinecolour)
{
    struct frontend *fe = FE_FROM_DR(dr);
    int ss = fe->ss, dy;

    cx *= ss; cy *= ss; radius *= ss;

    /* Filled disc by horizontal spans. */
    if (fillcolour >= 0) {
        set_draw_colour(fe, fillcolour);
        for (dy = -radius; dy <= radius; dy++) {
            int dx = (int)(sqrt((double)radius * radius - (double)dy * dy));
            SDL_RenderLine(fe->renderer, (float)(cx - dx), (float)(cy + dy),
                           (float)(cx + dx), (float)(cy + dy));
        }
    }

    /* Outline via midpoint circle algorithm. */
    if (outlinecolour >= 0) {
        int x = radius, y = 0, err = 1 - radius;
        set_draw_colour(fe, outlinecolour);
        while (x >= y) {
            plot_circle_points(fe->renderer, cx, cy, x, y);
            y++;
            if (err < 0) {
                err += 2 * y + 1;
            } else {
                x--;
                err += 2 * (y - x) + 1;
            }
        }
    }
}

static void sdl_clip(drawing *dr, int x, int y, int w, int h)
{
    struct frontend *fe = FE_FROM_DR(dr);
    int ss = fe->ss;
    SDL_Rect r;
    r.x = x * ss; r.y = y * ss; r.w = w * ss; r.h = h * ss;
    SDL_SetRenderClipRect(fe->renderer, &r);
}

static void sdl_unclip(drawing *dr)
{
    struct frontend *fe = FE_FROM_DR(dr);
    SDL_SetRenderClipRect(fe->renderer, NULL);
}

static void sdl_start_draw(drawing *dr)
{
    struct frontend *fe = FE_FROM_DR(dr);
    SDL_SetRenderTarget(fe->renderer, fe->render_target);
    SDL_SetRenderClipRect(fe->renderer, NULL);
}

static void sdl_end_draw(drawing *dr)
{
    struct frontend *fe = FE_FROM_DR(dr);
    SDL_SetRenderClipRect(fe->renderer, NULL);
    SDL_SetRenderTarget(fe->renderer, NULL);
}

static void sdl_status_bar(drawing *dr, const char *text)
{
    struct frontend *fe = FE_FROM_DR(dr);
    sfree(fe->statustext);
    fe->statustext = text ? dupstr(text) : NULL;
}

static blitter *sdl_blitter_new(drawing *dr, int w, int h)
{
    struct frontend *fe = FE_FROM_DR(dr);
    int ss = fe->ss;
    blitter *bl = snew(blitter);
    bl->w = w;
    bl->h = h;
    bl->x = bl->y = -1;
    bl->tex = SDL_CreateTexture(fe->renderer, SDL_PIXELFORMAT_RGBA8888,
                                SDL_TEXTUREACCESS_TARGET, w * ss, h * ss);
    SDL_SetTextureBlendMode(bl->tex, SDL_BLENDMODE_NONE);
    return bl;
}

static void sdl_blitter_free(drawing *dr, blitter *bl)
{
    if (bl->tex)
        SDL_DestroyTexture(bl->tex);
    sfree(bl);
}

/* Copy the on-target region [x,y,w,h] (device px) to/from the blitter,
 * clamped to the target so a region overhanging an edge is copied 1:1
 * rather than stretched (which would smear when dragging near edges). */
static void blitter_xfer(struct frontend *fe, blitter *bl, int x, int y,
                         bool save)
{
    int ss = fe->ss;
    float tw = (float)(fe->puzzle_w * ss), th = (float)(fe->puzzle_h * ss);
    float bx = (float)(x * ss), by = (float)(y * ss);
    float bw = (float)(bl->w * ss), bh = (float)(bl->h * ss);
    float ix0 = max(bx, 0.0f), iy0 = max(by, 0.0f);
    float ix1 = min(bx + bw, tw), iy1 = min(by + bh, th);
    SDL_FRect on_target, in_blitter;

    if (ix1 <= ix0 || iy1 <= iy0)
        return;                        /* fully off the target */
    on_target.x = ix0; on_target.y = iy0;
    on_target.w = ix1 - ix0; on_target.h = iy1 - iy0;
    in_blitter.x = ix0 - bx; in_blitter.y = iy0 - by;
    in_blitter.w = on_target.w; in_blitter.h = on_target.h;

    if (save) {
        SDL_Texture *prev = SDL_GetRenderTarget(fe->renderer);
        SDL_SetRenderTarget(fe->renderer, bl->tex);
        SDL_RenderTexture(fe->renderer, fe->render_target,
                          &on_target, &in_blitter);
        SDL_SetRenderTarget(fe->renderer, prev);
    } else {
        SDL_RenderTexture(fe->renderer, bl->tex, &in_blitter, &on_target);
    }
}

static void sdl_blitter_save(drawing *dr, blitter *bl, int x, int y)
{
    struct frontend *fe = FE_FROM_DR(dr);
    bl->x = x;
    bl->y = y;
    blitter_xfer(fe, bl, x, y, true);
}

static void sdl_blitter_load(drawing *dr, blitter *bl, int x, int y)
{
    blitter_xfer(FE_FROM_DR(dr), bl, x, y, false);
}

static const drawing_api sdl_drawing = {
    1,                                 /* version */
    sdl_draw_text,
    sdl_draw_rect,
    sdl_draw_line,
    sdl_draw_polygon,
    sdl_draw_circle,
    NULL,                              /* draw_update: we re-present whole frame */
    sdl_clip,
    sdl_unclip,
    sdl_start_draw,
    sdl_end_draw,
    sdl_status_bar,
    sdl_blitter_new,
    sdl_blitter_free,
    sdl_blitter_save,
    sdl_blitter_load,
    NULL, NULL, NULL, NULL, NULL, NULL, /* printing: begin/end doc/page/puzzle */
    NULL, NULL,                        /* line_width, line_dotted */
    NULL,                              /* text_fallback (drawing.c has a default) */
    NULL,                              /* draw_thick_line (faked via polygon) */
};

/* ---------------------------------------------------------------------
 * Platform callbacks required by the midend and games.
 * ------------------------------------------------------------------- */

void frontend_default_colour(frontend *fe, float *output)
{
    output[0] = output[1] = output[2] = 0.9f;
}

void get_random_seed(void **randseed, int *randseedsize)
{
    time_t *t = snew(time_t);
    *t = time(NULL);
    *randseed = t;
    *randseedsize = sizeof(time_t);
}

void activate_timer(frontend *fe)
{
    fe->timer_active = true;
    fe->last_ticks = SDL_GetTicks();
}

void deactivate_timer(frontend *fe)
{
    fe->timer_active = false;
}

void fatal(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "fatal error: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}

/* Stub to satisfy midend_print_puzzle()'s reference; we never print. */
void document_add_puzzle(document *doc, const game *game, game_params *par,
                         game_ui *ui, game_state *st, game_state *st2) {}

/* ---------------------------------------------------------------------
 * Combined-binary game lookup.
 * ------------------------------------------------------------------- */

/* Normalise a game name to lowercase alphanumerics, so the command-line
 * id "blackbox" matches the back end's display name "Black Box". */
static void normalize_name(char *out, const char *in, size_t cap)
{
    size_t j = 0;
    for (; *in && j + 1 < cap; in++)
        if (isalnum((unsigned char)*in))
            out[j++] = (char)tolower((unsigned char)*in);
    out[j] = '\0';
}

static int find_game_index(const char *name)
{
    char want[64], have[64];
    int i;
    if (!name)
        return -1;
    normalize_name(want, name, sizeof want);
    for (i = 0; i < gamecount; i++) {
        normalize_name(have, gamelist[i]->name, sizeof have);
        if (!strcmp(want, have))
            return i;
    }
    return -1;
}

/* ---------------------------------------------------------------------
 * Colours and game lifecycle.
 * ------------------------------------------------------------------- */

static SDL_Color *colours_from_midend(midend *me, int *nc)
{
    int n, i;
    float *cols = midend_colours(me, &n);
    SDL_Color *out = snewn(n, SDL_Color);
    for (i = 0; i < n; i++)
        out[i] = float_to_colour(cols + 3 * i);
    sfree(cols);
    *nc = n;
    return out;
}

static const char *display_name(int idx)
{
    if (idx >= 0 && idx < n_puzzle_metadata && idx < gamecount)
        return puzzle_metadata[idx].displayname;
    return gamelist[idx]->name;
}

/* ---------------------------------------------------------------------
 * Layout.
 * ------------------------------------------------------------------- */

#define KEY_MIN_W 52
#define KEY_H 52
#define STATUS_H 30
#define TOOLBAR_H 48
#define HEADER_H 56
#define CELL_MIN_W 150
#define LIST_ITEM_H 56

static void size_puzzle_to(struct frontend *fe, int maxw, int maxh)
{
    int w = maxw, h = maxh;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    /* user_size=true: maximise the tile size to fill the area, so the
     * puzzle is as large as possible on screen (good for mobile). */
    midend_size(fe->me, &w, &h, true, 1.0);
    fe->puzzle_w = w;
    fe->puzzle_h = h;

    /* Reuse the existing offscreen target when its device-pixel size is
     * unchanged; only the GPU texture (re)allocation is skipped. We still
     * re-clear it below, so a game that leaves pixels unpainted can never
     * show stale content through. */
    if (fe->puzzle_tex) {
        float tw = 0, th = 0;
        SDL_GetTextureSize(fe->puzzle_tex, &tw, &th);
        if ((int)tw != w * fe->ss || (int)th != h * fe->ss) {
            SDL_DestroyTexture(fe->puzzle_tex);
            fe->puzzle_tex = NULL;
        }
    }
    if (!fe->puzzle_tex) {
        fe->puzzle_tex = SDL_CreateTexture(fe->renderer,
                                           SDL_PIXELFORMAT_RGBA8888,
                                           SDL_TEXTUREACCESS_TARGET,
                                           w * fe->ss, h * fe->ss);
        SDL_SetTextureScaleMode(fe->puzzle_tex, SDL_SCALEMODE_LINEAR);
        /* Composite the play area opaquely so nothing the game leaves
         * unpainted shows through as transparency. */
        SDL_SetTextureBlendMode(fe->puzzle_tex, SDL_BLENDMODE_NONE);
    }
    fe->render_target = fe->puzzle_tex;
    SDL_SetRenderTarget(fe->renderer, fe->puzzle_tex);
    SDL_SetRenderDrawColor(fe->renderer, 235, 235, 235, 255);
    SDL_RenderClear(fe->renderer);
    SDL_SetRenderTarget(fe->renderer, NULL);
}

static void free_keyrects(struct frontend *fe)
{
    int i;
    for (i = 0; i < fe->nkeyrects; i++)
        sfree(fe->keyrects[i].label);
    sfree(fe->keyrects);
    fe->keyrects = NULL;
    fe->nkeyrects = 0;
}

static void free_uibtns(struct uibtn *a, int n)
{
    int i;
    for (i = 0; i < n; i++)
        sfree(a[i].label);
    sfree(a);
}

static void fetch_keys(struct frontend *fe)
{
    if (fe->keys)
        free_keys(fe->keys, fe->nkeys);
    fe->keys = midend_request_keys(fe->me, &fe->nkeys);
}

static void add_toolbar_btn(struct frontend *fe, int *n, int id,
                            const char *label)
{
    fe->toolbar[*n].label = dupstr(label);
    fe->toolbar[*n].id = id;
    fe->toolbar[*n].params = NULL;
    (*n)++;
}

static void layout_play(struct frontend *fe)
{
    int W = fe->win_w, H = fe->win_h, n = 0, i, cols, rows, keyw, avail_h;
    float s = fe->dpr, bx;
    int key_h = (int)(KEY_H * s), key_min_w = (int)(KEY_MIN_W * s);
    int inset = (int)(2 * s);

    /* Top toolbar. */
    fe->toolbar_h = (int)(TOOLBAR_H * s);
    free_uibtns(fe->toolbar, fe->ntoolbar);
    fe->toolbar = snewn(7, struct uibtn);
    add_toolbar_btn(fe, &n, TB_MENU, "\xE2\x86\x90");  /* back arrow */
    add_toolbar_btn(fe, &n, TB_NEW, "New");
    add_toolbar_btn(fe, &n, TB_RESTART, "Restart");
    add_toolbar_btn(fe, &n, TB_UNDO, "Undo");
    add_toolbar_btn(fe, &n, TB_REDO, "Redo");
    if (fe->ourgame->can_solve)
        add_toolbar_btn(fe, &n, TB_SOLVE, "Solve");
    add_toolbar_btn(fe, &n, TB_TYPE, "Type");
    fe->ntoolbar = n;
    bx = 0;
    for (i = 0; i < n; i++) {
        float w = (float)W * (i + 1) / n - bx;
        fe->toolbar[i].r.x = bx;
        fe->toolbar[i].r.y = 0;
        fe->toolbar[i].r.w = w - 1;
        fe->toolbar[i].r.h = (float)fe->toolbar_h;
        bx += w;
    }

    /* Keypad along the bottom. */
    free_keyrects(fe);
    if (fe->nkeys > 0) {
        cols = W / key_min_w;
        if (cols < 1) cols = 1;
        if (cols > fe->nkeys) cols = fe->nkeys;
        rows = (fe->nkeys + cols - 1) / cols;
        keyw = W / cols;
        fe->keypad_h = rows * key_h;
        fe->keypad_y = H - fe->keypad_h;
        fe->keyrects = snewn(fe->nkeys, struct keyrect);
        fe->nkeyrects = fe->nkeys;
        for (i = 0; i < fe->nkeys; i++) {
            int r = i / cols, c = i % cols;
            fe->keyrects[i].r.x = (float)(c * keyw + inset);
            fe->keyrects[i].r.y = (float)(fe->keypad_y + r * key_h + inset);
            fe->keyrects[i].r.w = (float)(keyw - 2 * inset);
            fe->keyrects[i].r.h = (float)(key_h - 2 * inset);
            fe->keyrects[i].button = fe->keys[i].button;
            fe->keyrects[i].label = fe->keys[i].label
                ? dupstr(fe->keys[i].label) : button2label(fe->keys[i].button);
        }
    } else {
        fe->keypad_h = 0;
        fe->keypad_y = H;
    }

    fe->status_h = midend_wants_statusbar(fe->me) ? (int)(STATUS_H * s) : 0;
    fe->status_y = fe->keypad_y - fe->status_h;

    /* Size the puzzle in logical points (device area / dpr); it is rendered
     * into an ss-times-larger texture and composited back at puzzle*dpr. */
    avail_h = fe->status_y - fe->toolbar_h;
    size_puzzle_to(fe, (int)(W / s), (int)(avail_h / s));
    fe->play_x = (W - (int)(fe->puzzle_w * s)) / 2;
    fe->play_y = fe->toolbar_h + (avail_h - (int)(fe->puzzle_h * s)) / 2;
    if (fe->play_x < 0) fe->play_x = 0;
    if (fe->play_y < fe->toolbar_h) fe->play_y = fe->toolbar_h;
}

static void layout_menu(struct frontend *fe)
{
    int W = fe->win_w, H = fe->win_h, cols, rows, i;
    float s = fe->dpr;
    int header = (int)(HEADER_H * s), labelh = (int)(28 * s);
    int cellw, cellh, maxscroll;

    cols = W / (int)(CELL_MIN_W * s);
    if (cols < 1) cols = 1;
    cellw = W / cols;
    cellh = cellw + labelh;
    rows = (gamecount + cols - 1) / cols;

    free_uibtns(fe->menu_cells, fe->nmenu_cells);
    fe->menu_cells = snewn(gamecount, struct uibtn);
    fe->nmenu_cells = gamecount;
    for (i = 0; i < gamecount; i++) {
        int r = i / cols, c = i % cols;
        fe->menu_cells[i].r.x = (float)(c * cellw);
        fe->menu_cells[i].r.y = (float)(header + r * cellh);
        fe->menu_cells[i].r.w = (float)cellw;
        fe->menu_cells[i].r.h = (float)cellh;
        fe->menu_cells[i].id = i;
        fe->menu_cells[i].params = NULL;
        fe->menu_cells[i].label = dupstr(display_name(i));
    }
    fe->menu_content_h = header + rows * cellh;
    maxscroll = fe->menu_content_h - H;
    if (maxscroll < 0) maxscroll = 0;
    if (fe->menu_scroll > maxscroll) fe->menu_scroll = maxscroll;
    if (fe->menu_scroll < 0) fe->menu_scroll = 0;
}

static void layout_presets(struct frontend *fe)
{
    int W = fe->win_w, H = fe->win_h, i, maxscroll;
    float s = fe->dpr;
    int header = (int)(HEADER_H * s), itemh = (int)(LIST_ITEM_H * s);
    int pad = (int)(8 * s);
    for (i = 0; i < fe->npreset_items; i++) {
        fe->preset_items[i].r.x = (float)pad;
        fe->preset_items[i].r.y = (float)(header + i * itemh);
        fe->preset_items[i].r.w = (float)(W - 2 * pad);
        fe->preset_items[i].r.h = (float)(itemh - (int)(6 * s));
    }
    fe->preset_content_h = header + fe->npreset_items * itemh;
    maxscroll = fe->preset_content_h - H;
    if (maxscroll < 0) maxscroll = 0;
    if (fe->preset_scroll > maxscroll) fe->preset_scroll = maxscroll;
    if (fe->preset_scroll < 0) fe->preset_scroll = 0;
}

static void compute_layout(struct frontend *fe)
{
    int ww, wh;

    /* Lay out and render in device pixels for crisp HiDPI output; dpr is
     * the ratio of device pixels to the window's logical points. */
    SDL_GetRenderOutputSize(fe->renderer, &fe->win_w, &fe->win_h);
    SDL_GetWindowSize(fe->window, &ww, &wh);
    fe->dpr = (ww > 0) ? (float)fe->win_w / (float)ww : 1.0f;
    if (fe->dpr < 1.0f) fe->dpr = 1.0f;

    /* The puzzle is laid out in logical points and rendered into a texture
     * ss times larger, so puzzle "pixels" map to ~dpr device pixels (correct
     * line weight, scaled fixed-size features) with anti-aliasing. ss must
     * be >= dpr so the texture is at least device resolution. */
    fe->ss = (int)ceil(fe->dpr);
    if (fe->ss < 2) fe->ss = 2;

    if (fe->state == ST_MENU)
        layout_menu(fe);
    else if (fe->state == ST_PRESETS)
        layout_presets(fe);
    else
        layout_play(fe);
}

/* ---------------------------------------------------------------------
 * Persistence: serialise the active game and remember which is active.
 * ------------------------------------------------------------------- */

/* Stable storage key for a game (its lowercase id, e.g. "net"). */
static const char *game_key(int idx)
{
    return (idx >= 0 && idx < n_puzzle_metadata) ? puzzle_metadata[idx].name
                                                 : "game";
}

struct sbuf { char *buf; int len, cap; };
static void sbuf_write(void *ctx, const void *data, int len)
{
    struct sbuf *s = ctx;
    if (s->len + len + 1 > s->cap) {
        s->cap = (s->len + len + 1) * 2;
        s->buf = sresize(s->buf, s->cap, char);
    }
    memcpy(s->buf + s->len, data, len);
    s->len += len;
    s->buf[s->len] = '\0';
}
static char *serialise_to_string(midend *me)
{
    struct sbuf s;
    s.cap = 1024;
    s.buf = snewn(s.cap, char);
    s.len = 0;
    s.buf[0] = '\0';
    midend_serialise(me, sbuf_write, &s);
    return s.buf;                       /* caller frees with sfree */
}

struct rbuf { const char *buf; int len, pos; };
static bool rbuf_read(void *ctx, void *dst, int len)
{
    struct rbuf *r = ctx;
    if (r->pos + len > r->len)
        return false;
    memcpy(dst, r->buf + r->pos, len);
    r->pos += len;
    return true;
}
static bool deserialise_from_string(midend *me, const char *s)
{
    struct rbuf r;
    r.buf = s;
    r.len = (int)strlen(s);
    r.pos = 0;
    return midend_deserialise(me, rbuf_read, &r) == NULL;
}

/* Save the active game's full state under its key. */
static void persist_game(struct frontend *fe)
{
    char *s;
    if (!fe->persist_enabled || fe->state != ST_PLAY || !fe->me)
        return;
    s = serialise_to_string(fe->me);
    storage_set(game_key(fe->game_idx), s);
    sfree(s);
    fe->dirty = false;
}

/* ---------------------------------------------------------------------
 * Entering / leaving puzzles, and the preset picker.
 * ------------------------------------------------------------------- */

static void enter_game(struct frontend *fe, int idx)
{
    bool loaded = false;

    fe->game_idx = idx;
    fe->ourgame = gamelist[idx];
    fe->me = midend_new(fe, fe->ourgame, &sdl_drawing, fe);

    /* Resume this puzzle's saved state if we have one (and aren't in a
     * screenshot/test run); otherwise generate a fresh puzzle. */
    if (fe->persist_enabled) {
        char *saved = storage_get(game_key(idx));
        if (saved) {
            loaded = deserialise_from_string(fe->me, saved);
            sfree(saved);
        }
    }
    if (!loaded)
        midend_new_game(fe->me);

    sfree(fe->colours);
    fe->colours = colours_from_midend(fe->me, &fe->ncolours);
    fetch_keys(fe);
    fe->state = ST_PLAY;
    compute_layout(fe);
    midend_force_redraw(fe->me);
    if (fe->persist_enabled) {
        storage_set("active", game_key(idx));
        persist_game(fe);              /* save the resumed/fresh board */
    }
#ifdef __EMSCRIPTEN__
    js_history_push();                  /* browser Back returns to the menu */
#endif
}

static void leave_game(struct frontend *fe)
{
    persist_game(fe);                  /* final save before tearing down */
    if (fe->persist_enabled)
        storage_set("active", "");     /* relaunch shows the menu */
    if (fe->me) {
        midend_free(fe->me);
        fe->me = NULL;
    }
    fe->ourgame = NULL;
    if (fe->keys) {
        free_keys(fe->keys, fe->nkeys);
        fe->keys = NULL;
        fe->nkeys = 0;
    }
    free_keyrects(fe);
    sfree(fe->colours);
    fe->colours = NULL;
    fe->ncolours = 0;
    fe->render_target = NULL;
    fe->state = ST_MENU;
    compute_layout(fe);
}

/* One-level "back": presets -> play, or play -> menu. */
static void do_back(struct frontend *fe)
{
    if (fe->state == ST_PRESETS) {
        fe->state = ST_PLAY;
        compute_layout(fe);
        midend_force_redraw(fe->me);
    } else if (fe->state == ST_PLAY) {
        leave_game(fe);
    }
}

/* Trigger a back navigation. On the web this routes through browser history
 * (popstate -> web_popstate -> do_back) so the Back button stays in sync;
 * elsewhere it is immediate. */
static void request_back(struct frontend *fe)
{
#ifdef __EMSCRIPTEN__
    (void)fe;
    js_history_back();
#else
    do_back(fe);
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE void web_popstate(void)
{
    if (g_fe)
        do_back(g_fe);
}
#endif

static void collect_presets(struct preset_menu *menu, struct uibtn **items,
                            int *n, int *cap, const char *prefix)
{
    int i;
    for (i = 0; i < menu->n_entries; i++) {
        struct preset_menu_entry *e = &menu->entries[i];
        if (e->params) {
            char *label;
            if (prefix) {
                label = snewn(strlen(prefix) + strlen(e->title) + 4, char);
                sprintf(label, "%s: %s", prefix, e->title);
            } else {
                label = dupstr(e->title);
            }
            if (*n >= *cap) {
                *cap = *cap ? *cap * 2 : 16;
                *items = sresize(*items, *cap, struct uibtn);
            }
            (*items)[*n].label = label;
            (*items)[*n].params = e->params;
            (*items)[*n].id = e->id;
            (*n)++;
        } else if (e->submenu) {
            collect_presets(e->submenu, items, n, cap, e->title);
        }
    }
}

static void open_presets(struct frontend *fe)
{
    struct preset_menu *pm = midend_get_presets(fe->me, NULL);
    int cap = 0;
    free_uibtns(fe->preset_items, fe->npreset_items);
    fe->preset_items = NULL;
    fe->npreset_items = 0;
    if (pm)
        collect_presets(pm, &fe->preset_items, &fe->npreset_items, &cap, NULL);
    fe->preset_scroll = 0;
    fe->state = ST_PRESETS;
    compute_layout(fe);
#ifdef __EMSCRIPTEN__
    js_history_push();
#endif
}

static void apply_preset(struct frontend *fe, game_params *params)
{
    midend_set_params(fe->me, params);
    midend_new_game(fe->me);
    sfree(fe->colours);
    fe->colours = colours_from_midend(fe->me, &fe->ncolours);
    fetch_keys(fe);
    fe->dirty = true;                  /* save the new board once back in play */
    /* Selecting a preset returns to play; pop the presets history entry
     * (do_back transitions ST_PRESETS -> ST_PLAY). */
    request_back(fe);
}

/* ---------------------------------------------------------------------
 * Thumbnails: render each puzzle's bundled example state (icons .sav
 * files) into a cached texture, using our own drawing API.
 * ------------------------------------------------------------------- */

#define THUMB_PX 220

static bool file_read(void *ctx, void *buf, int len)
{
    return fread(buf, 1, len, (FILE *)ctx) == (size_t)len;
}

static SDL_Texture *thumb_for(struct frontend *fe, int idx)
{
    const game *g = gamelist[idx];
    midend *me2;
    SDL_Texture *tex, *save_target;
    SDL_Color *save_colours;
    int save_ncolours, tw = THUMB_PX, th = THUMB_PX;
    char path[512];
    FILE *f;
    bool loaded = false;

    if (fe->thumbs[idx])
        return fe->thumbs[idx];

    me2 = midend_new(fe, g, &sdl_drawing, fe);

    if (idx < n_puzzle_metadata) {
        snprintf(path, sizeof path, "%s/%s.sav",
                 PUZZLES_ICON_DIR, puzzle_metadata[idx].name);
        if ((f = fopen(path, "rb")) != NULL) {
            loaded = (midend_deserialise(me2, file_read, f) == NULL);
            fclose(f);
        }
    }
    if (!loaded)
        midend_new_game(me2);

    midend_size(me2, &tw, &th, true, 1.0);
    tex = SDL_CreateTexture(fe->renderer, SDL_PIXELFORMAT_RGBA8888,
                            SDL_TEXTUREACCESS_TARGET, tw * fe->ss, th * fe->ss);
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);

    /* Temporarily redirect the drawing context to this thumbnail. */
    save_colours = fe->colours;
    save_ncolours = fe->ncolours;
    save_target = fe->render_target;
    fe->colours = colours_from_midend(me2, &fe->ncolours);
    fe->render_target = tex;
    SDL_SetRenderTarget(fe->renderer, tex);
    SDL_SetRenderDrawColor(fe->renderer, 235, 235, 235, 255);
    SDL_RenderClear(fe->renderer);
    midend_force_redraw(me2);
    sfree(fe->colours);
    fe->colours = save_colours;
    fe->ncolours = save_ncolours;
    fe->render_target = save_target;

    midend_free(me2);
    fe->thumbs[idx] = tex;
    return tex;
}

/* ---------------------------------------------------------------------
 * Input translation.
 * ------------------------------------------------------------------- */

static int translate_key(SDL_Keycode key, SDL_Keymod mod)
{
    int button = -1;

    switch (key) {
      case SDLK_UP:    button = CURSOR_UP; break;
      case SDLK_DOWN:  button = CURSOR_DOWN; break;
      case SDLK_LEFT:  button = CURSOR_LEFT; break;
      case SDLK_RIGHT: button = CURSOR_RIGHT; break;
      case SDLK_RETURN:
      case SDLK_KP_ENTER: button = CURSOR_SELECT; break;
      case SDLK_SPACE: button = CURSOR_SELECT2; break;
      case SDLK_BACKSPACE:
      case SDLK_DELETE: button = '\177'; break;
      default:
        if (key >= 0x20 && key < 0x7f)
            button = (int)key;
        break;
    }
    if (button == -1)
        return -1;

    if (mod & SDL_KMOD_CTRL) {
        if (button >= 0x20 && button < 0x7f)
            button = button & 0x1f;
        button |= MOD_CTRL;
    }
    if (mod & SDL_KMOD_SHIFT)
        button |= MOD_SHFT;
    return button;
}

/* Is (X,Y) inside the play area, and if so what are the puzzle coords? */
static bool window_to_puzzle(struct frontend *fe, float X, float Y,
                             int *px, int *py)
{
    if (Y < fe->toolbar_h || Y >= fe->status_y)
        return false;                  /* toolbar / status bar / keypad */
    /* Window (device px) -> puzzle logical points. */
    *px = (int)((X - fe->play_x) / fe->dpr);
    *py = (int)((Y - fe->play_y) / fe->dpr);
    return true;
}

static bool in_rect(SDL_FRect *r, float x, float y)
{
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

/* If (X,Y) hits a keypad button, return its midend button code, else -1. */
static int hit_keypad(struct frontend *fe, float X, float Y)
{
    int i;
    for (i = 0; i < fe->nkeyrects; i++)
        if (in_rect(&fe->keyrects[i].r, X, Y))
            return fe->keyrects[i].button;
    return -1;
}

/* If (X,Y) hits a toolbar button, return its TB_* id, else -1. */
static int hit_toolbar(struct frontend *fe, float X, float Y)
{
    int i;
    for (i = 0; i < fe->ntoolbar; i++)
        if (in_rect(&fe->toolbar[i].r, X, Y))
            return fe->toolbar[i].id;
    return -1;
}

/* Hit-test a list of buttons in content coords (cy = window y + scroll). */
static int hit_uibtn(struct uibtn *a, int n, float X, float cy)
{
    int i;
    for (i = 0; i < n; i++)
        if (in_rect(&a[i].r, X, cy))
            return i;
    return -1;
}

static void send_key(struct frontend *fe, int button)
{
    midend_process_key(fe->me, 0, 0, button);
    midend_redraw(fe->me);
    fe->dirty = true;                  /* saved once the interaction settles */
}

static void send_at(struct frontend *fe, float X, float Y, int button)
{
    int px, py;
    if (!window_to_puzzle(fe, X, Y, &px, &py))
        return;
    midend_process_key(fe->me, px, py, button);
    midend_redraw(fe->me);
    fe->dirty = true;
}

static void clamp_scroll(struct frontend *fe)
{
    if (fe->state == ST_MENU) {
        int m = fe->menu_content_h - fe->win_h;
        if (m < 0) m = 0;
        if (fe->menu_scroll > m) fe->menu_scroll = m;
        if (fe->menu_scroll < 0) fe->menu_scroll = 0;
    } else if (fe->state == ST_PRESETS) {
        int m = fe->preset_content_h - fe->win_h;
        if (m < 0) m = 0;
        if (fe->preset_scroll > m) fe->preset_scroll = m;
        if (fe->preset_scroll < 0) fe->preset_scroll = 0;
    }
}

static void scroll_by(struct frontend *fe, int dy)
{
    if (fe->state == ST_MENU) fe->menu_scroll += dy;
    else if (fe->state == ST_PRESETS) fe->preset_scroll += dy;
    clamp_scroll(fe);
}

/* A tap (not a scroll) at window (X,Y) in a list view: select an item. */
static void list_select(struct frontend *fe, float X, float Y)
{
    if (fe->state == ST_MENU) {
        int idx;
        if (Y < HEADER_H * fe->dpr) return;
        idx = hit_uibtn(fe->menu_cells, fe->nmenu_cells, X, Y + fe->menu_scroll);
        if (idx >= 0)
            enter_game(fe, fe->menu_cells[idx].id);
    } else if (fe->state == ST_PRESETS) {
        int idx;
        if (Y < HEADER_H * fe->dpr) {  /* header acts as Back */
            request_back(fe);
            return;
        }
        idx = hit_uibtn(fe->preset_items, fe->npreset_items, X,
                        Y + fe->preset_scroll);
        if (idx >= 0)
            apply_preset(fe, fe->preset_items[idx].params);
    }
}

static void do_toolbar(struct frontend *fe, int id)
{
    switch (id) {
      case TB_MENU:    request_back(fe); break;
      case TB_NEW:     midend_new_game(fe->me); midend_force_redraw(fe->me);
                       fe->dirty = true; break;
      case TB_RESTART: midend_restart_game(fe->me); midend_force_redraw(fe->me);
                       fe->dirty = true; break;
      case TB_UNDO:    send_key(fe, UI_UNDO); break;
      case TB_REDO:    send_key(fe, UI_REDO); break;
      case TB_SOLVE:   midend_solve(fe->me); midend_force_redraw(fe->me);
                       fe->dirty = true; break;
      case TB_TYPE:    open_presets(fe); break;
    }
}

/* Show a hand cursor when hovering something clickable. */
static void update_cursor(struct frontend *fe, float X, float Y)
{
    bool clickable;
    if (fe->state == ST_MENU)
        clickable = (Y >= HEADER_H * fe->dpr) &&
            hit_uibtn(fe->menu_cells, fe->nmenu_cells,
                      X, Y + fe->menu_scroll) >= 0;
    else if (fe->state == ST_PRESETS)
        clickable = (Y < HEADER_H * fe->dpr) ||
            hit_uibtn(fe->preset_items, fe->npreset_items,
                      X, Y + fe->preset_scroll) >= 0;
    else
        clickable = (hit_toolbar(fe, X, Y) != -1) ||
                    (hit_keypad(fe, X, Y) != -1);

    if (clickable != fe->hand_on && fe->cur_hand && fe->cur_default) {
        fe->hand_on = clickable;
        SDL_SetCursor(clickable ? fe->cur_hand : fe->cur_default);
    }
}

/* ---------------------------------------------------------------------
 * Rendering (per state). All chrome is drawn directly to the window.
 * ------------------------------------------------------------------- */

static void draw_button(struct frontend *fe, SDL_FRect *r, const char *label,
                        int fontsize, bool enabled, bool highlight)
{
    SDL_Color fg;
    fg.a = 255;
    if (enabled) { fg.r = fg.g = fg.b = 240; }
    else         { fg.r = fg.g = fg.b = 120; }

    if (highlight) SDL_SetRenderDrawColor(fe->renderer, 58, 92, 140, 255);
    else           SDL_SetRenderDrawColor(fe->renderer, 70, 70, 78, 255);
    SDL_RenderFillRect(fe->renderer, r);
    SDL_SetRenderDrawColor(fe->renderer, 110, 110, 120, 255);
    SDL_RenderRect(fe->renderer, r);
    draw_chrome_text(fe, (int)(r->x + r->w / 2), (int)(r->y + r->h / 2),
                     fontsize, ALIGN_HCENTRE | ALIGN_VCENTRE, fg, label);
}

/* Draw a texture letterboxed (aspect-preserving) into an area. */
static void draw_texture_fit(struct frontend *fe, SDL_Texture *tex,
                             SDL_FRect area)
{
    float tw = 0, th = 0, s;
    SDL_FRect dst;
    SDL_GetTextureSize(tex, &tw, &th);
    if (tw <= 0 || th <= 0)
        return;
    s = (area.w / tw < area.h / th) ? area.w / tw : area.h / th;
    dst.w = tw * s;
    dst.h = th * s;
    dst.x = area.x + (area.w - dst.w) / 2;
    dst.y = area.y + (area.h - dst.h) / 2;
    SDL_RenderTexture(fe->renderer, tex, NULL, &dst);
}

static void draw_header(struct frontend *fe, const char *title)
{
    int header = (int)(HEADER_H * fe->dpr);
    SDL_Color fg = { 240, 240, 240, 255 };
    SDL_FRect hb = { 0, 0, (float)fe->win_w, (float)header };
    SDL_SetRenderDrawColor(fe->renderer, 24, 24, 28, 255);
    SDL_RenderFillRect(fe->renderer, &hb);
    draw_chrome_text(fe, fe->win_w / 2, header / 2, 22,
                     ALIGN_HCENTRE | ALIGN_VCENTRE, fg, title);
}

static void render_play(struct frontend *fe)
{
    SDL_FRect dst;
    int i;

    dst.x = (float)fe->play_x;
    dst.y = (float)fe->play_y;
    dst.w = fe->puzzle_w * fe->dpr;     /* on-screen size in device px */
    dst.h = fe->puzzle_h * fe->dpr;
    SDL_RenderTexture(fe->renderer, fe->puzzle_tex, NULL, &dst);

    /* Toolbar. */
    for (i = 0; i < fe->ntoolbar; i++) {
        int id = fe->toolbar[i].id;
        bool en = true;
        if (id == TB_UNDO) en = midend_can_undo(fe->me);
        else if (id == TB_REDO) en = midend_can_redo(fe->me);
        draw_button(fe, &fe->toolbar[i].r, fe->toolbar[i].label, 15, en, false);
    }

    /* Status bar. */
    if (fe->status_h > 0) {
        SDL_Color fg = { 235, 235, 235, 255 };
        SDL_FRect sr = { 0, (float)fe->status_y, (float)fe->win_w,
                         (float)fe->status_h };
        SDL_SetRenderDrawColor(fe->renderer, 48, 48, 54, 255);
        SDL_RenderFillRect(fe->renderer, &sr);
        if (fe->statustext)
            draw_chrome_text(fe, (int)(8 * fe->dpr),
                             fe->status_y + fe->status_h / 2, 15,
                             ALIGN_HLEFT | ALIGN_VCENTRE, fg, fe->statustext);
    }

    /* Keypad. */
    for (i = 0; i < fe->nkeyrects; i++)
        draw_button(fe, &fe->keyrects[i].r, fe->keyrects[i].label, 20,
                    true, false);
}

static void render_menu(struct frontend *fe)
{
    int i, pad = (int)(8 * fe->dpr), header = (int)(HEADER_H * fe->dpr);

    for (i = 0; i < fe->nmenu_cells; i++) {
        SDL_FRect cell = fe->menu_cells[i].r;
        float y = cell.y - fe->menu_scroll;
        SDL_FRect ta, panel;
        SDL_Color fg = { 225, 225, 230, 255 };
        SDL_Texture *tex;

        if (y + cell.h < header || y > fe->win_h)
            continue;                  /* off-screen */

        ta.x = cell.x + pad;
        ta.y = y + pad;
        ta.w = cell.w - 2 * pad;
        ta.h = cell.w - 2 * pad;       /* square thumbnail area */

        panel = ta;
        SDL_SetRenderDrawColor(fe->renderer, 50, 50, 56, 255);
        SDL_RenderFillRect(fe->renderer, &panel);

        tex = thumb_for(fe, i);
        if (tex)
            draw_texture_fit(fe, tex, ta);

        draw_chrome_text(fe, (int)(cell.x + cell.w / 2),
                         (int)(y + cell.w - pad + 4 * fe->dpr), 15,
                         ALIGN_HCENTRE, fg, fe->menu_cells[i].label);
    }

    draw_header(fe, "Simon Tatham's Puzzles");
}

static void render_presets(struct frontend *fe)
{
    int i, cur = midend_which_preset(fe->me);
    char title[128];

    for (i = 0; i < fe->npreset_items; i++) {
        SDL_FRect r = fe->preset_items[i].r;
        r.y -= fe->preset_scroll;
        if (r.y + r.h < HEADER_H * fe->dpr || r.y > fe->win_h)
            continue;
        draw_button(fe, &r, fe->preset_items[i].label, 18, true,
                    fe->preset_items[i].id == cur);
    }

    snprintf(title, sizeof title, "\xE2\x80\xB9 %s: type",
             display_name(fe->game_idx));
    draw_header(fe, title);
}

/* ---------------------------------------------------------------------
 * SDL main callbacks.
 * ------------------------------------------------------------------- */

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[])
{
    struct frontend *fe;
    const char *gamename;
    const char *env;
    int gameidx;

    fe = snew(struct frontend);
    memset(fe, 0, sizeof(*fe));
    fe->ss = SUPERSAMPLE;
    *appstate = fe;
#ifdef __EMSCRIPTEN__
    g_fe = fe;
#endif

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    if (!TTF_Init()) {
        fprintf(stderr, "TTF_Init: %s\n", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    fe->font_path = dupstr(PUZZLES_ASSET_DIR "/DejaVuSans.ttf");
    fe->mono_path = dupstr(PUZZLES_ASSET_DIR "/DejaVuSansMono.ttf");
    if ((env = getenv("PUZZLES_FONT")) != NULL) {
        sfree(fe->font_path); fe->font_path = dupstr(env);
    }
    if ((env = getenv("PUZZLES_FONT_MONO")) != NULL) {
        sfree(fe->mono_path); fe->mono_path = dupstr(env);
    }
    if ((env = getenv("PUZZLES_SHOT")) != NULL)
        fe->shot_path = dupstr(env);

    {
        SDL_WindowFlags wflags =
            SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
        if (fe->shot_path)
            wflags |= SDL_WINDOW_HIDDEN;   /* don't steal focus in shot mode */
        if (!SDL_CreateWindowAndRenderer(
                "Puzzles", DEFAULT_WINDOW_W, DEFAULT_WINDOW_H,
                wflags, &fe->window, &fe->renderer)) {
            fprintf(stderr, "SDL_CreateWindowAndRenderer: %s\n",
                    SDL_GetError());
            return SDL_APP_FAILURE;
        }
    }

    fe->cur_default = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    fe->cur_hand = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);

    fe->thumbs = snewn(gamecount, SDL_Texture *);
    memset(fe->thumbs, 0, gamecount * sizeof(SDL_Texture *));
    fe->state = ST_MENU;

    /* Persist game state during normal use, but not while taking
     * screenshots or running the input self-tests. */
    fe->persist_enabled = !(fe->shot_path || getenv("PUZZLES_SELFTEST") ||
                            getenv("PUZZLES_NAVTEST"));
    storage_init();

    gamename = (argc > 1) ? argv[1] : getenv("PUZZLES_GAME");
    gameidx = find_game_index(gamename);

    /* Cross-process persistence test (PUZZLES_PERSISTTEST=save|load): run
     * "save" makes a move and saves; a later "load" run should resume it. */
    if ((env = getenv("PUZZLES_PERSISTTEST")) != NULL) {
        if (!strcmp(env, "save")) {
            int idx = find_game_index("net");
            float cx, cy;
            enter_game(fe, idx);
            cx = fe->play_x + fe->puzzle_w * fe->dpr / 3.0f;
            cy = fe->play_y + fe->puzzle_h * fe->dpr / 3.0f;
            send_at(fe, cx, cy, LEFT_BUTTON);
            send_at(fe, cx, cy, LEFT_RELEASE);
            persist_game(fe);
            printf("PERSISTTEST save: net can_undo=%d\n",
                   midend_can_undo(fe->me));
        } else {
            char *active = storage_get("active");
            int idx = (active && *active) ? find_game_index(active) : -1;
            if (idx >= 0) {
                enter_game(fe, idx);
                printf("PERSISTTEST load: active=%s can_undo=%d\n",
                       active, midend_can_undo(fe->me));
            } else {
                printf("PERSISTTEST load: no active game\n");
            }
            sfree(active);
        }
        return SDL_APP_SUCCESS;
    }

    /* Headless input smoke test (PUZZLES_SELFTEST): drive the real input
     * path and confirm a move registers. A left click makes a move in
     * games like Net/Mines; for digit games we then type '1' into the
     * cell the click selected. */
    if (getenv("PUZZLES_SELFTEST")) {
        float cx, cy;
        bool before;
        if (gameidx < 0)
            gameidx = find_game_index("net");
        enter_game(fe, gameidx);
        cx = fe->play_x + fe->puzzle_w * fe->dpr / 3.0f;
        cy = fe->play_y + fe->puzzle_h * fe->dpr / 3.0f;
        before = midend_can_undo(fe->me);
        send_at(fe, cx, cy, LEFT_BUTTON);
        send_at(fe, cx, cy, LEFT_RELEASE);
        if (!midend_can_undo(fe->me) && fe->nkeys > 0)
            send_key(fe, '1');
        printf("SELFTEST %-10s click->move: %s (can_undo %d->%d, keys=%d)\n",
               fe->ourgame->name,
               (!before && midend_can_undo(fe->me)) ? "PASS" : "no-op",
               before, midend_can_undo(fe->me), fe->nkeys);
        return SDL_APP_SUCCESS;
    }

    /* Headless navigation test: drive the full flow through the real
     * hit-testing (tap a puzzle -> play -> Type -> pick preset -> New ->
     * Menu) and confirm the state transitions hold. */
    if (getenv("PUZZLES_NAVTEST")) {
        int si = find_game_index("solo"), ok = 1;
        SDL_FRect c;
        compute_layout(fe);
        ok &= (fe->state == ST_MENU);
        c = fe->menu_cells[si].r;
        list_select(fe, c.x + c.w / 2, c.y + 10);      /* tap Solo's cell */
        ok &= (fe->state == ST_PLAY && fe->game_idx == si && fe->me);
        do_toolbar(fe, TB_TYPE);                        /* open type picker */
        ok &= (fe->state == ST_PRESETS && fe->npreset_items > 0);
        c = fe->preset_items[2].r;
        list_select(fe, c.x + c.w / 2, c.y + 5);        /* pick 3rd preset */
        ok &= (fe->state == ST_PLAY);
        do_toolbar(fe, TB_NEW);
        do_toolbar(fe, TB_MENU);                        /* back to grid */
        ok &= (fe->state == ST_MENU && fe->me == NULL);
        printf("NAVTEST: %s\n", ok ? "PASS" : "FAIL");
        return SDL_APP_SUCCESS;
    }

    if (gameidx >= 0) {
        enter_game(fe, gameidx);       /* jump straight to a game (dev/shot) */
        if (getenv("PUZZLES_PRESETS")) /* debug: open the type picker */
            open_presets(fe);
    } else {
        /* Restore the game that was active when we were last closed, so
         * the app resumes exactly where the user left off. */
        int aidx = -1;
        if (fe->persist_enabled) {
            char *active = storage_get("active");
            if (active && *active)
                aidx = find_game_index(active);
            sfree(active);
        }
        if (aidx >= 0)
            enter_game(fe, aidx);
        else
            compute_layout(fe);        /* otherwise start at the menu */
    }

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate)
{
    struct frontend *fe = appstate;

    if (fe->state == ST_PLAY && fe->me) {
        if (fe->timer_active) {
            Uint64 now = SDL_GetTicks();
            float dt = (now - fe->last_ticks) / 1000.0f;
            fe->last_ticks = now;
            if (dt > 0)
                midend_timer(fe->me, dt);
        }
        /* Long-press: a still finger held in the play area becomes a
         * right-click (e.g. flagging in Mines). */
        if (fe->touch_active && !fe->touch_longpress && !fe->touch_dragging &&
            SDL_GetTicks() - fe->touch_start >= LONGPRESS_MS) {
            fe->touch_longpress = true;
            fe->touch_dragging = true; /* so finger-up emits RIGHT_RELEASE */
            send_at(fe, fe->touch_x0, fe->touch_y0, RIGHT_BUTTON | MOD_STYLUS);
        }
        /* Persist the board once an interaction has settled (no button or
         * finger held), so a kill at any moment loses nothing committed. */
        if (fe->dirty && !fe->mouse_down && !fe->touch_active)
            persist_game(fe);
    } else if (!fe->scrolling && !fe->touch_active &&
               fabs(fe->scroll_vel) >= 1.0f) {
        /* Momentum: glide the list after a flick, with friction. */
        int before = (fe->state == ST_MENU) ? fe->menu_scroll
                                            : fe->preset_scroll;
        scroll_by(fe, (int)(fe->scroll_vel +
                            (fe->scroll_vel > 0 ? 0.5f : -0.5f)));
        if (((fe->state == ST_MENU) ? fe->menu_scroll : fe->preset_scroll)
            == before)
            fe->scroll_vel = 0;        /* hit the top/bottom */
        else
            fe->scroll_vel *= 0.95f;   /* friction (lower = glides longer) */
        if (fabs(fe->scroll_vel) < 1.0f)
            fe->scroll_vel = 0;
    }

    SDL_SetRenderTarget(fe->renderer, NULL);
    SDL_SetRenderDrawColor(fe->renderer, 28, 28, 32, 255);
    SDL_RenderClear(fe->renderer);

    if (fe->state == ST_MENU)
        render_menu(fe);
    else if (fe->state == ST_PRESETS)
        render_presets(fe);
    else
        render_play(fe);

    /* Headless screenshot: capture the composed frame and quit. */
    if (fe->shot_path && fe->frames >= 1) {
        SDL_Surface *s = SDL_RenderReadPixels(fe->renderer, NULL);
        if (s) {
            if (!SDL_SaveBMP(s, fe->shot_path))
                fprintf(stderr, "SDL_SaveBMP: %s\n", SDL_GetError());
            else
                fprintf(stderr, "wrote %s (%dx%d)\n", fe->shot_path,
                        s->w, s->h);
            SDL_DestroySurface(s);
        }
        return SDL_APP_SUCCESS;
    }

    SDL_RenderPresent(fe->renderer);
    fe->frames++;
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
    struct frontend *fe = appstate;
    bool playing = (fe->state == ST_PLAY);

    /* Mouse coordinates arrive in logical points; convert them to render
     * (device) pixels to match our device-pixel layout. Touch events use
     * normalized coordinates, which we scale by the device size ourselves. */
    if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
        event->type == SDL_EVENT_MOUSE_BUTTON_UP ||
        event->type == SDL_EVENT_MOUSE_MOTION)
        SDL_ConvertEventToRenderCoordinates(fe->renderer, event);

    switch (event->type) {
      case SDL_EVENT_QUIT:
        return SDL_APP_SUCCESS;

      case SDL_EVENT_WINDOW_RESIZED:
      case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
        /* The web build emits a burst of identical resize events during
         * load. Only relayout (and force a full redraw) when the render
         * output size or device-pixel ratio has actually changed, so we
         * don't reallocate the offscreen target and repaint the whole
         * puzzle several times over for no change. compute_layout derives
         * dpr the same way, so recompute it here to compare. */
        int nw = 0, nh = 0, ww = 0, wh = 0;
        float ndpr;
        SDL_GetRenderOutputSize(fe->renderer, &nw, &nh);
        SDL_GetWindowSize(fe->window, &ww, &wh);
        ndpr = (ww > 0) ? (float)nw / (float)ww : 1.0f;
        if (ndpr < 1.0f) ndpr = 1.0f;
        if (nw == fe->win_w && nh == fe->win_h && ndpr == fe->dpr)
            break;                     /* spurious no-op resize */
        compute_layout(fe);
        if (fe->me)
            midend_force_redraw(fe->me);
        break;
      }

      case SDL_EVENT_KEY_DOWN:
        if (event->key.key == SDLK_ESCAPE) {
            if (fe->state == ST_MENU)
                return SDL_APP_SUCCESS;
            request_back(fe);          /* presets -> play, or play -> menu */
        } else if (playing) {
            int button = translate_key(event->key.key, event->key.mod);
            if (button != -1)
                send_key(fe, button);
        }
        break;

      case SDL_EVENT_MOUSE_WHEEL:
        if (!playing)
            scroll_by(fe, (int)(-event->wheel.y * 48));
        break;

      /* --- Mouse (ignore events synthesised from touch) --- */
      case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        float X = event->button.x, Y = event->button.y;
        if (event->button.which == SDL_TOUCH_MOUSEID) break;
        if (!playing) {
            fe->scrolling = true;
            fe->scroll_last_y = Y;
            fe->touch_x0 = X; fe->touch_y0 = Y;
            fe->touch_scrolled = false;
            fe->scroll_vel = 0;        /* grabbing stops momentum */
        } else {
            int id = hit_toolbar(fe, X, Y);
            int kp;
            if (id != -1) { do_toolbar(fe, id); break; }
            kp = hit_keypad(fe, X, Y);
            if (kp != -1) { send_key(fe, kp); break; }
            {
                int px, py, button =
                    (event->button.button == SDL_BUTTON_RIGHT) ? RIGHT_BUTTON :
                    (event->button.button == SDL_BUTTON_MIDDLE) ? MIDDLE_BUTTON :
                    LEFT_BUTTON;
                if (window_to_puzzle(fe, X, Y, &px, &py)) {
                    fe->mouse_down = true;
                    send_at(fe, X, Y, button);
                }
            }
        }
        break;
      }

      case SDL_EVENT_MOUSE_BUTTON_UP: {
        float X = event->button.x, Y = event->button.y;
        if (event->button.which == SDL_TOUCH_MOUSEID) break;
        if (!playing) {
            if (fe->scrolling && !fe->touch_scrolled)
                list_select(fe, X, Y);
            fe->scrolling = false;
        } else if (fe->mouse_down) {
            int button =
                (event->button.button == SDL_BUTTON_RIGHT) ? RIGHT_RELEASE :
                (event->button.button == SDL_BUTTON_MIDDLE) ? MIDDLE_RELEASE :
                LEFT_RELEASE;
            send_at(fe, X, Y, button);
            fe->mouse_down = false;
        }
        break;
      }

      case SDL_EVENT_MOUSE_MOTION:
        if (event->motion.which == SDL_TOUCH_MOUSEID) break;
        update_cursor(fe, event->motion.x, event->motion.y);
        if (!playing && fe->scrolling) {
            float delta = fe->scroll_last_y - event->motion.y;
            scroll_by(fe, (int)delta);
            fe->scroll_vel = 0.5f * fe->scroll_vel + 0.5f * delta;
            fe->scroll_last_y = event->motion.y;
            if (fabs(event->motion.y - fe->touch_y0) > TOUCH_MOVE_SLOP)
                fe->touch_scrolled = true;
        } else if (playing && fe->mouse_down) {
            int button = LEFT_DRAG;
            if (event->motion.state & SDL_BUTTON_RMASK) button = RIGHT_DRAG;
            else if (event->motion.state & SDL_BUTTON_MMASK) button = MIDDLE_DRAG;
            send_at(fe, event->motion.x, event->motion.y, button);
        }
        break;

      /* --- Touch --- */
      case SDL_EVENT_FINGER_DOWN:
        if (!fe->touch_active) {
            fe->touch_active = true;
            fe->touch_id = event->tfinger.fingerID;
            fe->touch_x0 = event->tfinger.x * fe->win_w;
            fe->touch_y0 = event->tfinger.y * fe->win_h;
            fe->scroll_last_y = fe->touch_y0;
            fe->touch_start = SDL_GetTicks();
            fe->touch_dragging = false;
            fe->touch_longpress = false;
            fe->touch_scrolled = false;
            fe->scroll_vel = 0;        /* grabbing stops momentum */
        }
        break;

      case SDL_EVENT_FINGER_MOTION:
        if (fe->touch_active && event->tfinger.fingerID == fe->touch_id) {
            float x = event->tfinger.x * fe->win_w;
            float y = event->tfinger.y * fe->win_h;
            if (!playing) {            /* scroll a list */
                float delta = fe->scroll_last_y - y;
                scroll_by(fe, (int)delta);
                fe->scroll_vel = 0.5f * fe->scroll_vel + 0.5f * delta;
                fe->scroll_last_y = y;
                if (fabs(y - fe->touch_y0) > TOUCH_MOVE_SLOP)
                    fe->touch_scrolled = true;
            } else if (!fe->touch_longpress) {
                if (!fe->touch_dragging &&
                    (fabs(x - fe->touch_x0) > TOUCH_MOVE_SLOP ||
                     fabs(y - fe->touch_y0) > TOUCH_MOVE_SLOP)) {
                    fe->touch_dragging = true;
                    send_at(fe, fe->touch_x0, fe->touch_y0,
                            LEFT_BUTTON | MOD_STYLUS);
                }
                if (fe->touch_dragging)
                    send_at(fe, x, y, LEFT_DRAG);
            }
        }
        break;

      case SDL_EVENT_FINGER_UP:
        if (fe->touch_active && event->tfinger.fingerID == fe->touch_id) {
            float x = event->tfinger.x * fe->win_w;
            float y = event->tfinger.y * fe->win_h;
            if (!playing) {
                if (!fe->touch_scrolled)
                    list_select(fe, x, y);
            } else {
                int id = hit_toolbar(fe, x, y);
                if (!fe->touch_longpress && !fe->touch_dragging && id != -1) {
                    do_toolbar(fe, id);
                } else if (fe->touch_longpress) {
                    send_at(fe, x, y, RIGHT_RELEASE);
                } else if (fe->touch_dragging) {
                    send_at(fe, x, y, LEFT_RELEASE);
                } else {
                    int kp = hit_keypad(fe, x, y);
                    if (kp != -1) {
                        send_key(fe, kp);
                    } else {
                        send_at(fe, x, y, LEFT_BUTTON | MOD_STYLUS);
                        send_at(fe, x, y, LEFT_RELEASE);
                    }
                }
            }
            fe->touch_active = false;
        }
        break;

      default:
        break;
    }
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result)
{
    struct frontend *fe = appstate;
    int i;

    if (!fe)
        return;
    free_keyrects(fe);
    free_uibtns(fe->toolbar, fe->ntoolbar);
    free_uibtns(fe->menu_cells, fe->nmenu_cells);
    free_uibtns(fe->preset_items, fe->npreset_items);
    if (fe->keys)
        free_keys(fe->keys, fe->nkeys);
    if (fe->me)
        midend_free(fe->me);
    if (fe->thumbs) {
        for (i = 0; i < gamecount; i++)
            if (fe->thumbs[i])
                SDL_DestroyTexture(fe->thumbs[i]);
        sfree(fe->thumbs);
    }
    for (i = 0; i < fe->nfonts; i++)
        TTF_CloseFont(fe->fonts[i].ttf);
    sfree(fe->fonts);
    sfree(fe->colours);
    sfree(fe->statustext);
    sfree(fe->font_path);
    sfree(fe->mono_path);
    sfree(fe->shot_path);
    if (fe->puzzle_tex)
        SDL_DestroyTexture(fe->puzzle_tex);
    if (fe->cur_default)
        SDL_DestroyCursor(fe->cur_default);
    if (fe->cur_hand)
        SDL_DestroyCursor(fe->cur_hand);
    if (fe->renderer)
        SDL_DestroyRenderer(fe->renderer);
    if (fe->window)
        SDL_DestroyWindow(fe->window);
    TTF_Quit();
    sfree(fe);
}
