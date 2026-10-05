/* gumshoe_ws.c - Gumshoe custom widescreen: the mod's configuration, its
 * compositor, its hook site and the game's additions to the cycle host
 * (cyc_host_extras.h). See docs/WIDESCREEN.md. */
#include "gumshoe_ws.h"
#include "gumshoe_ws_objects.h"
#include "gumshoe_ws_world.h"

#include "crc32.h"
#include "cyc_host_extras.h"
#include "cyc_render.h"
#include "cyc_ring.h"
#include "cyc_video.h"
#include "mod_function_hooks.h"
#include "mod_runtime.h"
#include "mod_savestate.h"
#ifdef CYC_WITH_SDL
#include "cyc_tcp.h"
#endif

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const uint8_t *gumshoe_ram;

static int s_enabled, s_hud_edges = 1, s_room_edges = 1, s_lookahead = 1;
static int s_aspect = NES_VIDEO_FIT;
static bool s_ready;
static uint64_t s_wide_frames, s_native_frames;
static const char *s_fallback = "off";
static int s_seam_samples, s_seam_errors, s_seam_unknown;
static int s_margin_unknown;   /* view columns inside the stage with no cached tile (row 15) */
static int s_render_camera, s_render_x0, s_view_left;
static uint8_t s_opaque[CYC_VIDEO_MAX_WIDTH * 240];
static FILE *s_log;
static void log_frame(void);

/* ---- the machine ---- */

bool gumshoe_call(uint16_t routine, uint8_t x) {
    CycModRegs r = { 0 };
    r.x = x;
    r.s = 0xfd;
    r.p = 0x24;
    return cyc_mod_call(routine, &r);
}

/* The three stage banks hold the same engine at different addresses; the
 * content keys (no game bytes here) pick the copy that is mapped now. */
static const GumshoeBank BANKS[3] = {
    { 0x8435, 0x964A, 0x94CB, 0x9495, 0x4DCAE086, 0xBB77A968 },
    { 0x8378, 0x9600, 0x94F9, 0x94C3, 0x4DCAE086, 0xBB77A968 },
    { 0x8807, 0x99F0, 0x98AF, 0x9861, 0x4DCAE086, 0xBB77A968 },
};

static uint32_t code_key(uint16_t addr) {
    uint8_t bytes[8];
    for (int i = 0; i < 8; i++) bytes[i] = cyc_mod_peek((uint16_t)(addr + i));
    return crc32_compute(bytes, sizeof bytes);
}

int gumshoe_ws_bank(const GumshoeBank **out) {
    for (int i = 0; i < 3; i++)
        if (code_key(BANKS[i].builder) == BANKS[i].builder_key && code_key(BANKS[i].drawer) == BANKS[i].drawer_key) {
            if (out) *out = &BANKS[i];
            return i;
        }
    return -1;
}

/* ---- the camera ---- */

enum { REF_LINE = 120 };

int gumshoe_ws_view_left(int cam, int width) {
    int view = cam - (width - 256) / 2;
    if (s_room_edges) {
        int left, right;
        gws_world_bounds(&left, &right);
        if (left != INT_MIN && right != INT_MAX && right - left <= width) view = left - (width - (right - left)) / 2;
        else {
            if (left != INT_MIN && view < left) view = left;
            if (right != INT_MAX && view + width > right) view = right - width;
        }
    }
    /* The native picture stays whole inside the canvas. */
    if (view > cam) view = cam;
    if (view < cam - (width - 256)) view = cam - (width - 256);
    return view;
}

/* The cached terrain against the native picture's background, over the
 * native columns, at the camera the compositor uses. Background coverage
 * does not depend on the palette, so the Zapper's black frames compare too. */
static void measure_seam(int cam) {
    const uint8_t *bg = cyc_frame_bg_opaque();
    s_seam_samples = s_seam_errors = s_seam_unknown = 0;
    for (int y = 0; y < 240; y += 2) {
        int ty = cyc_render_line_scroll_y(y) % 240, row = ty / 8;
        uint16_t pattern = (uint16_t)(cyc_render_line_bg_table(y) + (ty & 7));
        for (int x = 0; x < 256; x++) {
            uint8_t tile, pal;
            int wx = cam + x;
            if (!gws_world_tile(wx, row, &tile, &pal)) { s_seam_unknown++; continue; }
            int bit = 7 - (wx & 7);
            int pixel = ((cyc_render_chr((uint16_t)(pattern + tile * 16)) >> bit) & 1) |
                        (((cyc_render_chr((uint16_t)(pattern + tile * 16 + 8)) >> bit) & 1) << 1);
            s_seam_samples++;
            s_seam_errors += (pixel != 0) != (bg[y * 256 + x] != 0);
        }
    }
}

/* ---- the HUD ----
 * Timer (OAM 54-59) and shots (60-63) are sprites over the playfield's top
 * rows. Centered they stay in the native picture; at the edges the canvas's
 * left edge takes them, and the native picture under them is drawn again
 * from the terrain and the frame's other sprites. */
static bool hud_sprite(int slot) { return slot >= HUD_FIRST_SLOT && cyc_render_oam()[slot * 4] < HUD_LAST_LINE; }

static int place_hud(int slot, int x, int y, int *out_x, void *user) {
    (void)y; (void)user;
    if (!hud_sprite(slot)) return 0;
    *out_x = x;
    return 1;
}

static int place_others(int slot, int x, int y, int *out_x, void *user) {
    (void)y; (void)user;
    if (hud_sprite(slot)) return 0;
    *out_x = x;
    return 1;
}

enum { HUD_ROWS = HUD_LAST_LINE + 16 };
static uint32_t s_hud_redraw[256 * HUD_ROWS];
static uint8_t s_hud_opaque[256 * HUD_ROWS];

/* Before the native picture goes in: its HUD rows as terrain plus the
 * frame's other sprites. */
static bool prepare_hud(const uint32_t *out, int width, int x0) {
    bool any = false;
    for (int slot = HUD_FIRST_SLOT; slot < 64; slot++) any |= hud_sprite(slot);
    if (!any) return false;
    for (int y = 0; y < HUD_ROWS; y++) for (int x = 0; x < 256; x++) {
        s_hud_redraw[y * 256 + x] = out[y * width + x0 + x];
        s_hud_opaque[y * 256 + x] = s_opaque[y * width + x0 + x];
    }
    cyc_render_sprites(s_hud_redraw, 256, HUD_ROWS, 0, s_hud_opaque, place_others, NULL);
    return true;
}

static void move_hud(uint32_t *out, int width, int height, int x0) {
    const int rows = HUD_ROWS;
    const uint32_t *redraw = s_hud_redraw;
    const uint8_t *oam = cyc_render_oam();
    for (int slot = HUD_FIRST_SLOT; slot < 64; slot++) {
        if (!hud_sprite(slot)) continue;
        int top = oam[slot * 4] + 1, left = oam[slot * 4 + 3];
        for (int y = top; y < top + 16 && y < rows && y < height; y++)
            for (int x = left; x < left + 8 && x < 256; x++) out[y * width + x0 + x] = redraw[y * 256 + x];
    }
    cyc_render_sprites(out, width, height, 0, NULL, place_hud, NULL);
}

/* ---- the left-column clip ----
 * The game hides sprites in native columns 0-7 ($2001 bit 2 clear), which on
 * a console hides the screen edge. In the wide picture that strip sits inside
 * the view, so an object crossing it would lose eight columns: the frame's
 * OAM is drawn there again, with its priority against the native background. */
static uint32_t s_unclipped;
static int s_objects_hidden;   /* the composed picture left margin objects out */

static void unclip_sprites(uint32_t *out, int width, int height, int x0) {
    const uint8_t *oam = cyc_render_oam(), *bg = cyc_frame_bg_opaque();
    for (int slot = 63; slot >= 0; slot--) {
        const uint8_t *o = &oam[slot * 4];
        if (o[0] >= 0xEF || o[3] >= 8) continue;
        int top = o[0] + 1, x = o[3];
        unsigned attr = o[2];
        for (int r = 0; r < 16; r++) {
            int py = top + r;
            if (py >= height || py >= 240) break;
            uint8_t mask = cyc_render_line_mask(py);
            bool tall = cyc_render_line_sprite16(py);
            int rows = tall ? 16 : 8;
            if (r >= rows) break;
            if (!(mask & 0x10) || (mask & 0x04)) continue;
            int row = (attr & 0x80) ? rows - 1 - r : r;
            uint16_t pattern = tall ? (uint16_t)(((o[1] & 1) ? 0x1000 : 0) + (o[1] & 0xFE) * 16 + (row >= 8 ? 16 : 0))
                                    : (uint16_t)(cyc_render_line_sprite_table(py) + o[1] * 16);
            uint8_t lo = cyc_render_chr((uint16_t)(pattern + (row & 7)));
            uint8_t hi = cyc_render_chr((uint16_t)(pattern + (row & 7) + 8));
            for (int c = 0; c < 8 && x + c < 8; c++) {
                int bit = (attr & 0x40) ? c : 7 - c;
                int pixel = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
                if (!pixel || ((attr & 0x20) && bg[py * 256 + x + c])) continue;
                out[py * width + x0 + x + c] = cyc_render_color(16 + (attr & 3) * 4 + pixel);
                s_unclipped++;
            }
        }
    }
}

/* ---- the compositor ---- */

static int render(uint32_t *out, int width, int height, int native_x0, const uint32_t *native, void *user) {
    (void)native_x0; (void)user;
    if (!s_enabled) { s_fallback = "off"; s_native_frames++; return 0; }
    if (!g_gws_world.valid) { s_fallback = "no stage"; s_native_frames++; return 0; }
    if (!(cyc_render_line_mask(REF_LINE) & 0x08)) { s_fallback = "background off"; s_native_frames++; return 0; }
    int cam = gws_world_unwrap(cyc_render_line_scroll_x(REF_LINE));
    measure_seam(cam);
    /* Only a picture that shows the cached stage is extended: transitions,
     * text and title screens fall back to the centered native picture. */
    if (s_seam_samples < 256 * 60 || s_seam_errors * 100 > s_seam_samples) {
        s_fallback = "native picture differs";
        s_native_frames++;
        return 0;
    }
    int view = gumshoe_ws_view_left(cam, width), x0 = cam - view;
    s_render_camera = cam;
    s_view_left = view;
    s_render_x0 = x0;
    uint32_t backdrop = cyc_render_color(0), colors[4][4];
    for (int p = 0; p < 4; p++) for (int c = 0; c < 4; c++) colors[p][c] = c ? cyc_render_color(p * 4 + c) : backdrop;
    int stage_left, stage_right;
    gws_world_bounds(&stage_left, &stage_right);
    s_margin_unknown = 0;
    for (int x = 0; x < width; x += 8) {
        uint8_t tile, pal;
        int wx = view + x;
        if (wx >= stage_left && wx < stage_right && !gws_world_tile(wx, 15, &tile, &pal)) s_margin_unknown++;
    }
    for (int y = 0; y < height; y++) {
        int ty = cyc_render_line_scroll_y(y) % 240, row = ty / 8;
        uint16_t pattern = (uint16_t)(cyc_render_line_bg_table(y) + (ty & 7));
        uint32_t *line = &out[y * width];
        uint8_t *op = &s_opaque[y * width];
        for (int x = 0; x < width;) {
            int wx = view + x;
            uint8_t tile, pal;
            if (gws_world_tile(wx, row, &tile, &pal)) {
                uint8_t lo = cyc_render_chr((uint16_t)(pattern + tile * 16));
                uint8_t hi = cyc_render_chr((uint16_t)(pattern + tile * 16 + 8));
                int end = x + 8 - (wx & 7);
                if (end > width) end = width;
                for (; x < end; x++, wx++) {
                    int bit = 7 - (wx & 7), pixel = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
                    line[x] = colors[pal][pixel];
                    op[x] = pixel != 0;
                }
            } else {
                line[x] = backdrop;
                op[x] = 0;
                x++;
            }
        }
    }
    bool hud = s_hud_edges && x0 > 0 && prepare_hud(out, width, x0);
    /* The native picture stays authoritative in its columns. */
    const uint8_t *bg = cyc_frame_bg_opaque();
    for (int y = 0; y < height; y++) {
        memcpy(&out[y * width + x0], &native[y * 256], 256 * sizeof(uint32_t));
        memcpy(&s_opaque[y * width + x0], &bg[y * 256], 256);
    }
    /* Around a shot the game shows no objects: its black frames (background
     * palettes all $0F, only the white target boxes) and the frames after
     * with an empty OAM. The margins follow the native picture. */
    bool flash = true, sprites = false;
    for (int i = 0; i < 16; i++) flash &= cyc_render_palette(i) == 0x0F;
    for (int slot = 0; slot < 64; slot++) sprites |= cyc_render_oam()[slot * 4] < 0xEF;
    s_objects_hidden = flash || !sprites;
    if (!s_objects_hidden) {
        if (x0 > 0) unclip_sprites(out, width, height, x0);
        gws_objects_draw(out, width, height, x0, s_opaque);
    }
    if (hud) move_hud(out, width, height, x0);
    cyc_render_set_native_origin(x0);
    s_fallback = "composed";
    s_wide_frames++;
    return 1;
}

/* ---- configuration ---- */

static void apply(void) {
    if (!s_ready) return;
    if (s_enabled) {
        cyc_render_set_compositor(render, NULL);
        cyc_video_set_mode(s_aspect);
        cyc_mod_set_ppu_write_hook(gws_world_ppu_write);
    } else {
        cyc_render_set_compositor(NULL, NULL);
        cyc_video_set_mode(NES_VIDEO_STOCK);
        cyc_mod_set_ppu_write_hook(NULL);
        gws_world_reset();
        gws_objects_reset();
    }
    nes_mod_set_function_hook_enabled("gumshoe.widescreen.nmi", s_enabled);
}

void gumshoe_ws_set_mod_enabled(int enabled) {
    s_enabled = enabled != 0;
    if (!s_enabled) {
        s_aspect = NES_VIDEO_FIT;
        s_hud_edges = s_room_edges = 1;
    }
    apply();
}

void gumshoe_ws_configure(const char *aspect, const char *hud, const char *camera) {
    int value;
    if (aspect && nes_video_geometry_parse(aspect, &value) && value != NES_VIDEO_STOCK) s_aspect = value;
    s_hud_edges = !hud || strcmp(hud, "center") != 0;
    s_room_edges = !camera || strcmp(camera, "centered") != 0;
    apply();
}

/* ---- the hook site (game.toml) ---- */

static int nmi_hook(uint16_t addr) {
    (void)addr;
    /* The OAM this NMI is about to show was drawn from the object table as it
     * is now: capture the edge objects' packets for that picture. */
    if (s_enabled) gws_objects_capture();
    return 0;
}

/* ---- test fixtures (headless routes; never player features) ----
 * --dev-hold ADDR:VALUE keeps a RAM byte at VALUE after every frame (e.g.
 * $DC, the game's own invincibility timer, so a long route survives
 * enemies). --dev-autofire N is a Zapper autopilot: input only, like
 * --zapper-input, but closed loop - it shoots Stevenson (OAM 44-53) so he
 * jumps when the game's collision columns ($0300, 16 x 15 cells, one per
 * metatile column of the screen) show a hole or a wall just ahead of his
 * feet, at most every N frames. */
enum { DEV_HOLDS = 8 };
static struct { uint16_t addr; uint8_t value; } s_dev_hold[DEV_HOLDS];
static int s_dev_holds, s_dev_autofire;
static long s_dev_cooldown, s_dev_trigger;

static bool dev_solid(int column, int row) {
    if (row < 0) return false;
    if (row >= 15) return true;
    return RAM((uint16_t)(0x300 + 15 * (column & 15) + row)) != 0;
}

static void dev_fixtures(void) {
    for (int i = 0; i < s_dev_holds; i++) cyc_mod_poke(s_dev_hold[i].addr, s_dev_hold[i].value);
    if (!s_dev_autofire || RAM(RAM_StageMode) != STAGE_PLAY) return;
    if (s_dev_cooldown) s_dev_cooldown--;
    if (s_dev_trigger) {
        if (!--s_dev_trigger) cyc_set_zapper(-1, -1, false);
        return;
    }
    const uint8_t *oam = cyc_ppu_oam();
    int x0 = 256, y0 = 240, x1 = -1, y1 = -1;
    for (int slot = 44; slot <= 53; slot++) {
        const uint8_t *o = &oam[slot * 4];
        if (o[0] >= 0xEF) continue;
        if (o[3] < x0) x0 = o[3];
        if (o[3] + 8 > x1) x1 = o[3] + 8;
        if (o[0] + 1 < y0) y0 = o[0] + 1;
        if (o[0] + 9 > y1) y1 = o[0] + 9;
    }
    if (x1 < 0 || s_dev_cooldown) return;
    int column = ((RAM(RAM_ScrollX) + (x0 + x1) / 2) >> 4), feet = y1 / 16;
    bool standing = dev_solid(column, feet) || dev_solid(column + 1, feet) || dev_solid(column - 1, feet);
    bool obstacle = false;
    for (int k = 1; k <= 2; k++)
        obstacle |= !dev_solid(column + k, feet) || dev_solid(column + k, feet - 1) || dev_solid(column + k, feet - 2);
    if (!standing || !obstacle) return;
    cyc_set_zapper((x0 + x1) / 2, (y0 + y1) / 2, true);
    s_dev_trigger = 3;
    s_dev_cooldown = s_dev_autofire;
}

/* ---- the game's additions to the host ---- */

static void x_power_on(void *ctx) {
    (void)ctx;
    gumshoe_ram = cyc_cpu_ram();
    gws_world_reset();
    gws_objects_reset();
    s_wide_frames = s_native_frames = 0;
    s_ready = true;
    apply();
}

static void x_frame_end(void *ctx) {
    (void)ctx;
    dev_fixtures();
    if (s_enabled) {
        if (!s_lookahead) gws_world_invalidate_lookahead();
        gws_world_update();
    }
    if (s_log) {
        /* The log measures the composed picture of every frame, not only of
         * the frames a window or --present-out shows. */
        int w, h;
        cyc_render_present(&w, &h);
        log_frame();
    }
}

static const CycHostOption OPTIONS[] = {
    { "--widescreen", true, "fit | 16:9 | 21:9 | 32:9 | off: enable the widescreen mod at that aspect (overrides Mods)" },
    { "--widescreen-camera", true, "edges | centered" },
    { "--widescreen-hud", true, "edges | center" },
    { "--widescreen-lookahead", true, "on | off (off: diagnostics only, no isolated builder runs)" },
    { "--widescreen-ring", true, "128 | 256 world columns cached (128: wraparound diagnostics)" },
    { "--widescreen-log", true, "FILE: one JSON line per frame (the mod's state, seams, objects; probes)" },
    { "--dev-hold", true, "ADDR:VALUE keep a RAM byte at VALUE after each frame (test fixture; repeatable)" },
    { "--dev-autofire", true, "N: pull the Zapper at Stevenson every N frames (test fixture; headless)" },
};

static bool x_option(void *ctx, const char *name, const char *value) {
    (void)ctx;
    if (!strcmp(name, "--widescreen")) {
        int aspect;
        if (!strcmp(value, "off")) { gumshoe_ws_set_mod_enabled(0); return true; }
        if (!nes_video_geometry_parse(value, &aspect) || aspect == NES_VIDEO_STOCK) return false;
        s_aspect = aspect;
        s_enabled = 1;
        apply();
        return true;
    }
    if (!strcmp(name, "--widescreen-camera")) {
        if (strcmp(value, "edges") && strcmp(value, "centered")) return false;
        s_room_edges = !strcmp(value, "edges");
        return true;
    }
    if (!strcmp(name, "--widescreen-hud")) {
        if (strcmp(value, "edges") && strcmp(value, "center")) return false;
        s_hud_edges = !strcmp(value, "edges");
        return true;
    }
    if (!strcmp(name, "--widescreen-lookahead")) {
        if (strcmp(value, "on") && strcmp(value, "off")) return false;
        s_lookahead = !strcmp(value, "on");
        return true;
    }
    if (!strcmp(name, "--widescreen-ring")) {
        if (!gws_world_set_ring(atoi(value))) return false;
        gws_world_reset();
        return true;
    }
    if (!strcmp(name, "--widescreen-log")) {
        s_log = fopen(value, "w");
        return s_log != NULL;
    }
    if (!strcmp(name, "--dev-hold")) {
        char *end;
        long addr = strtol(value, &end, 0), v;
        if (*end != ':' || addr < 0 || addr > 0x7FF || s_dev_holds == DEV_HOLDS) return false;
        v = strtol(end + 1, &end, 0);
        if (*end || v < 0 || v > 255) return false;
        s_dev_hold[s_dev_holds].addr = (uint16_t)addr;
        s_dev_hold[s_dev_holds++].value = (uint8_t)v;
        return true;
    }
    if (!strcmp(name, "--dev-autofire")) {
        s_dev_autofire = atoi(value);
        return s_dev_autofire >= 8;
    }
    return false;
}

static int state_json(char *buf, int cap) {
    const GwsWorld *w = &g_gws_world;
    int left, right;
    gws_world_bounds(&left, &right);
    return snprintf(buf, (size_t)cap,
        "\"enabled\":%d,\"compositor\":%d,\"render_width\":%d,\"picture\":\"%s\",\"bank\":%d,\"stage_mode\":%u,"
        "\"attract\":%u,\"program\":%u,\"valid\":%d,\"start_known\":%d,\"camera\":%d,\"render_camera\":%d,"
        "\"view_left\":%d,\"native_x0\":%d,\"room_edges\":%d,\"hud_edges\":%d,\"head\":%d,\"stage_end\":%d,"
        "\"stage_left\":%d,\"stage_right\":%d,\"lookahead_runs\":%u,\"lookahead_columns\":%u,"
        "\"lookahead_failures\":%u,\"verified_tiles\":%u,\"mismatched_tiles\":%u,\"first_mismatch\":[%d,%d],"
        "\"verified_palettes\":%u,\"mismatched_palettes\":%u,\"first_pal_mismatch\":[%d,%d,%d,%d],"
        "\"ppu_writes\":%u,\"seam_samples\":%d,\"seam_errors\":%d,\"seam_unknown\":%d,\"margin_unknown\":%d,"
        "\"ring\":%d,\"lookahead_count\":%d,\"unclipped_pixels\":%u,\"objects_hidden\":%d,"
        "\"wide_frames\":%llu,\"native_frames\":%llu",
        s_enabled, cyc_render_has_compositor(), cyc_video_width(), s_fallback, w->bank, RAM(RAM_StageMode),
        RAM(RAM_Attract), RAM(RAM_Program), w->valid, w->start_known, w->cam, s_render_camera, s_view_left,
        s_render_x0, s_room_edges, s_hud_edges, w->head, w->stage_end == INT32_MAX ? -1 : w->stage_end,
        left == INT_MIN ? -1 : left, right == INT_MAX ? -1 : right, w->lookahead_runs, w->lookahead_columns,
        w->lookahead_failures, w->verified_tiles, w->mismatched_tiles,
        w->first_mismatch_column == GWS_UNKNOWN_COLUMN ? -1 : w->first_mismatch_column, w->first_mismatch_row,
        w->verified_palettes, w->mismatched_palettes,
        w->first_pal_mismatch_column == GWS_UNKNOWN_COLUMN ? -1 : w->first_pal_mismatch_column, w->first_pal_mismatch_row,
        w->first_pal_expected, w->first_pal_actual,
        w->ppu_writes, s_seam_samples, s_seam_errors, s_seam_unknown, s_margin_unknown, w->ring, w->ahead_count,
        s_unclipped, s_objects_hidden,
        (unsigned long long)s_wide_frames,
        (unsigned long long)s_native_frames);
}

/* --widescreen-log FILE: one JSON line per frame, numbered as the machine
 * counts them (a loaded state continues the saved run's numbers). */
static void log_frame(void) {
    static char buf[8192];
    int n = snprintf(buf, sizeof buf, "{\"frame\":%lu,", (unsigned long)cyc_ring_frame);
    n += state_json(buf + n, (int)sizeof buf - n);
    buf[n++] = ',';
    n += gws_objects_json(buf + n, (int)sizeof buf - n);
    fprintf(s_log, "%.*s}\n", n, buf);
}

#ifdef CYC_WITH_SDL
static void tcp_state(int id, const char *line) {
    (void)line;
    char f[2048];
    int w, h;
    cyc_render_present(&w, &h);
    int n = state_json(f, (int)sizeof f);
    f[n++] = ',';
    gws_objects_json(f + n, (int)sizeof f - n);
    cyc_tcp_ok(id, f);
}

static void x_tcp_setup(void *ctx) {
    (void)ctx;
    cyc_tcp_register("gumshoe_ws_state", "widescreen: mode, camera, world cache, seam, edge objects", tcp_state);
}
#endif

static const CycHostExtras EXTRAS = {
    .ctx = NULL,
    .power_on = x_power_on,
    .frame_end = x_frame_end,
    .options = OPTIONS,
    .option_count = sizeof OPTIONS / sizeof OPTIONS[0],
    .option = x_option,
#ifdef CYC_WITH_SDL
    .tcp_setup = x_tcp_setup,
#endif
};

const CycHostExtras *cyc_host_extras(void) { return &EXTRAS; }

/* ---- save states ----
 * The world cache and the edge packets are mod records (versioned); settings
 * belong to the Mods selection and are never taken from a loaded state. A
 * state saved without the mod loads with an empty cache, which the next frame
 * fills from the nametables. */
static int save_world(uint8_t *data, int cap) {
    if (!s_enabled) return 0;
    if (cap < (int)sizeof g_gws_world) return -1;
    memcpy(data, &g_gws_world, sizeof g_gws_world);
    return sizeof g_gws_world;
}
static int validate_world(const uint8_t *data, int len) {
    if (!len) return 1;
    if (len != (int)sizeof g_gws_world) return 0;
    GwsWorld w;
    memcpy(&w, data, sizeof w);
    return w.version == GWS_WORLD_VERSION && (w.ring == 128 || w.ring == GWS_RING) && (w.valid == 0 || w.valid == 1) &&
           w.bank >= -1 && w.bank <= 2;
}
static int load_world(const uint8_t *data, int len) {
    if (!validate_world(data, len)) return 0;
    if (!len || !s_enabled) { gws_world_reset(); return 1; }
    memcpy(&g_gws_world, data, sizeof g_gws_world);
    return 1;
}
static int save_objects(uint8_t *data, int cap) {
    if (!s_enabled) return 0;
    if (cap < (int)sizeof g_gws_objects) return -1;
    memcpy(data, &g_gws_objects, sizeof g_gws_objects);
    return sizeof g_gws_objects;
}
static int validate_objects(const uint8_t *data, int len) {
    if (!len) return 1;
    if (len != (int)sizeof g_gws_objects) return 0;
    GwsObjects o;
    memcpy(&o, data, sizeof o);
    if (o.version != GWS_OBJECTS_VERSION) return 0;
    for (int i = 0; i < OBJ_SLOTS; i++)
        if (o.packet[i].count < 0 || o.packet[i].count > GWS_PACKET_SPRITES) return 0;
    for (int i = 0; i < GWS_RESIDENTS; i++)
        if (o.resident[i].active < 0 || o.resident[i].active > 1 || o.resident[i].packet.count < 0 ||
            o.resident[i].packet.count > GWS_PACKET_SPRITES) return 0;
    return 1;
}
static int load_objects(const uint8_t *data, int len) {
    if (!validate_objects(data, len)) return 0;
    if (!len || !s_enabled) { gws_objects_reset(); return 1; }
    memcpy(&g_gws_objects, data, sizeof g_gws_objects);
    return 1;
}

NES_MOD_CONSTRUCTOR(register_gumshoe_widescreen) {
    if (!nes_mod_register_function_entry_plugin("gumshoe.widescreen.nmi", 0x8031, nmi_hook) ||
        !nes_mod_register_savestate_hook("gumshoe.widescreen.world", save_world, load_world) ||
        !nes_mod_register_savestate_validator("gumshoe.widescreen.world", validate_world) ||
        !nes_mod_register_savestate_hook("gumshoe.widescreen.objects", save_objects, load_objects) ||
        !nes_mod_register_savestate_validator("gumshoe.widescreen.objects", validate_objects))
        fprintf(stderr, "[Widescreen] Failed to register the Gumshoe widescreen hooks\n");
}
