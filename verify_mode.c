/*
 * verify_mode.c — Dual-execution verification mode
 *
 * In VERIFY mode: native code runs the game normally. Nestopia runs
 * in the background. After each frame, we compare RAM between the two.
 * Divergences are logged and recorded in the ring buffer.
 *
 * In EMULATED mode: Nestopia drives everything (handled in extras.c).
 * In NATIVE mode: no emulator, just recompiled code.
 */
#include "verify_mode.h"
#include "nes_snapshot.h"
#include "nes_runtime.h"
#include "debug_server.h"

#include <stdio.h>
#include <string.h>

#ifdef ENABLE_NESTOPIA_ORACLE
#include "nestopia_bridge.h"
#endif

RunMode  g_run_mode = RUN_MODE_NATIVE;
static uint64_t s_divergence_count = 0;
static int s_emu_initialized = 0;
static FILE *s_mode_trace = NULL;
static uint64_t s_first_2C_diverge_frame = (uint64_t)-1;
static uint64_t s_first_25_diverge_frame = (uint64_t)-1;

/* Snapshot of native RAM taken BEFORE NMI — represents end-of-frame state
 * (main loop has finished, game is waiting for next VBlank).  Compare this
 * against oracle's post-retro_run state, which is also end-of-frame. */
static uint8_t s_native_pre_nmi[0x800];
static int s_have_pre_nmi = 0;

void verify_mode_init(const char *rom_path) {
#ifdef ENABLE_NESTOPIA_ORACLE
    if (g_run_mode == RUN_MODE_NATIVE) return;

    int rc = nestopia_bridge_init(rom_path);
    if (rc != 0) {
        fprintf(stderr, "[verify] Nestopia init failed (rc=%d), falling back to native\n", rc);
        g_run_mode = RUN_MODE_NATIVE;
        return;
    }
    s_emu_initialized = 1;

    /* Align oracle with native post-RESET state.
     * Native func_RESET() completes synchronously (spin-waits resolved by
     * maybe_trigger_vblank) — takes 0 NMI frames.
     * Nestopia RESET has two VBlank waits — takes ~2 frames.
     * Run 2 warm-up frames so both sides start from the same point. */
    if (g_run_mode == RUN_MODE_VERIFY) {
        nestopia_bridge_run_frame(0);
        fprintf(stderr, "[verify] Oracle warmed up 1 frame (RESET VBlank alignment)\n");
    }

    fprintf(stderr, "[verify] Nestopia oracle initialized (mode=%s)\n",
            g_run_mode == RUN_MODE_VERIFY ? "verify" : "emulated");
#else
    (void)rom_path;
    if (g_run_mode != RUN_MODE_NATIVE) {
        fprintf(stderr, "[verify] Nestopia not compiled in, falling back to native\n");
        g_run_mode = RUN_MODE_NATIVE;
    }
#endif
}

int verify_mode_run_nmi(void) {
    /* Frame boundary: only invoke the game's NMI handler when NMI is
     * enabled.  main_runner.c calls game_run_nmi unconditionally every
     * wall-clock frame (for oracle sync + frame counter), so this gate
     * lives here rather than there. */
    extern uint8_t g_ppuctrl;
    int nmi_enabled = (g_ppuctrl & 0x80) != 0;

    if (g_run_mode == RUN_MODE_NATIVE) {
        if (nmi_enabled) func_NMI();
        return 1;
    }

#ifdef ENABLE_NESTOPIA_ORACLE
    if (!s_emu_initialized) {
        if (nmi_enabled) func_NMI();
        return 1;
    }

    if (g_run_mode == RUN_MODE_EMULATED) {
        if (nmi_enabled) func_NMI();
        return 1;
    }

    /* VERIFY mode: compare pre-NMI native state against oracle post-frame.
     *
     * Execution-point alignment:
     *   Native pre-NMI  = main loop done, waiting for VBlank
     *   Oracle post-run  = retro_run finished (NMI + main loop done)
     *
     * Both represent "end of frame" — the game has processed the previous
     * VBlank and is idle.  This avoids false positives from $0020 (VBlank
     * flag) being set by NMI but not yet cleared by main loop. */

    /* 1. Snapshot native RAM BEFORE NMI */
    memcpy(s_native_pre_nmi, g_ram, 0x800);

    /* 2. Run native NMI — but only if NMI is actually enabled.
     *    When disabled, the frame boundary still advances (oracle runs,
     *    frame counter ticks) so cadence stays in lockstep, but the
     *    game's NMI handler is not invoked (matches real-hardware
     *    behavior where NMI with PPUCTRL bit7=0 simply doesn't fire). */
    if (nmi_enabled) func_NMI();

    /* 3. Run Nestopia for one frame (same input) */
    nestopia_bridge_run_frame(g_controller1_buttons);

    /* 4. Get Nestopia's post-frame RAM */
    static uint8_t emu_ram[0x800];
    nestopia_bridge_get_ram(emu_ram);

    /* Per-frame CSV trace of $2C (decrement counter) and $25/$26 (mode).
     * Emitted unconditionally so we can see cadence, not just divergence. */
    if (!s_mode_trace) {
        s_mode_trace = fopen("mode_trace.csv", "w");
        if (s_mode_trace)
            fprintf(s_mode_trace,
                "frame,btn,nat_2C,emu_2C,nat_25,emu_25,nat_26,emu_26,nmi_fires,cycles,instrs,forced_caps,maxdepth_skips,no_ppu\n");
    }
    if (s_mode_trace) {
        uint32_t nf = runtime_pop_nmi_fires();
        uint32_t cy = runtime_pop_cycle_budget_used();
        uint32_t ii = runtime_pop_instrs_ticked();
        uint32_t fc = runtime_pop_forced_caps();
        fprintf(s_mode_trace, "%llu,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%u,%u,%u,%u,%u,%u\n",
            (unsigned long long)g_frame_count,
            g_controller1_buttons,
            s_native_pre_nmi[0x2C], emu_ram[0x2C],
            s_native_pre_nmi[0x25], emu_ram[0x25],
            s_native_pre_nmi[0x26], emu_ram[0x26],
            nf, cy, ii,
            fc & 0xFFFF, (fc >> 16) & 0xFF, (fc >> 24) & 0xFF);
        if (s_first_2C_diverge_frame == (uint64_t)-1 &&
            s_native_pre_nmi[0x2C] != emu_ram[0x2C]) {
            s_first_2C_diverge_frame = g_frame_count;
            fprintf(stderr, "[verify-first] $2C diverges frame=%llu nat=%02X emu=%02X (btn=%02X)\n",
                (unsigned long long)g_frame_count,
                s_native_pre_nmi[0x2C], emu_ram[0x2C], g_controller1_buttons);
        }
        if (s_first_25_diverge_frame == (uint64_t)-1 &&
            s_native_pre_nmi[0x25] != emu_ram[0x25]) {
            s_first_25_diverge_frame = g_frame_count;
            fprintf(stderr, "[verify-first] $25 diverges frame=%llu nat=%02X emu=%02X (btn=%02X)\n",
                (unsigned long long)g_frame_count,
                s_native_pre_nmi[0x25], emu_ram[0x25], g_controller1_buttons);
        }
        fflush(s_mode_trace);
    }

    /* 5. Compare pre-NMI native vs post-frame oracle
     *    Skip stack ($0100-$01FF) — recomp uses C call stack.
     *    Skip first frame (no meaningful pre-NMI snapshot yet). */
    if (!s_have_pre_nmi) {
        s_have_pre_nmi = 1;
        return 1; /* skip comparison for frame 0 */
    }

    int diff_count = 0;
    int first_diff_addr = -1;
    uint8_t first_native = 0, first_emu = 0;

    for (int i = 0; i < 0x0800; i++) {
        if (i >= 0x0100 && i < 0x0200) continue; /* skip 6502 stack page */
        if (s_native_pre_nmi[i] != emu_ram[i]) {
            if (diff_count == 0) {
                first_diff_addr = i;
                first_native = s_native_pre_nmi[i];
                first_emu = emu_ram[i];
            }
            diff_count++;
        }
    }

    int passed = (diff_count == 0);

    if (!passed) {
        s_divergence_count++;
        fprintf(stderr, "[verify] DIVERGE frame %llu: %d bytes differ | first: $%04X native=0x%02X emu=0x%02X\n",
                (unsigned long long)g_frame_count, diff_count,
                first_diff_addr, first_native, first_emu);

        /* Dump ALL non-stack diverging addresses for first 10 divergences
         * AND for the 20-frame window straddling the $25 transition. */
        if (s_divergence_count <= 10 ||
            (g_frame_count >= 355 && g_frame_count <= 375)) {
            for (int i = 0; i < 0x0800; i++) {
                if (i >= 0x0100 && i < 0x0200) continue;
                if (s_native_pre_nmi[i] != emu_ram[i]) {
                    fprintf(stderr, "[verify]   $%04X: native=0x%02X emu=0x%02X\n",
                            i, s_native_pre_nmi[i], emu_ram[i]);
                }
            }
        }

        /* Always log section-progression addresses when they diverge */
        {
            static const int watch[] = {0x24, 0x25, 0x26, 0x2C, 0x5E, 0xDB, 0xE7};
            for (int w = 0; w < 7; w++) {
                int a = watch[w];
                if (s_native_pre_nmi[a] != emu_ram[a]) {
                    fprintf(stderr, "[verify-section] $%04X: native=0x%02X emu=0x%02X (frame %llu)\n",
                            a, s_native_pre_nmi[a], emu_ram[a],
                            (unsigned long long)g_frame_count);
                }
            }
        }
    }

    if (s_divergence_count > 0 && g_frame_count < 200) {
        /* Log when frames match after prior divergences — helps identify
         * transient vs persistent differences */
        fprintf(stderr, "[verify] MATCH frame %llu (after %llu divergences)\n",
                (unsigned long long)g_frame_count,
                (unsigned long long)s_divergence_count);
    }

    return passed;
#else
    func_NMI();
    return 1;
#endif
}

uint64_t verify_mode_get_divergence_count(void) {
    return s_divergence_count;
}
