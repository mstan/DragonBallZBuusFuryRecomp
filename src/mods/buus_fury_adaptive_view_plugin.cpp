#include "mod_runtime.h"

namespace {

void reset_adaptive_view() {
    (void)gba_mod_set_adaptive_view_enabled(0);
}

void activate_adaptive_view() {
    (void)gba_mod_set_adaptive_view_enabled(1);
}

}  // namespace

GBA_MOD_CONSTRUCTOR(buus_fury_register_adaptive_view_plugin) {
    (void)gba_mod_register_reset_callback(reset_adaptive_view);
    (void)gba_mod_register_activation_plugin(
        "dragon-ball-z-buus-fury.adaptive-view",
        activate_adaptive_view);
}
