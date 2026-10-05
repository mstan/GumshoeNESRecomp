/* The widescreen mod's trusted plugin: the Mods screen's selection turns it
 * on with its options; the reset callback restores stock presentation first
 * on every launch (runner/include/mod_runtime.h). */
#include "mod_runtime.h"
#include "gumshoe_ws.h"

#include <stdio.h>

static const char PACKAGE[] = "gumshoe.enhancement.widescreen";

static void reset_widescreen(void) { gumshoe_ws_set_mod_enabled(0); }

static void activate_widescreen(void) {
    char aspect[32] = "fit", hud[32] = "edges", camera[32] = "edges";
    nes_mod_option_value(PACKAGE, "widescreen", "aspect", aspect, sizeof aspect);
    nes_mod_option_value(PACKAGE, "widescreen", "hud", hud, sizeof hud);
    nes_mod_option_value(PACKAGE, "widescreen", "camera", camera, sizeof camera);
    gumshoe_ws_configure(aspect, hud, camera);
    gumshoe_ws_set_mod_enabled(1);
}

NES_MOD_CONSTRUCTOR(register_gumshoe_widescreen_plugin) {
    if (!nes_mod_register_reset_callback(reset_widescreen) ||
        !nes_mod_register_activation_plugin("gumshoe.widescreen", activate_widescreen))
        fprintf(stderr, "[Mods] Failed to register the Gumshoe widescreen plugin\n");
}
