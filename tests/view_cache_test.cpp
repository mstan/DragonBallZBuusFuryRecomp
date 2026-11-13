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
    gbarecomp::set_active_ppu(nullptr); gbarecomp::set_active_bus(nullptr);
    std::puts("Buu view discovery cache: PASS");
}
