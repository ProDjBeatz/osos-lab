/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Updraft: a one-button cave flyer for RepriseOS.
 *
 * Click Center (or Play) to flap; slip the bat through the gaps between the
 * stalactites and stalagmites. Menu quits, the Hold switch pauses.
 * All art is drawn in code.
 */
#include "game_api.h"
#include <stdbool.h>

#define RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

/* Simulation runs in fixed 1/60 s steps; positions in 1/256 pixel. */
#define STEP_MS 16
#define FIX 256
#define GRAVITY 92          /* per step, 1/256 px */
#define FLAP_VELOCITY -1450 /* per step, 1/256 px */
#define MAX_FALL 2300
#define GROUND_HEIGHT 18
#define COLUMN_WIDTH 42
#define COLUMN_SPACING 160
#define COLUMNS 4
#define BAT_X 84
#define BAT_W 30
#define BAT_H 20

enum state { TITLE, PLAYING, FALLING, OVER };

struct column {
    int x;   /* left edge, pixels */
    int gap; /* gap centre y */
    int gap_size;
    bool scored;
    uint8_t seed;
};

static enum state state;
static struct column columns[COLUMNS];
static int bat_y, bat_velocity; /* 1/256 px */
static int score, best;
static uint32_t last_ms, credit_ms, ticks;
static uint32_t scroll; /* world scroll, pixels */
static uint32_t rng = 0x2545f491u;
static bool paused, held;
static int flash; /* white flash frames after a crash */
static int flap_frames;

static uint32_t random_next(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* ---- Drawing helpers ---- */

static const struct game_framebuffer *fb;

static void fill(int x, int y, int w, int h, uint16_t color)
{
    int x0 = clamp(x, 0, (int)fb->width), x1 = clamp(x + w, 0, (int)fb->width);
    int y0 = clamp(y, 0, (int)fb->height), y1 = clamp(y + h, 0, (int)fb->height);
    for (int row = y0; row < y1; ++row) {
        uint16_t *p = fb->pixels + row * fb->stride_pixels;
        for (int col = x0; col < x1; ++col)
            p[col] = color;
    }
}

static void pixel(int x, int y, uint16_t color)
{
    if (x >= 0 && y >= 0 && x < (int)fb->width && y < (int)fb->height)
        fb->pixels[y * fb->stride_pixels + x] = color;
}

/* Blend toward black by level/8 (cheap shading for RGB565). */
static uint16_t shade(uint16_t c, int level)
{
    int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    r = r * (8 - level) / 8;
    g = g * (8 - level) / 8;
    b = b * (8 - level) / 8;
    return (uint16_t)(r << 11 | g << 5 | b);
}

/* Large text: a 5x7 font scaled up, for the title and the score. */
static const char font_chars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ !";
static const uint8_t font[][7] = {
    {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14},     {14, 17, 1, 2, 4, 8, 31},
    {30, 1, 1, 14, 1, 1, 30},     {2, 6, 10, 18, 31, 2, 2},    {31, 16, 30, 1, 1, 17, 14},
    {6, 8, 16, 30, 17, 17, 14},   {31, 1, 2, 4, 8, 8, 8},      {14, 17, 17, 14, 17, 17, 14},
    {14, 17, 17, 15, 1, 2, 12},   {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30},
    {14, 17, 16, 16, 16, 17, 14}, {28, 18, 17, 17, 17, 18, 28}, {31, 16, 16, 30, 16, 16, 31},
    {31, 16, 16, 30, 16, 16, 16}, {14, 17, 16, 23, 17, 17, 15}, {17, 17, 17, 31, 17, 17, 17},
    {14, 4, 4, 4, 4, 4, 14},      {7, 2, 2, 2, 2, 18, 12},     {17, 18, 20, 24, 20, 18, 17},
    {16, 16, 16, 16, 16, 16, 31}, {17, 27, 21, 21, 17, 17, 17}, {17, 17, 25, 21, 19, 17, 17},
    {14, 17, 17, 17, 17, 17, 14}, {30, 17, 17, 30, 16, 16, 16}, {14, 17, 17, 17, 21, 18, 13},
    {30, 17, 17, 30, 20, 18, 17}, {15, 16, 16, 14, 1, 1, 30},  {31, 4, 4, 4, 4, 4, 4},
    {17, 17, 17, 17, 17, 17, 14}, {17, 17, 17, 17, 17, 10, 4}, {17, 17, 17, 21, 21, 21, 10},
    {17, 17, 10, 4, 10, 17, 17},  {17, 17, 10, 4, 4, 4, 4},    {31, 1, 2, 4, 8, 16, 31},
    {0, 0, 0, 0, 0, 0, 0},        {4, 4, 4, 4, 4, 0, 4},
};

static int text_width(const char *s, int scale)
{
    int n = 0;
    while (s[n])
        n++;
    return n ? n * 6 * scale - scale : 0;
}

static void big_text(int x, int y, const char *s, int scale, uint16_t color)
{
    for (; *s; ++s, x += 6 * scale) {
        int index = -1;
        for (int i = 0; font_chars[i]; ++i)
            if (font_chars[i] == *s)
                index = i;
        if (index < 0)
            continue;
        for (int row = 0; row < 7; ++row)
            for (int col = 0; col < 5; ++col)
                if (font[index][row] & (16 >> col))
                    fill(x + col * scale, y + row * scale, scale, scale, color);
    }
}

/* Text with a drop shadow, centred on cx. */
static void label(int cx, int y, const char *s, int scale, uint16_t color)
{
    int x = cx - text_width(s, scale) / 2;
    big_text(x + scale / 2 + 1, y + scale / 2 + 1, s, scale, RGB(10, 6, 20));
    big_text(x, y, s, scale, color);
}

static void number_text(char *out, int value)
{
    char digits[12];
    int n = 0;
    do {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && n < 11);
    for (int i = 0; i < n; ++i)
        out[i] = digits[n - 1 - i];
    out[n] = 0;
}

/* ---- Scenery ---- */

/* Background: a vertical gradient, precomputed per row. */
static uint16_t sky[256];
/* Far cave wall silhouettes (two parallax layers), heights per world column. */
static uint8_t far_top[512], far_bottom[512], mid_top[512], mid_bottom[512];

static int wave(uint32_t x, uint32_t seed)
{
    /* Sum of triangle waves: cheap, smooth enough, no floating point. */
    int total = 0;
    static const int periods[3] = {97, 41, 17};
    static const int amplitudes[3] = {14, 6, 3};
    for (int i = 0; i < 3; ++i) {
        int p = periods[i];
        int t = (int)((x + seed * (i + 3)) % (uint32_t)p);
        int tri = t < p / 2 ? t : p - t;
        total += tri * amplitudes[i] * 2 / p;
    }
    return total;
}

static void build_scenery(void)
{
    for (int y = 0; y < 256; ++y) {
        int t = y > 240 ? 240 : y;
        sky[y] = RGB(22 + t / 12, 14 + t / 10, 44 + t / 6);
    }
    for (int x = 0; x < 512; ++x) {
        far_top[x] = (uint8_t)(18 + wave((uint32_t)x, 11));
        far_bottom[x] = (uint8_t)(22 + wave((uint32_t)x + 200, 29));
        mid_top[x] = (uint8_t)(8 + wave((uint32_t)x * 2, 53) / 2 * 2);
        mid_bottom[x] = (uint8_t)(10 + wave((uint32_t)x * 2 + 77, 71));
    }
}

static void draw_background(void)
{
    int w = (int)fb->width, h = (int)fb->height;
    int floor_y = h - GROUND_HEIGHT;
    for (int y = 0; y < floor_y; ++y) {
        uint16_t *p = fb->pixels + y * fb->stride_pixels;
        uint16_t c = sky[y & 255];
        for (int x = 0; x < w; ++x)
            p[x] = c;
    }
    uint16_t far = RGB(34, 26, 62), mid = RGB(26, 19, 46);
    uint32_t far_scroll = scroll / 4, mid_scroll = scroll / 2;
    for (int x = 0; x < w; ++x) {
        int f = (int)((x + far_scroll) & 511), m = (int)((x + mid_scroll) & 511);
        fill(x, 0, 1, far_top[f], far);
        fill(x, floor_y - far_bottom[f], 1, far_bottom[f], far);
        fill(x, 0, 1, mid_top[m], mid);
        fill(x, floor_y - mid_bottom[m], 1, mid_bottom[m], mid);
    }
    /* Drifting glow motes. */
    for (int i = 0; i < 14; ++i) {
        uint32_t s = (uint32_t)i * 2654435761u;
        int x = (int)((s % (uint32_t)(w + 40)) + (uint32_t)w * 4 - (scroll / 3 + (uint32_t)i * 7)) %
                (w + 40) - 20;
        int y = (int)((s >> 8) % (uint32_t)(floor_y - 40)) + 20 +
                (int)((ticks / 6 + (uint32_t)i * 13) % 20) - 10;
        uint16_t glow = (i & 1) ? RGB(120, 220, 200) : RGB(170, 140, 240);
        pixel(x, y, glow);
        if (i % 3 == 0) {
            pixel(x + 1, y, shade(glow, 3));
            pixel(x, y + 1, shade(glow, 3));
        }
    }
}

static void draw_ground(void)
{
    int w = (int)fb->width, h = (int)fb->height;
    int y0 = h - GROUND_HEIGHT;
    fill(0, y0, w, GROUND_HEIGHT, RGB(46, 34, 52));
    fill(0, y0, w, 2, RGB(90, 70, 96));
    for (int x = 0; x < w; ++x) {
        uint32_t wx = (uint32_t)x + scroll;
        if ((wx * 7 + (wx >> 3) * 13) % 23 == 0)
            fill(x, y0 + 5 + (int)(wx % 9), 3, 2, RGB(64, 50, 72));
    }
}

/* One rock spire, drawn from a base edge toward its tip, with a jagged outline.
   up: grows upward from the floor (stalagmite); otherwise hangs from the roof. */
static void draw_spire(int x, int base_y, int tip_y, bool up, uint8_t seed)
{
    uint16_t body = RGB(92, 78, 112), dark = RGB(60, 48, 80), light = RGB(140, 124, 160);
    int length = up ? base_y - tip_y : tip_y - base_y;
    for (int i = 0; i <= length; ++i) {
        int y = up ? base_y - i : base_y + i;
        /* Full width along the shaft, tapering only over the last 22 px. */
        int from_tip = length - i;
        int taper = from_tip < 22 ? (22 - from_tip) * (COLUMN_WIDTH / 2 - 3) / 22 : 0;
        int jag = (int)((((uint32_t)i * 37u + seed * 11u) >> 2) % 3);
        int left = x + taper + jag, right = x + COLUMN_WIDTH - taper - ((jag + 1) % 3);
        if (right <= left)
            right = left + 1;
        fill(left, y, right - left, 1, body);
        fill(left, y, 3, 1, light);
        fill(right - 5, y, 5, 1, dark);
        /* Mineral bands every so often. */
        if ((i + seed) % 29 < 2)
            fill(left + 3, y, right - left - 8, 1, shade(body, 2));
    }
    /* A crystal glinting near the tip of some spires. */
    if (seed % 3 == 0) {
        int cy = up ? tip_y + 28 : tip_y - 30;
        int cx = x + COLUMN_WIDTH / 2 - 2;
        uint16_t c = (seed & 4) ? RGB(110, 240, 210) : RGB(200, 150, 255);
        fill(cx, cy, 4, 6, c);
        fill(cx + 1, cy - 2, 2, 2, c);
        pixel(cx + 1, cy + 1, RGB(255, 255, 255));
    }
}

static void draw_columns(void)
{
    int floor_y = (int)fb->height - GROUND_HEIGHT;
    for (int i = 0; i < COLUMNS; ++i) {
        struct column *c = &columns[i];
        if (c->x > (int)fb->width || c->x + COLUMN_WIDTH < 0)
            continue;
        int top = c->gap - c->gap_size / 2, bottom = c->gap + c->gap_size / 2;
        draw_spire(c->x, 0, top, false, c->seed);
        draw_spire(c->x, floor_y, bottom, true, (uint8_t)(c->seed + 5));
    }
}

/* The bat: a 15x10 pixel sprite drawn at 2x (30x20 on screen), two wing poses.
   '#' body, 'w' wing, 'e' eye, 'f' fang, 'b' belly. */
#define BAT_SPRITE_W 15
#define BAT_SPRITE_H 10
static const char *const bat_up[BAT_SPRITE_H] = {
    "w             w",
    "ww   #   #   ww",
    "www  #####  www",
    "wwww##e#e##wwww",
    " wwww#####wwww ",
    "  www#bbb#www  ",
    "   ww#bbb#ww   ",
    "      #f#      ",
    "               ",
    "               ",
};
static const char *const bat_down[BAT_SPRITE_H] = {
    "               ",
    "     #   #     ",
    "     #####     ",
    "    ##e#e##    ",
    "  wwww###wwww  ",
    " wwwww#bb#wwwww",
    "wwwww #bbb#wwww",
    "ww     #f#   ww",
    "w             w",
    "               ",
};

static void draw_bat(int x, int y, bool wings_up, bool dead)
{
    const char *const *sprite = wings_up ? bat_up : bat_down;
    uint16_t body = dead ? RGB(150, 140, 150) : RGB(70, 52, 96);
    uint16_t wing = dead ? RGB(180, 170, 180) : RGB(150, 104, 196);
    uint16_t belly = dead ? RGB(200, 190, 200) : RGB(196, 160, 210);
    uint16_t edge = RGB(16, 8, 26);
    /* Outline first: every filled cell's neighbours, one screen pixel out. */
    for (int row = 0; row < BAT_SPRITE_H; ++row)
        for (int col = 0; col < BAT_SPRITE_W; ++col)
            if (sprite[row][col] != ' ')
                fill(x + col * 2 - 1, y + row * 2 - 1, 4, 4, edge);
    for (int row = 0; row < BAT_SPRITE_H; ++row)
        for (int col = 0; col < BAT_SPRITE_W; ++col) {
            char ch = sprite[row][col];
            if (ch == ' ')
                continue;
            uint16_t c = ch == '#' ? body : ch == 'w' ? wing : ch == 'b' ? belly
                       : ch == 'e' ? (dead ? RGB(40, 40, 40) : RGB(255, 222, 80))
                                   : RGB(255, 255, 255);
            fill(x + col * 2, y + row * 2, 2, 2, c);
            /* Rim light along the top edge of the wings. */
            if (ch == 'w' && !dead && (row == 0 || sprite[row - 1][col] == ' '))
                fill(x + col * 2, y + row * 2, 2, 1, RGB(214, 186, 246));
        }
}

/* ---- Game logic ---- */

static void place_column(struct column *c, int x)
{
    int floor_y = 240 - GROUND_HEIGHT;
    /* Gaps shrink slowly with the score, but never below 70 px. */
    int gap_size = 96 - score / 2;
    if (gap_size < 74)
        gap_size = 74;
    int margin = 30 + gap_size / 2;
    c->x = x;
    c->gap_size = gap_size;
    c->gap = margin + (int)(random_next() % (uint32_t)(floor_y - 2 * margin));
    c->scored = false;
    c->seed = (uint8_t)random_next();
}

static void reset(void)
{
    score = 0;
    bat_y = 100 * FIX;
    bat_velocity = 0;
    flash = 0;
    for (int i = 0; i < COLUMNS; ++i)
        place_column(&columns[i], 340 + i * COLUMN_SPACING);
}

static void flap(void)
{
    bat_velocity = FLAP_VELOCITY;
    flap_frames = 8;
}

static bool hits(const struct column *c, int bx, int by)
{
    /* Forgiving hitbox: the body and inner wings only. */
    int left = bx + 9, right = bx + BAT_W - 9, top = by + 3, bottom = by + 15;
    if (right < c->x + 3 || left > c->x + COLUMN_WIDTH - 3)
        return false;
    int gap_top = c->gap - c->gap_size / 2, gap_bottom = c->gap + c->gap_size / 2;
    return top < gap_top || bottom > gap_bottom;
}

static int speed(void)
{
    return score < 10 ? 2 : 3;
}

static void step(void)
{
    ticks++;
    if (flap_frames)
        flap_frames--;
    int floor_y = 240 - GROUND_HEIGHT;
    if (state == TITLE) {
        /* Hover gently on the title screen. */
        scroll += 1;
        int t = (int)(ticks % 64);
        bat_y = (96 + (t < 32 ? t : 64 - t) / 4) * FIX;
        if (ticks % 18 == 0)
            flap_frames = 8;
        return;
    }
    if (state == OVER) {
        if (flash)
            flash--;
        return;
    }
    bat_velocity += GRAVITY;
    if (bat_velocity > MAX_FALL)
        bat_velocity = MAX_FALL;
    bat_y += bat_velocity;
    if (bat_y < -4 * FIX) {
        bat_y = -4 * FIX;
        bat_velocity = 0;
    }
    int by = bat_y / FIX;
    if (by + 16 >= floor_y) {
        bat_y = (floor_y - 16) * FIX;
        if (state == PLAYING)
            flash = 6;
        state = OVER;
        if (score > best)
            best = score;
        return;
    }
    if (state == FALLING)
        return;

    scroll += (uint32_t)speed();
    for (int i = 0; i < COLUMNS; ++i) {
        struct column *c = &columns[i];
        c->x -= speed();
        if (c->x + COLUMN_WIDTH < -2) {
            int rightmost = c->x;
            for (int j = 0; j < COLUMNS; ++j)
                if (columns[j].x > rightmost)
                    rightmost = columns[j].x;
            place_column(c, rightmost + COLUMN_SPACING);
        }
        if (!c->scored && c->x + COLUMN_WIDTH < BAT_X + 5) {
            c->scored = true;
            score++;
        }
        if (hits(c, BAT_X, by)) {
            state = FALLING;
            flash = 6;
            bat_velocity = 0;
        }
    }
}

static void press(void)
{
    switch (state) {
    case TITLE:
        reset();
        state = PLAYING;
        flap();
        break;
    case PLAYING:
        flap();
        break;
    case FALLING:
        break;
    case OVER:
        if (flash == 0) {
            reset();
            state = TITLE;
        }
        break;
    }
}

/* ---- SDK callbacks ---- */

static void init(void)
{
    rng ^= game_ticks_ms() * 2654435761u;
    if (!rng)
        rng = 1;
    build_scenery();
    reset();
    state = TITLE;
    last_ms = game_ticks_ms();
    credit_ms = 0;
    paused = held = false;
}

static void input(const struct game_input_event *event)
{
    if (event->type != GAME_INPUT_PRESS)
        return;
    switch (event->button) {
    case GAME_BUTTON_MENU:
        game_request_exit();
        break;
    case GAME_BUTTON_CENTER:
    case GAME_BUTTON_PLAY:
        if (paused) {
            if (!held)
                paused = false;
        } else if (event->button == GAME_BUTTON_PLAY && state == PLAYING) {
            paused = true;
        } else {
            press();
        }
        break;
    default:
        break;
    }
}

static void draw_hud(void)
{
    int w = (int)fb->width;
    char text[16];
    if (state == TITLE) {
        label(w / 2, 46, "UPDRAFT", 5, RGB(255, 236, 170));
        if ((ticks / 30) % 2 == 0)
            label(w / 2, 150, "CLICK CENTER TO FLY", 2, RGB(240, 240, 255));
        if (best) {
            char line[24] = "BEST ";
            number_text(line + 5, best);
            label(w / 2, 180, line, 2, RGB(180, 230, 220));
        }
        return;
    }
    number_text(text, score);
    label(w / 2, 14, text, 4, RGB(255, 255, 255));
    if (state == OVER) {
        fill(w / 2 - 92, 72, 184, 96, RGB(30, 20, 48));
        fill(w / 2 - 92, 72, 184, 3, RGB(255, 236, 170));
        label(w / 2, 84, "GAME OVER", 3, RGB(255, 236, 170));
        char line[24] = "BEST ";
        number_text(line + 5, best);
        label(w / 2, 118, line, 2, RGB(180, 230, 220));
        if (flash == 0 && (ticks / 30) % 2 == 0)
            label(w / 2, 144, "CENTER TO RETRY", 2, RGB(240, 240, 255));
    }
    if (paused)
        label(w / 2, 100, held ? "HOLD ON" : "PAUSED", 3, RGB(255, 236, 170));
}

static void frame(const struct game_frame *game_frame)
{
    fb = &game_frame->framebuffer;
    uint32_t now = game_ticks_ms();
    uint32_t elapsed = now - last_ms;
    last_ms = now;
    if (elapsed > 100)
        elapsed = 100;

    int hold = game_hold_locked();
    held = hold == 1;
    if (held && state == PLAYING)
        paused = true;
    game_read_wheel(); /* Keep wheel deltas from piling up. */

    if (!paused) {
        credit_ms += elapsed;
        while (credit_ms >= STEP_MS) {
            credit_ms -= STEP_MS;
            step();
        }
    } else {
        credit_ms = 0;
        ticks++;
    }

    draw_background();
    draw_columns();
    draw_ground();
    bool wings_up = flap_frames > 4 || (state == TITLE && flap_frames > 0);
    draw_bat(BAT_X, bat_y / FIX, wings_up, state == FALLING || state == OVER);
    if (flash)
        fill(0, 0, (int)fb->width, (int)fb->height, flash > 3 ? RGB(255, 255, 255)
                                                            : RGB(255, 200, 220));
    draw_hud();
}

static void shutdown(void)
{
}

const struct game_app game_application = {
    .package_id = "reprise-updraft",
    .init = init,
    .input = input,
    .frame = frame,
    .shutdown = shutdown,
};
