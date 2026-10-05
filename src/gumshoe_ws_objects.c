/* gumshoe_ws_objects.c - complete sprites for objects at the native edges;
 * see gumshoe_ws_objects.h. */
#include "gumshoe_ws_objects.h"
#include "gumshoe_ws_world.h"

#include "cyc_render.h"
#include "cyc_video.h"

#include <stdio.h>
#include <string.h>

GwsObjects g_gws_objects;
#define O (&g_gws_objects)

enum { EDGE = 32, SHIFTED_X = 128, OAM_SPRITES = 43 /* the drawer stops at cursor $AC */ };

void gws_objects_reset(void) {
    memset(O, 0, sizeof *O);
    O->version = GWS_OBJECTS_VERSION;
}

/* The flag means "off the screen" on either side: objects enter from the
 * right at 256 + X and moving left clear it at X 255; one that leaves at the
 * left sets it at X $FF and is dropped by its own update at $F0 and above. */
static int decode_x(uint8_t x, uint8_t flags) {
    if (!(flags & OBJ_FLAG_XHIGH)) return x;
    return x < 0xC0 ? 256 + x : x - 256;
}
static int screen_x(const uint8_t *o) { return decode_x(o[OBJ_X], o[OBJ_FLAGS]); }

/* The drawer for the object at slot offset `off` with X 128: its sprites,
 * moved to screen X `x`, into *p. Inside an isolated scope. */
static bool draw_packet(const GumshoeBank *b, int off, int x, GwsPacket *p) {
    for (int s = 0; s < OAM_SPRITES; s++) cyc_mod_poke((uint16_t)(RAM_Oam + s * 4), 0xF4);
    cyc_mod_poke(RAM_OamCursor, 0);
    cyc_mod_poke(RAM_ObjectOffset, (uint8_t)off);
    if (!gumshoe_call(b->drawer, (uint8_t)off)) return false;
    int used = cyc_mod_peek(RAM_OamCursor) / 4;
    p->count = 0;
    for (int s = 0; s < used && s < OAM_SPRITES && p->count < GWS_PACKET_SPRITES; s++) {
        uint16_t at = (uint16_t)(RAM_Oam + s * 4);
        uint8_t y = cyc_mod_peek(at);
        if (y >= 0xEF) continue;
        GwsSprite *v = &p->sprite[p->count++];
        v->y = y;
        v->tile = cyc_mod_peek((uint16_t)(at + 1));
        v->attr = cyc_mod_peek((uint16_t)(at + 2));
        v->x = (int16_t)(cyc_mod_peek((uint16_t)(at + 3)) - SHIFTED_X + x);
    }
    return true;
}

static void put_object(int off, const uint8_t *data) {
    for (int i = 0; i < OBJ_SIZE; i++) cyc_mod_poke((uint16_t)(RAM_Objects + off + i), data[i]);
}

/* One frame of a resident: the game's animation and type routines in a free
 * slot, at X 128 so no edge check ends it; its own motion and the camera's
 * step (scroll) move r->x. */
static void step_resident(const GumshoeBank *b, GwsResident *r, int scroll) {
    int off = -1;
    for (int slot = 0; slot < OBJ_SLOTS && off < 0; slot++)
        if (!cyc_mod_peek((uint16_t)(RAM_Objects + slot * OBJ_SIZE + OBJ_TYPE))) off = slot * OBJ_SIZE;
    if (off < 0) return; /* a full table: hold the resident this frame */
    put_object(off, r->data);
    cyc_mod_poke(RAM_ObjectOffset, (uint8_t)off);
    if (!gumshoe_call(b->animate, (uint8_t)off) || !gumshoe_call(b->update, (uint8_t)off)) {
        O->failures++;
        r->active = 0;
        return;
    }
    uint8_t data[OBJ_SIZE];
    for (int i = 0; i < OBJ_SIZE; i++) data[i] = cyc_mod_peek((uint16_t)(RAM_Objects + off + i));
    O->resident_steps++;
    if (!data[OBJ_TYPE]) { r->active = 0; return; }
    r->x += decode_x(data[OBJ_X], data[OBJ_FLAGS]) - SHIFTED_X - scroll;
    data[OBJ_X] = SHIFTED_X;
    data[OBJ_FLAGS] &= (uint8_t)~OBJ_FLAG_XHIGH;
    memcpy(r->data, data, sizeof data);
    put_object(off, data);
    if (r->x < -CYC_VIDEO_MAX_WIDTH || r->x > 256 + CYC_VIDEO_MAX_WIDTH || !(data[OBJ_FLAGS] & OBJ_FLAG_VISIBLE)) {
        r->active = 0;
        return;
    }
    if (!draw_packet(b, off, r->x, &r->packet)) { O->failures++; r->active = 0; }
}

/* Objects the game dropped since the previous NMI just past the left edge
 * (each type's own check), continued as residents. A slot that was emptied
 * or taken by a new object counts; anything else is the same object. */
static void start_residents(void) {
    const uint8_t *table = &gumshoe_ram[RAM_Objects];
    for (int slot = 0; slot < OBJ_SLOTS; slot++) {
        const uint8_t *p = &O->prev[slot * OBJ_SIZE], *o = &table[slot * OBJ_SIZE];
        if (!p[OBJ_TYPE] || !(p[OBJ_FLAGS] & OBJ_FLAG_VISIBLE) || (p[OBJ_FLAGS] & OBJ_FLAG_XHIGH)) continue;
        int x = p[OBJ_X];
        if (x >= EDGE / 2) continue;
        if (o[OBJ_TYPE] && o[OBJ_TYPE] == p[OBJ_TYPE] && !(o[2] == 0 && p[2] != 0)) continue;
        int n;
        for (n = 0; n < GWS_RESIDENTS && O->resident[n].active; n++) {}
        if (n == GWS_RESIDENTS) return;
        GwsResident *r = &O->resident[n];
        memset(r, 0, sizeof *r);
        r->active = 1;
        r->x = x;
        memcpy(r->data, p, OBJ_SIZE);
        r->data[OBJ_X] = SHIFTED_X;
        O->residents_started++;
    }
}

void gws_objects_capture(void) {
    const GumshoeBank *b;
    memset(O->packet, 0, sizeof O->packet);
    O->valid = 0;
    if (gumshoe_ws_bank(&b) < 0 || !g_gws_world.valid) {
        memset(O->resident, 0, sizeof O->resident);
        O->prev_valid = 0;
        return;
    }
    O->valid = 1;
    if (O->generation != g_gws_world.generation) {
        memset(O->resident, 0, sizeof O->resident);
        O->prev_valid = 0;
        O->generation = g_gws_world.generation;
    }
    int cam = gws_world_unwrap(RAM(RAM_ScrollX) | ((RAM(RAM_PpuCtrl) & 1) << 8));
    int scroll = O->prev_valid ? cam - O->cam : 0;
    O->cam = cam;
    if (O->prev_valid) start_residents();
    memcpy(O->prev, &gumshoe_ram[RAM_Objects], sizeof O->prev);
    O->prev_valid = 1;
    int pending[OBJ_SLOTS], n = 0, residents = 0;
    for (int slot = 0; slot < OBJ_SLOTS; slot++) {
        const uint8_t *o = &gumshoe_ram[RAM_Objects + slot * OBJ_SIZE];
        if (!o[OBJ_TYPE] || !(o[OBJ_FLAGS] & OBJ_FLAG_VISIBLE)) continue;
        int x = screen_x(o);
        if (x < EDGE || x >= 256 - EDGE) pending[n++] = slot;
    }
    for (int i = 0; i < GWS_RESIDENTS; i++) residents += O->resident[i].active;
    O->edge_objects = (uint32_t)n;
    if (!n && !residents) return;
    if (!cyc_mod_isolate_begin()) { O->failures++; return; }
    for (int i = 0; i < n; i++) {
        int slot = pending[i], base = RAM_Objects + slot * OBJ_SIZE;
        int x = decode_x(cyc_mod_peek((uint16_t)(base + OBJ_X)), cyc_mod_peek((uint16_t)(base + OBJ_FLAGS)));
        cyc_mod_poke((uint16_t)(base + OBJ_X), SHIFTED_X);
        cyc_mod_poke((uint16_t)(base + OBJ_FLAGS), (uint8_t)(cyc_mod_peek((uint16_t)(base + OBJ_FLAGS)) & ~OBJ_FLAG_XHIGH));
        if (draw_packet(b, slot * OBJ_SIZE, x, &O->packet[slot])) O->captures++;
        else O->failures++;
    }
    for (int i = 0; i < GWS_RESIDENTS; i++)
        if (O->resident[i].active) step_resident(b, &O->resident[i], scroll);
    cyc_mod_isolate_end();
}

void gws_objects_draw(uint32_t *out, int width, int height, int x0, const uint8_t *bg_opaque) {
    if (!O->valid) return;
    bool tall = cyc_render_line_sprite16(120);
    uint16_t table = cyc_render_line_sprite_table(120);
    int rows = tall ? 16 : 8;
    /* Residents under the table's objects; the drawer's order (lower slots
     * on top, like OAM) within each. */
    for (int k = GWS_RESIDENTS + OBJ_SLOTS - 1; k >= 0; k--) {
        const GwsPacket *p = k >= OBJ_SLOTS ? (O->resident[k - OBJ_SLOTS].active ? &O->resident[k - OBJ_SLOTS].packet : NULL)
                                            : &O->packet[k];
        if (!p) continue;
        for (int i = p->count - 1; i >= 0; i--) {
            const GwsSprite *v = &p->sprite[i];
            int top = v->y + 1;
            for (int r = 0; r < rows; r++) {
                int py = top + r;
                if (py < 0 || py >= height) continue;
                if (!(cyc_render_line_mask(py) & 0x10)) continue;
                int row = (v->attr & 0x80) ? rows - 1 - r : r;
                uint16_t pattern = tall ? (uint16_t)(((v->tile & 1) ? 0x1000 : 0) + (v->tile & 0xFE) * 16 + (row >= 8 ? 16 : 0))
                                        : (uint16_t)(table + v->tile * 16);
                uint8_t lo = cyc_render_chr((uint16_t)(pattern + (row & 7)));
                uint8_t hi = cyc_render_chr((uint16_t)(pattern + (row & 7) + 8));
                for (int c = 0; c < 8; c++) {
                    int nx = v->x + c, px = x0 + nx;
                    /* Margins, and the native left strip: the drawer hides a
                     * sprite whose X wraps, so its columns 0-7 are missing
                     * there too (any the picture has are the same pixels). */
                    if (px < 0 || px >= width || (nx >= 8 && nx < 256)) continue;
                    int bit = (v->attr & 0x40) ? c : 7 - c;
                    int pixel = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
                    if (!pixel) continue;
                    size_t at = (size_t)py * (size_t)width + (size_t)px;
                    if ((v->attr & 0x20) && bg_opaque && bg_opaque[at]) continue;
                    out[at] = cyc_render_color(16 + (v->attr & 3) * 4 + pixel);
                }
            }
        }
    }
}

int gws_objects_json(char *buf, int cap) {
    int residents = 0;
    for (int i = 0; i < GWS_RESIDENTS; i++) residents += O->resident[i].active;
    int n = snprintf(buf, (size_t)cap,
                     "\"objects\":{\"valid\":%d,\"edge_objects\":%u,\"captures\":%u,\"failures\":%u,\"residents\":%d,"
                     "\"residents_started\":%u,\"resident_steps\":%u,\"resident_x\":[",
                     O->valid, O->edge_objects, O->captures, O->failures, residents, O->residents_started, O->resident_steps);
    bool any = false;
    for (int i = 0; i < GWS_RESIDENTS && n < cap - 32; i++)
        if (O->resident[i].active) { n += snprintf(buf + n, (size_t)(cap - n), "%s%d", any ? "," : "", O->resident[i].x); any = true; }
    n += snprintf(buf + n, (size_t)(cap - n), "],\"packets\":[");
    bool first = true;
    for (int slot = 0; slot < OBJ_SLOTS && n < cap - 64; slot++) {
        const GwsPacket *p = &O->packet[slot];
        if (!p->count) continue;
        int lo = p->sprite[0].x, hi = lo;
        for (int i = 1; i < p->count; i++) {
            if (p->sprite[i].x < lo) lo = p->sprite[i].x;
            if (p->sprite[i].x > hi) hi = p->sprite[i].x;
        }
        n += snprintf(buf + n, (size_t)(cap - n), "%s{\"slot\":%d,\"sprites\":%d,\"x\":[%d,%d]}", first ? "" : ",", slot,
                      p->count, lo, hi + 8);
        first = false;
    }
    n += snprintf(buf + n, (size_t)(cap - n), "]}");
    return n;
}
