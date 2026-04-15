/*
 * extras.c — Gumshoe game-specific runner hooks
 * Implements game_extras.h.
 * Features: TCP debug server, verify mode via Nestopia oracle.
 */
#include "game_extras.h"
#include "nes_runtime.h"
#include "debug_server.h"
#include "verify_mode.h"
#ifdef ENABLE_NESTOPIA_ORACLE
#include "nestopia_bridge.h"
#endif
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* Globals expected by the runner framework */
const char *g_rom_path_for_extras = NULL;
int         g_watchdog_triggered  = 0;
uint32_t    g_watchdog_frame      = 0;
const char *g_watchdog_stack_dump = "";

static void gumshoe_pack_game_data(uint8_t out[32]) {
    memset(out, 0, 32);

    out[0x00] = g_ram[0x24];
    out[0x01] = g_ram[0x25];
    out[0x02] = g_ram[0x26];
    out[0x03] = g_ram[0x2C];
    out[0x04] = g_ram[0x38];
    out[0x05] = g_ram[0x4F];
    out[0x06] = g_ram[0x53];
    out[0x07] = g_ram[0x84];
    out[0x08] = g_ram[0x8A];
    out[0x09] = g_ram[0x8B];
    out[0x0A] = g_ram[0xC5];
    out[0x0B] = g_ram[0xC7];
    out[0x0C] = g_ram[0xCD];
    out[0x0D] = g_ram[0xD2];
    out[0x0E] = g_ram[0xD3];
    out[0x0F] = g_ram[0xD4];
    out[0x10] = g_ram[0xDB];
    out[0x11] = g_ram[0xE3];
    out[0x12] = g_ram[0xE4];
    out[0x13] = g_ram[0xE7];
    memcpy(&out[0x14], &g_ram[0x0600], 6);
    memcpy(&out[0x1A], &g_ram[0x060C], 6);
}

static int gumshoe_parse_int_arg(const char *json, const char *key) {
    const char *p;
    char pattern[32];

    if (!json) return -1;
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    p = strstr(json, pattern);
    if (!p) return -1;
    p = strchr(p, ':');
    if (!p) return -1;
    p++;
    while (*p == ' ' || *p == '"') p++;
    return atoi(p);
}

static void gumshoe_add_default_followers(void) {
    static const uint16_t addrs[] = {
        0x001E, 0x0024, 0x0025, 0x0026, 0x0027, 0x0028, 0x0029, 0x002C, 0x002E, 0x002F,
        0x0038, 0x004E, 0x004F, 0x0053, 0x006A, 0x0070, 0x0084, 0x008A, 0x008B,
        0x00C5, 0x00C7, 0x00C9, 0x00CD, 0x00D2, 0x00D3, 0x00D4, 0x00D9, 0x00DB,
        0x00E3, 0x00E4, 0x00E7
    };
    size_t i;

    for (i = 0; i < sizeof(addrs) / sizeof(addrs[0]); i++) {
        debug_server_add_follower(addrs[i], -1);
    }
}

static void gumshoe_append_json(char **dst, size_t *remaining, const char *fmt, ...) {
    va_list args;
    int written;

    if (!dst || !*dst || !remaining || *remaining == 0) {
        return;
    }

    va_start(args, fmt);
    written = vsnprintf(*dst, *remaining, fmt, args);
    va_end(args);

    if (written < 0) {
        **dst = '\0';
        *remaining = 0;
        return;
    }

    if ((size_t)written >= *remaining) {
        *dst += *remaining - 1;
        *remaining = 1;
        return;
    }

    *dst += written;
    *remaining -= (size_t)written;
}

static void gumshoe_send_window_json(int id, uint32_t center, int before, int after) {
    char json[32768];
    char *p = json;
    size_t remaining = sizeof(json);
    uint32_t first;
    uint32_t last;
    uint32_t f;
    int emitted = 0;

    if (before < 0) before = 0;
    if (after < 0) after = 0;

    first = (center > (uint32_t)before) ? (center - (uint32_t)before) : 0;
    last = center + (uint32_t)after;

    gumshoe_append_json(&p, &remaining,
        "{\"id\":%d,\"ok\":true,\"center\":%u,\"first\":%u,\"last\":%u,\"frames\":[",
        id, center, first, last);

    for (f = first; f <= last; f++) {
        const NESFrameRecord *r = debug_server_get_frame_record(f);
        uint8_t sel = 0;
        int sel_valid = 0;
        uint16_t sel_base = 0;

        if (!r) {
            continue;
        }

        sel = r->ram_full[0x0064];
        if (sel >= 0x00 && sel <= 0xF0 && (sel % 0x0C) == 0) {
            sel_base = (uint16_t)(0x0600 + sel);
            if (sel_base + 11 < 0x0800) {
                sel_valid = 1;
            }
        }

        if (emitted) {
            gumshoe_append_json(&p, &remaining, ",");
        }
        emitted = 1;

        gumshoe_append_json(&p, &remaining,
            "{\"frame\":%u,\"bank\":%d,\"buttons1\":\"0x%02X\","
            "\"ram\":{\"1E\":\"0x%02X\",\"24\":\"0x%02X\",\"25\":\"0x%02X\",\"26\":\"0x%02X\","
                    "\"27\":\"0x%02X\",\"28\":\"0x%02X\",\"29\":\"0x%02X\",\"2C\":\"0x%02X\","
                    "\"2E\":\"0x%02X\",\"2F\":\"0x%02X\",\"38\":\"0x%02X\",\"4E\":\"0x%02X\","
                    "\"4F\":\"0x%02X\",\"53\":\"0x%02X\",\"6A\":\"0x%02X\",\"70\":\"0x%02X\","
                    "\"84\":\"0x%02X\",\"8A\":\"0x%02X\",\"8B\":\"0x%02X\",\"C5\":\"0x%02X\","
                    "\"C7\":\"0x%02X\",\"C9\":\"0x%02X\",\"CD\":\"0x%02X\",\"D2\":\"0x%02X\","
                    "\"D3\":\"0x%02X\",\"D4\":\"0x%02X\",\"D9\":\"0x%02X\",\"DB\":\"0x%02X\","
                    "\"E3\":\"0x%02X\",\"E4\":\"0x%02X\",\"E7\":\"0x%02X\","
                    "\"00\":\"0x%02X\",\"01\":\"0x%02X\",\"63\":\"0x%02X\",\"64\":\"0x%02X\"},"
            "\"ppu\":{\"ctrl\":\"0x%02X\",\"mask\":\"0x%02X\",\"status\":\"0x%02X\","
                    "\"scroll_x\":%u,\"scroll_y\":%u,\"hud_ctrl\":\"0x%02X\","
                    "\"hud_sx\":%u,\"hud_sy\":%u,\"spr0_active\":%d,\"spr0_reads\":%d},"
            "\"pal\":{\"00\":\"0x%02X\",\"01\":\"0x%02X\",\"02\":\"0x%02X\",\"03\":\"0x%02X\","
                    "\"10\":\"0x%02X\",\"11\":\"0x%02X\",\"12\":\"0x%02X\",\"13\":\"0x%02X\"},"
            "\"ent0\":{\"b0\":\"0x%02X\",\"b1\":\"0x%02X\",\"b2\":\"0x%02X\",\"b7\":\"0x%02X\",\"b8\":\"0x%02X\",\"b9\":\"0x%02X\"},"
            "\"ent1\":{\"b0\":\"0x%02X\",\"b1\":\"0x%02X\",\"b2\":\"0x%02X\",\"b7\":\"0x%02X\",\"b8\":\"0x%02X\",\"b9\":\"0x%02X\"},"
            "\"selected\":{\"valid\":%s,\"slot\":%d,\"base\":\"0x%04X\",\"b0\":\"0x%02X\",\"b1\":\"0x%02X\",\"b2\":\"0x%02X\",\"b7\":\"0x%02X\",\"b8\":\"0x%02X\",\"b9\":\"0x%02X\"}}",
            r->frame_number, r->current_bank, r->controller1_buttons,
            r->ram_full[0x001E], r->ram_full[0x0024], r->ram_full[0x0025], r->ram_full[0x0026],
            r->ram_full[0x0027], r->ram_full[0x0028], r->ram_full[0x0029], r->ram_full[0x002C],
            r->ram_full[0x002E], r->ram_full[0x002F], r->ram_full[0x0038], r->ram_full[0x004E],
            r->ram_full[0x004F], r->ram_full[0x0053], r->ram_full[0x006A], r->ram_full[0x0070],
            r->ram_full[0x0084], r->ram_full[0x008A], r->ram_full[0x008B], r->ram_full[0x00C5],
            r->ram_full[0x00C7], r->ram_full[0x00C9], r->ram_full[0x00CD], r->ram_full[0x00D2],
            r->ram_full[0x00D3], r->ram_full[0x00D4], r->ram_full[0x00D9], r->ram_full[0x00DB],
            r->ram_full[0x00E3], r->ram_full[0x00E4], r->ram_full[0x00E7],
            r->ram_full[0x0000], r->ram_full[0x0001], r->ram_full[0x0063], r->ram_full[0x0064],
            r->ppuctrl, r->ppumask, r->ppustatus,
            r->ppuscroll_x, r->ppuscroll_y, r->ppuctrl_hud,
            r->ppuscroll_x_hud, r->ppuscroll_y_hud, r->spr0_split_active, r->spr0_reads_ctr,
            r->ppu_pal[0x00], r->ppu_pal[0x01], r->ppu_pal[0x02], r->ppu_pal[0x03],
            r->ppu_pal[0x10], r->ppu_pal[0x11], r->ppu_pal[0x12], r->ppu_pal[0x13],
            r->ram_full[0x0600], r->ram_full[0x0601], r->ram_full[0x0602], r->ram_full[0x0607], r->ram_full[0x0608], r->ram_full[0x0609],
            r->ram_full[0x060C], r->ram_full[0x060D], r->ram_full[0x060E], r->ram_full[0x0613], r->ram_full[0x0614], r->ram_full[0x0615],
            sel_valid ? "true" : "false",
            sel_valid ? (int)(sel / 0x0C) : -1,
            sel_valid ? sel_base : 0,
            sel_valid ? r->ram_full[sel_base + 0] : 0,
            sel_valid ? r->ram_full[sel_base + 1] : 0,
            sel_valid ? r->ram_full[sel_base + 2] : 0,
            sel_valid ? r->ram_full[sel_base + 7] : 0,
            sel_valid ? r->ram_full[sel_base + 8] : 0,
            sel_valid ? r->ram_full[sel_base + 9] : 0);
    }

    gumshoe_append_json(&p, &remaining, "]}");
    debug_server_send_line(json);
}

static void gumshoe_send_oam_json(int id, uint32_t frame) {
    char json[32768];
    char *p = json;
    size_t remaining = sizeof(json);
    const NESFrameRecord *r = debug_server_get_frame_record(frame);
    int emitted = 0;
    int i;

    if (!r) {
        debug_server_send_fmt("{\"id\":%d,\"ok\":false,\"err\":\"frame not in buffer\"}", id);
        return;
    }

    gumshoe_append_json(&p, &remaining,
        "{\"id\":%d,\"ok\":true,\"frame\":%u,\"sprites\":[",
        id, r->frame_number);

    for (i = 0; i < 64; i++) {
        uint8_t y = r->oam[i * 4 + 0];
        uint8_t tile = r->oam[i * 4 + 1];
        uint8_t attr = r->oam[i * 4 + 2];
        uint8_t x = r->oam[i * 4 + 3];

        if (y == 0xF4) {
            continue;
        }

        if (emitted) {
            gumshoe_append_json(&p, &remaining, ",");
        }
        emitted = 1;

        gumshoe_append_json(&p, &remaining,
            "{\"slot\":%d,\"y\":\"0x%02X\",\"tile\":\"0x%02X\",\"attr\":\"0x%02X\",\"x\":\"0x%02X\"}",
            i, y, tile, attr, x);
    }

    gumshoe_append_json(&p, &remaining, "]}");
    debug_server_send_line(json);
}

static void gumshoe_send_entity_summary_json(int id, int slot) {
    uint16_t base;

    if (slot < 0 || slot >= 21) {
        debug_server_send_fmt("{\"id\":%d,\"ok\":false,\"err\":\"slot out of range (0-20)\"}", id);
        return;
    }

    base = (uint16_t)(0x0600 + slot * 12);
    debug_server_send_fmt(
        "{\"id\":%d,\"ok\":true,\"slot\":%d,\"base\":\"0x%04X\","
        "\"bytes\":[\"0x%02X\",\"0x%02X\",\"0x%02X\",\"0x%02X\",\"0x%02X\",\"0x%02X\","
                  "\"0x%02X\",\"0x%02X\",\"0x%02X\",\"0x%02X\",\"0x%02X\",\"0x%02X\"]}",
        id, slot, base,
        g_ram[base + 0], g_ram[base + 1], g_ram[base + 2], g_ram[base + 3],
        g_ram[base + 4], g_ram[base + 5], g_ram[base + 6], g_ram[base + 7],
        g_ram[base + 8], g_ram[base + 9], g_ram[base + 10], g_ram[base + 11]);
}

static void gumshoe_send_frame_entities_json(int id, uint32_t frame) {
    const NESFrameRecord *r = debug_server_get_frame_record(frame);
    char json[32768];
    char *p = json;
    size_t remaining = sizeof(json);
    int slot;
    int emitted = 0;

    if (!r) {
        debug_server_send_fmt("{\"id\":%d,\"ok\":false,\"err\":\"frame not in buffer\"}", id);
        return;
    }

    gumshoe_append_json(&p, &remaining,
        "{\"id\":%d,\"ok\":true,\"frame\":%u,\"slots\":[",
        id, r->frame_number);

    for (slot = 0; slot < 21; slot++) {
        uint16_t base = (uint16_t)(0x0600 + slot * 12);
        uint8_t b0 = r->ram_full[base + 0];
        uint8_t b1 = r->ram_full[base + 1];
        uint8_t b2 = r->ram_full[base + 2];
        uint8_t b6 = r->ram_full[base + 6];
        uint8_t b7 = r->ram_full[base + 7];
        uint8_t b8 = r->ram_full[base + 8];
        uint8_t b9 = r->ram_full[base + 9];
        int active = (b0 != 0 || b1 != 0 || b2 != 0 || b6 != 0 || b7 != 0 || b8 != 0 || b9 != 0);

        if (emitted) {
            gumshoe_append_json(&p, &remaining, ",");
        }
        emitted = 1;

        gumshoe_append_json(&p, &remaining,
            "{\"slot\":%d,\"base\":\"0x%04X\",\"active\":%s,"
            "\"b0\":\"0x%02X\",\"b1\":\"0x%02X\",\"b2\":\"0x%02X\",\"b6\":\"0x%02X\",\"b7\":\"0x%02X\",\"b8\":\"0x%02X\",\"b9\":\"0x%02X\"}",
            slot, base, active ? "true" : "false",
            b0, b1, b2, b6, b7, b8, b9);
    }

    gumshoe_append_json(&p, &remaining, "]}");
    debug_server_send_line(json);
}

uint32_t game_get_expected_crc32(void) { return 0xBEB8AB01u; }

const char *game_get_name(void) { return "Gumshoe"; }

void game_on_init(void) {
    int port = (g_run_mode == RUN_MODE_EMULATED) ? 4373 : 4372;
    debug_server_init(port);

    /* Gumshoe requires Zapper (light gun) on port 2. */
    g_zapper_enabled = 1;
    g_zapper_x = 128;
    g_zapper_y = 120;

    /* Gumshoe's hit-detection polls $2002 bit6 twice in the same frame
     * (func_C5BC_b1 Phase 1 at $C627, Phase 2 at $C658). On real hardware,
     * sprite-0-hit latches once per frame and clears only at pre-render.
     * The default pulse model clears bit6 on read, which breaks Phase 2.
     * Enable sticky mode so bit6 is only cleared at VBlank. */
    g_spr0_sticky_mode = 1;

    /* Pre-arm the shot path and its branch gates so manual repros survive inspection latency. */
    gumshoe_add_default_followers();

    if (g_run_mode != RUN_MODE_NATIVE && g_rom_path_for_extras) {
        verify_mode_init(g_rom_path_for_extras);
    }
}

void game_on_frame(uint64_t frame_count) {
    (void)frame_count;
}

void game_post_nmi(uint64_t frame_count) { (void)frame_count; }

int game_handle_arg(const char *key, const char *val) {
    if (strcmp(key, "--verify") == 0) {
        g_run_mode = RUN_MODE_VERIFY;
        return 1;
    }
    if (strcmp(key, "--emulated") == 0) {
        g_run_mode = RUN_MODE_EMULATED;
        return 1;
    }
    (void)val;
    return 0;
}

const char *game_arg_usage(void) {
    return "  --verify     Run both native + Nestopia, compare each frame\n"
           "  --emulated   Run Nestopia only (reference mode)\n";
}

void game_run_nmi(void) {
    verify_mode_run_nmi();
}

void game_run_main(void) {
    if (g_run_mode == RUN_MODE_EMULATED) {
#ifdef ENABLE_NESTOPIA_ORACLE
        printf("[Emulated] Nestopia driving main loop\n");
        static uint32_t emu_argb[256 * 240];
        extern void runner_present_framebuf(const uint32_t *argb_buf);

        for (;;) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) exit(0);
                if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) exit(0);
            }
            const uint8_t *keys = SDL_GetKeyboardState(NULL);
            uint8_t btn = 0;
            if (keys[SDL_SCANCODE_Z])      btn |= 0x80;
            if (keys[SDL_SCANCODE_X])      btn |= 0x40;
            if (keys[SDL_SCANCODE_TAB])    btn |= 0x20;
            if (keys[SDL_SCANCODE_RETURN]) btn |= 0x10;
            if (keys[SDL_SCANCODE_UP])     btn |= 0x08;
            if (keys[SDL_SCANCODE_DOWN])   btn |= 0x04;
            if (keys[SDL_SCANCODE_LEFT])   btn |= 0x02;
            if (keys[SDL_SCANCODE_RIGHT])  btn |= 0x01;
            g_controller1_buttons = btn;

            debug_server_poll();

            int ovr = debug_server_get_input_override();
            if (ovr >= 0) g_controller1_buttons = (uint8_t)ovr;

            nestopia_bridge_run_frame(g_controller1_buttons);
            nestopia_bridge_get_framebuf_argb(emu_argb);
            runner_present_framebuf(emu_argb);

            nestopia_bridge_get_ram(g_ram);
            nestopia_bridge_get_chr_ram(g_chr_ram, sizeof(g_chr_ram));
            nestopia_bridge_get_nametable(g_ppu_nt, sizeof(g_ppu_nt));
            nestopia_bridge_get_palette(g_ppu_pal);
            nestopia_bridge_get_oam(g_ppu_oam);
            {
                NestopiaPpuRegs pr;
                nestopia_bridge_get_ppu_regs(&pr);
                g_ppuctrl     = pr.ctrl;
                g_ppumask     = pr.mask;
                g_ppuscroll_x = pr.scroll_x;
                g_ppuscroll_y = pr.scroll_y;
            }

            g_frame_count++;
            debug_server_record_frame();

            SDL_Delay(16);
        }
#else
        fprintf(stderr, "[Error] Nestopia not compiled in\n");
        func_RESET();
#endif
    } else {
        func_RESET();
    }
}

int game_dispatch_override(uint16_t addr) {
    (void)addr;
    return 0;
}

uint8_t game_ram_read_hook(uint16_t pc, uint16_t addr, uint8_t val) {
    (void)pc; (void)addr;
    return val;
}

void game_fill_frame_record(void *record) {
    NESFrameRecord *r = (NESFrameRecord *)record;
    gumshoe_pack_game_data(r->game_data);
}

void game_post_render(uint32_t *framebuf) { (void)framebuf; }

int game_handle_debug_cmd(const char *cmd, int id, const char *json) {
    if (strcmp(cmd, "game_info") == 0) {
        debug_server_send_fmt("{\"id\":%d,\"ok\":true,\"game\":\"Gumshoe\",\"run_mode\":%d}", id, (int)g_run_mode);
        return 1;
    }

    if (strcmp(cmd, "gumshoe_state") == 0) {
        uint8_t data[32];
        gumshoe_pack_game_data(data);
        debug_server_send_fmt(
            "{\"id\":%d,\"ok\":true,"
            "\"frame\":%llu,\"bank\":%d,"
            "\"ram\":{\"24\":\"0x%02X\",\"25\":\"0x%02X\",\"26\":\"0x%02X\",\"2C\":\"0x%02X\","
                    "\"38\":\"0x%02X\",\"4F\":\"0x%02X\",\"53\":\"0x%02X\",\"84\":\"0x%02X\","
                    "\"8A\":\"0x%02X\",\"8B\":\"0x%02X\",\"C5\":\"0x%02X\",\"C7\":\"0x%02X\","
                    "\"CD\":\"0x%02X\",\"D2\":\"0x%02X\",\"D3\":\"0x%02X\",\"D4\":\"0x%02X\","
                    "\"DB\":\"0x%02X\",\"E3\":\"0x%02X\",\"E4\":\"0x%02X\",\"E7\":\"0x%02X\"},"
            "\"entities\":["
                "{\"slot\":0,\"b0\":\"0x%02X\",\"b1\":\"0x%02X\",\"b2\":\"0x%02X\",\"b3\":\"0x%02X\",\"b4\":\"0x%02X\",\"b5\":\"0x%02X\"},"
                "{\"slot\":1,\"b0\":\"0x%02X\",\"b1\":\"0x%02X\",\"b2\":\"0x%02X\",\"b3\":\"0x%02X\",\"b4\":\"0x%02X\",\"b5\":\"0x%02X\"}"
            "],"
            "\"zapper\":{\"enabled\":%d,\"x\":%d,\"y\":%d,\"trigger\":%d}}",
            id, (unsigned long long)g_frame_count, g_current_bank,
            data[0x00], data[0x01], data[0x02], data[0x03],
            data[0x04], data[0x05], data[0x06], data[0x07],
            data[0x08], data[0x09], data[0x0A], data[0x0B],
            data[0x0C], data[0x0D], data[0x0E], data[0x0F],
            data[0x10], data[0x11], data[0x12], data[0x13],
            data[0x14], data[0x15], data[0x16], data[0x17], data[0x18], data[0x19],
            data[0x1A], data[0x1B], data[0x1C], data[0x1D], data[0x1E], data[0x1F],
            g_zapper_enabled, g_zapper_x, g_zapper_y, g_zapper_trigger);
        return 1;
    }

    if (strcmp(cmd, "gumshoe_frame") == 0) {
        const NESFrameRecord *r;
        int frame = gumshoe_parse_int_arg(json, "frame");

        if (frame < 0) {
            debug_server_send_fmt("{\"id\":%d,\"ok\":false,\"err\":\"missing frame\"}", id);
            return 1;
        }

        r = debug_server_get_frame_record((uint32_t)frame);
        if (!r) {
            debug_server_send_fmt("{\"id\":%d,\"ok\":false,\"err\":\"frame not in buffer\"}", id);
            return 1;
        }

        debug_server_send_fmt(
            "{\"id\":%d,\"ok\":true,"
            "\"frame\":%u,\"bank\":%d,"
            "\"ram\":{\"24\":\"0x%02X\",\"25\":\"0x%02X\",\"26\":\"0x%02X\",\"2C\":\"0x%02X\","
                    "\"38\":\"0x%02X\",\"4F\":\"0x%02X\",\"53\":\"0x%02X\",\"84\":\"0x%02X\","
                    "\"8A\":\"0x%02X\",\"8B\":\"0x%02X\",\"C5\":\"0x%02X\",\"C7\":\"0x%02X\","
                    "\"CD\":\"0x%02X\",\"D2\":\"0x%02X\",\"D3\":\"0x%02X\",\"D4\":\"0x%02X\","
                    "\"DB\":\"0x%02X\",\"E3\":\"0x%02X\",\"E4\":\"0x%02X\",\"E7\":\"0x%02X\"},"
            "\"entities\":["
                "{\"slot\":0,\"b0\":\"0x%02X\",\"b1\":\"0x%02X\",\"b2\":\"0x%02X\",\"b3\":\"0x%02X\",\"b4\":\"0x%02X\",\"b5\":\"0x%02X\"},"
                "{\"slot\":1,\"b0\":\"0x%02X\",\"b1\":\"0x%02X\",\"b2\":\"0x%02X\",\"b3\":\"0x%02X\",\"b4\":\"0x%02X\",\"b5\":\"0x%02X\"}"
            "],"
            "\"meta\":{\"verify_pass\":%d,\"diff_count\":%d,\"last_func\":\"%s\",\"buttons1\":\"0x%02X\"}}",
            id, r->frame_number, r->current_bank,
            r->game_data[0x00], r->game_data[0x01], r->game_data[0x02], r->game_data[0x03],
            r->game_data[0x04], r->game_data[0x05], r->game_data[0x06], r->game_data[0x07],
            r->game_data[0x08], r->game_data[0x09], r->game_data[0x0A], r->game_data[0x0B],
            r->game_data[0x0C], r->game_data[0x0D], r->game_data[0x0E], r->game_data[0x0F],
            r->game_data[0x10], r->game_data[0x11], r->game_data[0x12], r->game_data[0x13],
            r->game_data[0x14], r->game_data[0x15], r->game_data[0x16], r->game_data[0x17], r->game_data[0x18], r->game_data[0x19],
            r->game_data[0x1A], r->game_data[0x1B], r->game_data[0x1C], r->game_data[0x1D], r->game_data[0x1E], r->game_data[0x1F],
            r->verify_pass, r->diff_count, r->last_func, r->controller1_buttons);
        return 1;
    }

    if (strcmp(cmd, "gumshoe_entities") == 0) {
        debug_server_send_fmt(
            "{\"id\":%d,\"ok\":true,"
            "\"slots\":["
            "{\"slot\":0,\"base\":\"0x0600\",\"b0\":\"0x%02X\",\"b1\":\"0x%02X\",\"b2\":\"0x%02X\",\"b7\":\"0x%02X\",\"b8\":\"0x%02X\",\"b9\":\"0x%02X\"},"
            "{\"slot\":1,\"base\":\"0x060C\",\"b0\":\"0x%02X\",\"b1\":\"0x%02X\",\"b2\":\"0x%02X\",\"b7\":\"0x%02X\",\"b8\":\"0x%02X\",\"b9\":\"0x%02X\"},"
            "{\"slot\":2,\"base\":\"0x0618\",\"b0\":\"0x%02X\",\"b1\":\"0x%02X\",\"b2\":\"0x%02X\",\"b7\":\"0x%02X\",\"b8\":\"0x%02X\",\"b9\":\"0x%02X\"}"
            "]}",
            id,
            g_ram[0x0600], g_ram[0x0601], g_ram[0x0602], g_ram[0x0607], g_ram[0x0608], g_ram[0x0609],
            g_ram[0x060C], g_ram[0x060D], g_ram[0x060E], g_ram[0x0613], g_ram[0x0614], g_ram[0x0615],
            g_ram[0x0618], g_ram[0x0619], g_ram[0x061A], g_ram[0x061F], g_ram[0x0620], g_ram[0x0621]);
        return 1;
    }

    if (strcmp(cmd, "gumshoe_window") == 0) {
        int frame = gumshoe_parse_int_arg(json, "frame");
        int before = gumshoe_parse_int_arg(json, "before");
        int after = gumshoe_parse_int_arg(json, "after");

        if (frame < 0) {
            debug_server_send_fmt("{\"id\":%d,\"ok\":false,\"err\":\"missing frame\"}", id);
            return 1;
        }

        if (before < 0) before = 2;
        if (after < 0) after = 2;
        gumshoe_send_window_json(id, (uint32_t)frame, before, after);
        return 1;
    }

    if (strcmp(cmd, "gumshoe_oam") == 0) {
        int frame = gumshoe_parse_int_arg(json, "frame");

        if (frame < 0) {
            debug_server_send_fmt("{\"id\":%d,\"ok\":false,\"err\":\"missing frame\"}", id);
            return 1;
        }

        gumshoe_send_oam_json(id, (uint32_t)frame);
        return 1;
    }

    if (strcmp(cmd, "gumshoe_entity") == 0) {
        gumshoe_send_entity_summary_json(id, gumshoe_parse_int_arg(json, "slot"));
        return 1;
    }

    if (strcmp(cmd, "gumshoe_entities_frame") == 0) {
        int frame = gumshoe_parse_int_arg(json, "frame");

        if (frame < 0) {
            debug_server_send_fmt("{\"id\":%d,\"ok\":false,\"err\":\"missing frame\"}", id);
            return 1;
        }

        gumshoe_send_frame_entities_json(id, (uint32_t)frame);
        return 1;
    }

    return 0;
}
