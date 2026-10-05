/* gumshoe_ws_world.c - the stage's tile map in world columns; see
 * gumshoe_ws_world.h and docs/WIDESCREEN.md. */
#include "gumshoe_ws_world.h"
#include "gumshoe_ws.h"

#include "cyc_render.h"
#include "cyc_video.h"

#include <limits.h>
#include <string.h>

GwsWorld g_gws_world;
#define W (&g_gws_world)

static const uint32_t ALL_ROWS = (1u << GWS_ROWS) - 1;

static int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
static int mod(int a, int m) { return ((a % m) + m) % m; }
static int scroll9(void) { return RAM(RAM_ScrollX) | ((RAM(RAM_PpuCtrl) & 1) << 8); }

int gws_world_unwrap(int s) {
    int d = mod(s - W->cam, 512);
    if (d >= 256) d -= 512;
    return W->cam + d;
}

/* The world column a nametable column (0-63: left table, right table) holds
 * when it is one of the 64 starting at world column lo. */
static int map_slot(int slot, int lo) { return lo + mod(slot - lo, 64); }

static GwsColumn *column(int abs, bool create) {
    GwsColumn *c = &W->col[mod(abs, W->ring)];
    if (c->tag != abs) {
        if (!create) return NULL;
        memset(c, 0, sizeof *c);
        c->tag = abs;
    }
    return c;
}

/* A tile the game itself put in the nametable: the truth, and a check of
 * the lookahead's prediction for it. */
static void store_real_tile(GwsColumn *c, int row, uint8_t v, GwsSource source) {
    uint32_t bit = 1u << row;
    if (c->predicted & bit) {
        if (c->tile[row] == v) W->verified_tiles++;
        else {
            if (!W->mismatched_tiles++) { W->first_mismatch_column = c->tag; W->first_mismatch_row = row; }
        }
        c->predicted &= ~bit;
    }
    c->tile[row] = v;
    c->tiles_known |= bit;
    c->source = (uint8_t)source;
}

static void store_real_palette(GwsColumn *c, int row, uint8_t v) {
    uint32_t bit = 1u << row;
    if (c->pal_predicted & bit) {
        if (c->pal[row] == v) W->verified_palettes++;
        else if (!W->mismatched_palettes++) {
            W->first_pal_mismatch_column = c->tag;
            W->first_pal_mismatch_row = row;
            W->first_pal_expected = c->pal[row];
            W->first_pal_actual = v;
        }
        c->pal_predicted &= ~bit;
    }
    c->pal[row] = v;
    c->pal_known |= bit;
}

static void store_attribute(int nt, int index, uint8_t value, int lo) {
    int ax = index & 7, ay = index >> 3;
    for (int dx = 0; dx < 4; dx++) {
        GwsColumn *c = column(map_slot(nt * 32 + ax * 4 + dx, lo), true);
        for (int dy = 0; dy < 4; dy++) {
            int row = ay * 4 + dy;
            if (row >= GWS_ROWS) break;
            store_real_palette(c, row, (uint8_t)((value >> (((dy >> 1) << 2) | ((dx >> 1) << 1))) & 3));
        }
    }
}

void gws_world_ppu_write(unsigned reg, uint16_t addr, uint8_t value, unsigned increment) {
    (void)increment;
    if (reg != 7 || !W->valid || addr < 0x2000 || addr >= 0x3000) return;
    W->ppu_writes++;
    /* NMI uploads what the frame's logic built for the camera it just set:
     * the written column is within a screen of that camera. */
    int lo = floor_div(gws_world_unwrap(scroll9()), 8) - 16;
    int nt = (addr >> 10) & 1, off = addr & 0x3FF;
    if (off < 960) store_real_tile(column(map_slot(nt * 32 + (off & 31), lo), true), off >> 5, value, GWS_SRC_PPU);
    else store_attribute(nt, off - 960, value, lo);
}

/* World columns [first, first + count) from the nametables as they are now. */
static void copy_nametables(int first, int count) {
    for (int abs = first; abs < first + count; abs++) {
        int slot = mod(abs, 64), base = 0x2000 + (slot >> 5) * 0x400, x = slot & 31;
        GwsColumn *c = column(abs, true);
        for (int row = 0; row < GWS_ROWS; row++) {
            store_real_tile(c, row, cyc_render_nametable((uint16_t)(base + row * 32 + x)), GWS_SRC_VISIBLE);
            uint8_t a = cyc_render_nametable((uint16_t)(base + 960 + (row >> 2) * 8 + (x >> 2)));
            store_real_palette(c, row, (uint8_t)((a >> ((((row >> 1) & 1) << 2) | (((x >> 1) & 1) << 1))) & 3));
        }
    }
}

static int s_ring = GWS_RING;

bool gws_world_set_ring(int columns) {
    if (columns != 128 && columns != GWS_RING) return false;
    s_ring = columns;
    return true;
}

void gws_world_reset(void) {
    memset(W, 0, sizeof *W);
    W->version = GWS_WORLD_VERSION;
    W->ring = s_ring;
    W->bank = -1;
    W->ahead_base = -1;
    W->stage_end = INT32_MAX;
    W->first_mismatch_column = GWS_UNKNOWN_COLUMN;
    W->first_mismatch_row = -1;
    W->first_pal_mismatch_column = GWS_UNKNOWN_COLUMN;
    W->first_pal_mismatch_row = -1;
    W->prev_mode = -1;
    for (int i = 0; i < GWS_RING; i++) W->col[i].tag = GWS_UNKNOWN_COLUMN;
}

void gws_world_invalidate_lookahead(void) { W->ahead_base = -1; }

/* The world column of the last column the builder made: its second tile
 * column's PPU address, near the camera. */
static bool builder_head(int *head) {
    uint8_t hi = RAM(RAM_ColumnB), lo = RAM(RAM_ColumnB + 1);
    if (hi < 0x20 || hi > 0x2F || (lo & 0xE0)) return false; /* the builder writes row 0 */
    *head = map_slot(((hi >> 2) & 1) * 32 + (lo & 31), floor_div(W->cam, 8) - 16);
    return true;
}

static void begin_stage(int bank, bool setup) {
    uint32_t generation = W->generation;
    gws_world_reset();
    W->generation = generation + 1;
    W->valid = 1;
    W->bank = bank;
    W->start_known = setup;
    W->cam = scroll9();
    W->prev_mode = RAM(RAM_StageMode);
    int head;
    if (!builder_head(&head)) head = floor_div(W->cam, 8) + 32;
    W->head = head;
    /* The setup builds the first columns from scroll 0 (18 at once); mid-stage
     * (the mod enabled during play) only the visible and built-ahead columns
     * are certainly this stage's. */
    int first = setup ? head - 63 : floor_div(W->cam, 8);
    if (setup && first < 0) first = 0;
    copy_nametables(first, head - first + 1);
}

/* Columns past the last one built: the game's own builder, again and again,
 * on an isolated machine (cyc_mod.h). Real columns are never replaced. */
static void lookahead(const GumshoeBank *b, int head, int count) {
    W->ahead_base = head;
    W->ahead_count = count;
    W->lookahead_runs++;
    int last = head;
    bool ended = false;
    if (count > 0 && cyc_mod_isolate_begin()) {
        for (int k = 0; k < count; k++) {
            if (cyc_mod_peek(RAM_ScreensLeft) == 1) { ended = true; break; }
            cyc_mod_poke(RAM_BuildColumn, 1);
            cyc_mod_poke(RAM_BuildHold, 0);
            if (!gumshoe_call(b->builder, 0)) { W->lookahead_failures++; break; }
            if (cyc_mod_peek(RAM_BuildColumn)) { ended = true; break; } /* it declined */
            int a = last + 1, bcol = last + 2;
            uint8_t ahi = cyc_mod_peek(RAM_ColumnA), alo = cyc_mod_peek(RAM_ColumnA + 1);
            uint8_t bhi = cyc_mod_peek(RAM_ColumnB), blo = cyc_mod_peek(RAM_ColumnB + 1);
            if (((ahi >> 2) & 1) * 32 + (alo & 31) != mod(a, 64) || ((bhi >> 2) & 1) * 32 + (blo & 31) != mod(bcol, 64)) {
                W->lookahead_failures++;
                break;
            }
            for (int side = 0; side < 2; side++) {
                GwsColumn *c = column(side ? bcol : a, true);
                if (c->tiles_known & ~c->predicted) continue; /* the game already wrote it */
                uint16_t src = (uint16_t)((side ? RAM_ColumnB : RAM_ColumnA) + 2);
                for (int row = 0; row < GWS_ROWS; row++) c->tile[row] = cyc_mod_peek((uint16_t)(src + row));
                c->tiles_known = c->predicted = ALL_ROWS;
                c->source = GWS_SRC_LOOKAHEAD;
            }
            if (mod(bcol, 4) == 3) {
                /* An odd metatile column completes a 32-pixel attribute column:
                 * the eight (address, value) entries NMI will upload. They are
                 * the truth, not $01A0: the list's eleventh entry overruns into
                 * $01A0[0] after the first entry copied it. */
                for (int i = 0; i < 8; i++) {
                    uint16_t at = (uint16_t)(RAM_AttrList + 3 * i);
                    uint16_t addr = (uint16_t)(cyc_mod_peek(at) << 8 | cyc_mod_peek((uint16_t)(at + 1)));
                    uint8_t v = cyc_mod_peek((uint16_t)(at + 2));
                    int off = addr & 0x3FF, index = off - 960;
                    if (addr < 0x2000 || addr >= 0x3000 || off < 960) continue;
                    int nt = (addr >> 10) & 1, ax = index & 7, ay = index >> 3;
                    for (int dx = 0; dx < 4; dx++) {
                        GwsColumn *c = column(map_slot(nt * 32 + ax * 4 + dx, bcol - 31), false);
                        if (!c || c->source != GWS_SRC_LOOKAHEAD) continue;
                        for (int dy = 0; dy < 4; dy++) {
                            int row = ay * 4 + dy;
                            if (row >= GWS_ROWS) break;
                            c->pal[row] = (uint8_t)((v >> (((dy >> 1) << 2) | ((dx >> 1) << 1))) & 3);
                            c->pal_known |= 1u << row;
                            c->pal_predicted |= 1u << row;
                        }
                    }
                }
            }
            last = bcol;
            W->lookahead_columns++;
        }
        cyc_mod_isolate_end();
    } else if (count > 0) {
        W->lookahead_failures++;
    }
    /* A short run that did not reach the end leaves the end where it was. */
    if (ended) W->stage_end = last + 1;
    else if (last >= W->stage_end) W->stage_end = INT32_MAX;
    /* Predictions past the new end are no longer the game's future. */
    for (int abs = last + 1; abs <= head + 2 * GWS_LOOKAHEAD + 2; abs++) {
        GwsColumn *c = column(abs, false);
        if (c && c->source == GWS_SRC_LOOKAHEAD) c->tag = GWS_UNKNOWN_COLUMN;
    }
}

void gws_world_update(void) {
    const GumshoeBank *b;
    int bank = gumshoe_ws_bank(&b), mode = RAM(RAM_StageMode);
    if (bank < 0) {
        if (W->valid) {
            uint32_t generation = W->generation;
            gws_world_reset();
            W->generation = generation + 1;
        }
        W->prev_mode = mode;
        return;
    }
    if (mode == STAGE_SETUP && W->prev_mode != STAGE_SETUP) begin_stage(bank, true);
    else if ((!W->valid || W->bank != bank) && (mode == STAGE_SETUP || mode == STAGE_PLAY)) begin_stage(bank, false);
    W->prev_mode = mode;
    if (!W->valid) return;
    W->cam = gws_world_unwrap(scroll9());
    /* The picture just drawn shows these columns: they are current. */
    if (cyc_render_line_mask(120) & 0x08) copy_nametables(floor_div(gws_world_unwrap(cyc_render_line_scroll_x(120)), 8), 33);
    int head;
    if (builder_head(&head)) W->head = head;
    /* As far as the widest view of this camera reaches (the anchored view at
     * the stage's start shows the most), plus a column. */
    int width = cyc_video_width(), right = gumshoe_ws_view_left(W->cam, width) + width;
    int count = (right - (W->head + 1) * 8 + 15) / 16 + 1;
    if (count < 0) count = 0;
    if (count > GWS_LOOKAHEAD) count = GWS_LOOKAHEAD;
    if (W->ring < GWS_RING && count > (W->ring - 48) / 2) count = (W->ring - 48) / 2;
    if (W->head != W->ahead_base || count > W->ahead_count) lookahead(b, W->head, count);
}

bool gws_world_tile(int wx, int row, uint8_t *tile, uint8_t *pal) {
    if (!W->valid || row < 0 || row >= GWS_ROWS) return false;
    int abs = floor_div(wx, 8);
    if (abs >= W->stage_end || (W->start_known && abs < 0)) return false;
    const GwsColumn *c = &W->col[mod(abs, W->ring)];
    if (c->tag != abs || !(c->tiles_known >> row & 1)) return false;
    *tile = c->tile[row];
    *pal = (c->pal_known >> row & 1) ? c->pal[row] : 0;
    return true;
}

void gws_world_bounds(int *left, int *right) {
    *left = W->start_known ? 0 : INT_MIN;
    *right = W->stage_end != INT32_MAX ? W->stage_end * 8 : INT_MAX;
}
