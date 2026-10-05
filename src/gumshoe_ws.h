/*
 * gumshoe_ws.h - Gumshoe custom widescreen, shared parts (docs/WIDESCREEN.md).
 *
 * The cycle backend's custom-renderer pattern (SuperMarioBros2JapanFDSRecomp,
 * SuperMarioBros2Recomp, KirbysAdventureNESRecomp): a host-side wide nametable
 * fed by the game's own nametable writes and by the game's own column builder
 * run ahead on an isolated machine (runner/cyc/cyc_mod.h), a compositor
 * (runner/cyc/cyc_render.h) at cyc_video's width, and the native picture kept
 * authoritative inside its 256 columns. Nothing here changes the machine: the
 * Zapper still sees only the native picture, so hit detection is the game's.
 *
 * Addresses are Gumshoe (USA, Europe), PRG+CHR payload CRC32 BEB8AB01:
 * mapper 66 (32 KB PRG banks, 8 KB CHR banks), vertical mirroring, no
 * vertical scroll, a sprite HUD. Stages run in PRG banks 0-2, each with its
 * own copy of the same engine at different addresses (same RAM layout).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "cyc_core.h"
#include "cyc_mod.h"

/* ---- RAM ---- */
enum {
    RAM_ScrollX        = 0x000B, /* fine scroll X; the nametable bit is $08 bit 0 */
    RAM_PpuCtrl        = 0x0008,
    RAM_Attract        = 0x0024, /* 1: title, letter and attract play */
    RAM_StageMode      = 0x0025, /* 3: stage setup, 4: play, others: transitions */
    RAM_Program        = 0x0026, /* 1 game, 3 attract demo */
    RAM_ScreensLeft    = 0x004E, /* the builder stops at 1 (end of the stage) */
    RAM_StreamPtr      = 0x0050, /* (2) the current screen's column stream */
    RAM_ColumnPos      = 0x0052, /* metatile column, 0-31 across both nametables */
    RAM_BuildColumn    = 0x0053, /* nonzero: the builder makes one column */
    RAM_UploadCount    = 0x0054, /* NMI uploads this many buffered tile columns */
    RAM_ScreenIndex    = 0x0056,
    RAM_StreamIndex    = 0x0059,
    RAM_OamCursor      = 0x0063,
    RAM_ObjectOffset   = 0x0064,
    RAM_BuildHold      = 0x008B, /* nonzero: no column is built */
    RAM_ZapperShot     = 0x00C5,
    RAM_AttrList       = 0x0180, /* after an odd column: 8 (hi, lo, value) attribute uploads */
    RAM_Oam            = 0x0200,
    RAM_ColumnA        = 0x0530, /* PPU address (hi, lo) then 30 tiles */
    RAM_ColumnB        = 0x0550,
    RAM_Objects        = 0x0600, /* 21 slots of 12 bytes */
};

enum {
    OBJ_SLOTS = 21, OBJ_SIZE = 12,
    OBJ_TYPE = 0, OBJ_FLAGS = 7, OBJ_Y = 8, OBJ_X = 9,
    OBJ_FLAG_XHIGH = 0x80,   /* X is 256 + [9]: past the right edge, not drawn */
    OBJ_FLAG_VISIBLE = 0x10,
    STAGE_SETUP = 3, STAGE_PLAY = 4,
    HUD_FIRST_SLOT = 54,     /* timer (54-59, $D193) and shots (60-63, $D243) */
    HUD_LAST_LINE = 40,      /* both rows sit at Y 16 and 24 */
};

/* The engine copy the mapped PRG bank holds (-1: none, the title bank). */
typedef struct {
    uint16_t builder;     /* one 16-pixel column into RAM_ColumnA/B */
    uint16_t drawer;      /* one object's sprites at RAM_OamCursor */
    uint16_t animate;     /* one object's animation step (X = slot offset) */
    uint16_t update;      /* one object's type routine, movement included */
    uint32_t builder_key; /* CRC-32 of the routine's first 8 bytes */
    uint32_t drawer_key;
} GumshoeBank;
int gumshoe_ws_bank(const GumshoeBank **out);

/* The machine's CPU RAM (reads; writes go through cyc_mod_poke). */
extern const uint8_t *gumshoe_ram;
static inline uint8_t RAM(uint16_t a) { return gumshoe_ram[a & 0x7FF]; }

/* An isolated call of a program routine with X (S at $FD); false when it did
 * not return. */
bool gumshoe_call(uint16_t routine, uint8_t x);

/* ---- the mod (gumshoe_ws.c) ---- */
void gumshoe_ws_set_mod_enabled(int enabled);
void gumshoe_ws_configure(const char *aspect, const char *hud, const char *camera);
/* The wide view's left world pixel for the camera (centered, or stopped at
 * the stage's ends), keeping the native picture inside the canvas. */
int  gumshoe_ws_view_left(int cam, int width);
