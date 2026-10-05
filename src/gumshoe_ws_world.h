/*
 * gumshoe_ws_world.h - the stage's tile map in world columns.
 *
 * Gumshoe streams one 16-pixel column at a time into its two nametables, about
 * 36 pixels ahead of the screen's right edge, and never scrolls back. The wide
 * picture needs the columns the nametables already overwrote (left margin)
 * and those the game has not built yet (right margin). Both come from the
 * game itself:
 *
 *   - every CPU write to a nametable or attribute table (cyc_mod.h PPU write
 *     callback; bank independent, tile edits included) is stored at its world
 *     column, and the visible columns are re-read from CIRAM each frame;
 *   - columns ahead come from the game's own column builder (bank 0 $8435,
 *     bank 1 $8378, bank 2 $8807), run again and again from the live stream
 *     state in an isolated scope: tiles from $0532/$0552, attributes from
 *     the upload list at $0180 after each odd column. Objects that builder spawns, its stage-loop
 *     decisions and everything else it touches are discarded with the scope.
 *     A column the game later writes for real replaces the prediction (and is
 *     counted against it).
 *
 * World columns are absolute 8-pixel tile columns: 0 is the stage's first
 * (its setup builds from scroll 0), found by unwrapping the 9-bit scroll.
 * The cache is a ring of GWS_RING columns tagged with their world column.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum {
    GWS_RING = 256,          /* tile columns kept (2048 pixels); --widescreen-ring may use fewer */
    GWS_ROWS = 30,
    GWS_LOOKAHEAD = 40,      /* most metatile columns built ahead (640 pixels) */
    GWS_WORLD_VERSION = 1,
};
#define GWS_UNKNOWN_COLUMN INT32_MIN

typedef enum { GWS_SRC_NONE, GWS_SRC_VISIBLE, GWS_SRC_PPU, GWS_SRC_LOOKAHEAD } GwsSource;

typedef struct {
    int32_t  tag;            /* world tile column, or GWS_UNKNOWN_COLUMN */
    uint32_t tiles_known;    /* bit per row */
    uint32_t pal_known;
    uint32_t predicted;      /* rows whose tile came from the lookahead, unconfirmed */
    uint32_t pal_predicted;  /* rows whose palette came from the lookahead, unconfirmed */
    uint8_t  tile[GWS_ROWS];
    uint8_t  pal[GWS_ROWS];  /* palette 0-3 of each tile */
    uint8_t  source;         /* GwsSource of the latest store */
    uint8_t  pad[3];
} GwsColumn;

typedef struct {
    uint32_t version;
    int32_t  ring;           /* columns of col[] in use: 128 or GWS_RING */
    int32_t  valid;          /* a stage is cached */
    uint32_t generation;     /* counts stages begun (objects drop their residents) */
    int32_t  bank;           /* engine copy (0-2) */
    int32_t  start_known;    /* world column 0 is the stage's first (setup seen) */
    int32_t  cam;            /* the game's camera, world pixels (unwrapped $0B/$08) */
    int32_t  head;           /* world tile column of the last column the game built */
    int32_t  ahead_base;     /* head the lookahead was built from (-1: stale) */
    int32_t  ahead_count;    /* metatile columns it was asked for */
    int32_t  stage_end;      /* first world tile column past the stage, or INT32_MAX */
    int32_t  prev_mode;      /* $25 at the previous frame end */
    uint32_t lookahead_columns, lookahead_failures, lookahead_runs;
    uint32_t verified_tiles, mismatched_tiles, ppu_writes, unmapped_writes;
    int32_t  first_mismatch_column, first_mismatch_row;
    uint32_t verified_palettes, mismatched_palettes;
    int32_t  first_pal_mismatch_column, first_pal_mismatch_row, first_pal_expected, first_pal_actual;
    GwsColumn col[GWS_RING];
} GwsWorld;

extern GwsWorld g_gws_world;

void gws_world_reset(void);
/* cyc_mod_set_ppu_write_hook callback. */
void gws_world_ppu_write(unsigned reg, uint16_t addr, uint8_t value, unsigned increment);
/* Frame end (outside isolated scopes): stage tracking, visible columns,
 * lookahead. */
void gws_world_update(void);
/* A world pixel's tile and palette; false where nothing is known. */
bool gws_world_tile(int wx, int row, uint8_t *tile, uint8_t *pal);
/* An absolute world X for a 9-bit scroll value near the cached camera. */
int  gws_world_unwrap(int scroll9);
/* The stage's world pixel bounds for camera anchoring: left is 0 once the
 * setup was seen (else INT32_MIN), right the end once known (else INT32_MAX). */
void gws_world_bounds(int *left, int *right);
/* Lookahead from scratch at the next update (a loaded state, an option). */
void gws_world_invalidate_lookahead(void);
/* Diagnostics: a smaller ring (128 columns) so wraparound shows on short
 * routes at 16:9 and 21:9. Takes effect at the next stage. */
bool gws_world_set_ring(int columns);
