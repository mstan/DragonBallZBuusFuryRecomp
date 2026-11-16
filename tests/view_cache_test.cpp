#define BUUS_FURY_VIEW_TEST 1
#include "../src/buus_fury_extended_view.cpp"
#include <memory>
#include <vector>

static void check(bool yes, const char* message) {
    if (!yes) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
int main() {
    auto bus = std::make_unique<gba::GbaBus>();
    gba::GbaPpu ppu;
    std::vector<std::uint8_t> rom(0x10000);
    bus->set_rom(rom.data(), rom.size());
    gbarecomp::set_active_bus(bus.get());
    gbarecomp::set_active_ppu(&ppu);
    auto put = [&](unsigned offset, unsigned value) {
        auto* p = bus->ewram_ptr() + offset;
        for (int i = 0; i < 4; ++i) p[i] = value >> (8 * i);
    };
    auto layer = [&](unsigned offset, unsigned index) {
        put(offset, buus_fury::kFieldLayerVtable); put(offset + 4, index);
        put(offset + 8, 0x08000000); put(offset + 0x3c, 1024); put(offset + 0x40, 1024);
    };
    buus_fury::install_extended_view(24, 24);
    buus_fury::g_field_scans = 0;
    for (int i = 0; i < 1000; ++i) buus_fury::resolve_field_objects(bus.get());
    check(buus_fury::g_field_scans == 1, "missing layers require one scan per frame");
    layer(0x100, 0);
    ppu.tick(gba::GbaPpu::kCyclesPerFrame);
    buus_fury::resolve_field_objects(bus.get());
    check(buus_fury::g_field_objects[0] == 0x02000100 && buus_fury::g_field_scans == 2,
          "newly allocated layer discovered on next frame despite other missing layers");
    for (int i = 0; i < 1000; ++i) buus_fury::resolve_field_objects(bus.get());
    check(buus_fury::g_field_scans == 2, "partial layer set does not rescan per pixel");
    // A current-build restore can keep the exact same frame number while
    // replacing the object arena. The runtime increments this host epoch.
    put(0x100, 0); layer(0x300, 1); ++g_runtime_state_epoch;
    buus_fury::resolve_field_objects(bus.get());
    check(buus_fury::g_field_objects[0] == 0 && buus_fury::g_field_objects[1] == 0x02000300 &&
          buus_fury::g_field_scans == 3, "same-frame restore invalidates cached presence and absence");
    buus_fury::install_extended_view(24, 24);
    buus_fury::resolve_field_objects(bus.get());
    check(buus_fury::g_field_scans == 4, "reset/install invalidates discovery cache");
    // A narrow/decorative layer can end before the camera's native viewport.
    // Framing must not hide a native-centered actor or cancel real scrolling.
    g_ws_active = 1; g_ws_extra_left = 24; g_ws_extra_right = 24;
    buus_fury::g_content_x_bounds[0x08000000] = {0,800};
    put(0x300 + 0x24,3);
    bus->io().write16(0x000,0x1f40);
    for (unsigned bg=0;bg<4;++bg) bus->io().write16(0x008+bg*2,(28+bg)<<8);
    bus->io().write16(0x014,120);
    const int first_shift=buus_fury::calculate_view_shift(bus.get());
    buus_fury::g_render_view_shift=first_shift;
    int actor_x=0, hud_x=0;
    check(buus_fury::buus_fury_obj_x_provider(3,80,120,0,&actor_x)==1 &&
          actor_x+24>=0 && actor_x+24+16<=288,
          "a native-centered actor remains within the widened viewport");
    check(buus_fury::buus_fury_obj_x_provider(0,4,8,0,&hud_x)==1 && hud_x+24==8,
          "boundary framing preserves the native HUD anchor");
    bus->io().write16(0x014,200);
    const int second_shift=buus_fury::calculate_view_shift(bus.get());
    check(first_shift>=-24 && first_shift<=24 && second_shift>=-24 && second_shift<=24,
          "framing stays within the added margins");
    check((968+second_shift)-(888+first_shift)==80,
          "a far-scrolled decorative layer cannot pin the rendered camera");
    g_ws_extra_left = 16; g_ws_extra_right = 32;
    buus_fury::g_content_x_bounds[0x08000000] = {100,800};
    put(0x300 + 0x24,0); bus->io().write16(0x014,8);
    check(buus_fury::calculate_view_shift(bus.get())==16,
          "left framing uses only the available left margin");
    g_ws_extra_left = 0; g_ws_extra_right = 0;
    check(buus_fury::calculate_view_shift(bus.get())==0,
          "native width never changes the game's camera origin");
    gbarecomp::set_active_ppu(nullptr); gbarecomp::set_active_bus(nullptr);
    std::puts("Buu view discovery cache: PASS");
}
