/*
 * gumshoe_ws_objects.h - complete sprites for objects at the native edges.
 *
 * The game keeps an object's screen X in 8 bits plus a high flag (object byte
 * 7 bit 7): it spawns objects about 36 pixels past the right edge and drops
 * them at the left edge. Its drawer (bank 0 $964A, bank 1 $9600, bank 2
 * $99F0) skips flagged objects and hides any sprite that would wrap, so the
 * native picture shows none of a flagged object and only part of one
 * crossing an edge. At each NMI (the hook site gumshoe.widescreen.nmi, where
 * the OAM about to be shown matches the object table) the drawer runs on an
 * isolated machine for every such object with its X moved on screen; the
 * sprites it writes, moved back, are the object's complete packet. Margins
 * draw them; inside the native columns the native picture stays as is.
 *
 * An object the game drops at the left edge (each type's own check, e.g. a
 * balloon at X-high and X >= $F0, counted as missed) lives on as a resident
 * for the picture only: its last 12 bytes, stepped every frame by the game's
 * own animation and type routines (bank 0 $94CB / $9495, bank 1 $94F9 /
 * $94C3, bank 2 $98AF / $9861: its own movement) at X 128 in a free slot of
 * an isolated machine, moved against the camera as the scroll routine ($8CAE
 * moves every object by the scroll step) would, and drawn the same way, until
 * it leaves the widest view or its routine ends it. Nothing
 * of it reaches the game: activation, movement, collisions and the Zapper's
 * targets are the game's own.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "gumshoe_ws.h"

enum { GWS_PACKET_SPRITES = 32, GWS_RESIDENTS = 16, GWS_OBJECTS_VERSION = 2 };

typedef struct {
    int16_t x;               /* screen X relative to native column 0 */
    uint8_t y, tile, attr, pad;
} GwsSprite;

typedef struct {
    int32_t   count;
    GwsSprite sprite[GWS_PACKET_SPRITES];
} GwsPacket;

typedef struct {
    int32_t   active;
    int32_t   x;               /* screen X relative to native column 0 */
    uint8_t   data[OBJ_SIZE];  /* the object's bytes, X at 128 */
    GwsPacket packet;
} GwsResident;

typedef struct {
    uint32_t    version;
    int32_t     valid;
    GwsPacket   packet[OBJ_SLOTS];
    uint8_t     prev[OBJ_SLOTS * OBJ_SIZE];  /* the table at the previous NMI */
    int32_t     prev_valid;
    uint32_t    generation;                  /* the world's, when prev was taken */
    int32_t     cam;                         /* the game's camera then (world pixels) */
    GwsResident resident[GWS_RESIDENTS];
    uint32_t    captures, failures, edge_objects, residents_started, resident_steps;
} GwsObjects;

extern GwsObjects g_gws_objects;

void gws_objects_reset(void);
/* At NMI entry (outside the compositor): capture the packets. */
void gws_objects_capture(void);
/* Into the wide canvas at native origin x0: the margins and native columns
 * 0-7 (where the drawer's hidden wrapping sprites and the left clip fall). */
void gws_objects_draw(uint32_t *out, int width, int height, int x0, const uint8_t *bg_opaque);
int  gws_objects_json(char *buf, int cap);
