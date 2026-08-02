#include "buus_fury_extended_view.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <utility>

#include "gba_bus.h"
#include "gba_ppu.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"

extern "C" unsigned g_ws_active;
extern "C" unsigned g_ws_extra_left;
extern "C" unsigned g_ws_extra_right;

namespace buus_fury {
namespace {

// Buu's Fury keeps a four-entry cache of decompressed 32x32 chunks for each
// authored overworld layer and streams their visible rings into VRAM screen
// blocks 28..31. A 480-pixel view can cross three chunk columns, so a margin
// occasionally needs one chunk beyond those four native caches. The resource
// layout and variable-bit LZ decoder below mirror the renderer at 0x08007C12
// and the ARM routine installed at 0x03000040.
constexpr std::uint32_t kFieldLayerVtable = 0x08049410u;
constexpr std::uint32_t kCameraLeft = 0x030019BCu;
constexpr std::uint32_t kCameraRight = 0x030019C4u;
constexpr std::size_t kFieldChunkBytes = 0x800u;
constexpr int kFieldCacheBiasX = 8;
constexpr int kFieldCacheBiasY = 48;
std::uint32_t g_field_objects[4] = {};
std::unordered_map<
    std::uint32_t, std::array<std::uint8_t, kFieldChunkBytes>>
    g_decoded_chunks;

gba::GbaBus* active_bus() {
    return gbarecomp::active_bus();
}

std::uint32_t ewram32(gba::GbaBus* bus, std::uint32_t address) {
    if (!bus || address < 0x02000000u || address + 3u >= 0x02040000u)
        return 0;
    const std::uint8_t* p = bus->ewram_ptr() + address - 0x02000000u;
    return static_cast<std::uint32_t>(p[0]) |
        (static_cast<std::uint32_t>(p[1]) << 8) |
        (static_cast<std::uint32_t>(p[2]) << 16) |
        (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint8_t mem8(gba::GbaBus* bus, std::uint32_t address) {
    if (!bus) return 0;
    if (address >= 0x02000000u && address < 0x02040000u)
        return bus->ewram_ptr()[address - 0x02000000u];
    if (address >= 0x08000000u &&
        address - 0x08000000u < bus->rom_size()) {
        return bus->rom_ptr()[address - 0x08000000u];
    }
    return 0;
}

std::uint16_t mem16(gba::GbaBus* bus, std::uint32_t address) {
    return static_cast<std::uint16_t>(mem8(bus, address)) |
        static_cast<std::uint16_t>(mem8(bus, address + 1u) << 8);
}

std::uint32_t mem32(gba::GbaBus* bus, std::uint32_t address) {
    return static_cast<std::uint32_t>(mem16(bus, address)) |
        (static_cast<std::uint32_t>(mem16(bus, address + 2u)) << 16);
}

class FieldBitReader {
public:
    FieldBitReader(gba::GbaBus* bus, std::uint32_t address)
        : bus_(bus), address_(address) {}

    std::uint32_t bits(unsigned count) {
        if (count > 31u) {
            ok_ = false;
            return 0u;
        }
        std::uint32_t value = 0u;
        while (count--)
            value = (value << 1) | bit();
        return value;
    }

    bool ok() const { return ok_; }

private:
    std::uint32_t bit() {
        if (remaining_ == 0u) {
            if (!bus_ || address_ < 0x08000000u ||
                address_ - 0x08000000u + 4u > bus_->rom_size()) {
                ok_ = false;
                return 0u;
            }
            word_ = mem32(bus_, address_);
            address_ += 4u;
            remaining_ = 32u;
        }
        const std::uint32_t value = word_ >> 31;
        word_ <<= 1;
        --remaining_;
        return value;
    }

    gba::GbaBus* bus_ = nullptr;
    std::uint32_t address_ = 0u;
    std::uint32_t word_ = 0u;
    unsigned remaining_ = 0u;
    bool ok_ = true;
};

bool decode_field_chunk(
    gba::GbaBus* bus, std::uint32_t descriptor,
    std::array<std::uint8_t, kFieldChunkBytes>* out) {
    if (!bus || !out || descriptor < 0x08000000u ||
        descriptor - 0x08000000u + 8u > bus->rom_size())
        return false;

    const std::uint32_t kind = mem32(bus, descriptor);
    const std::uint32_t output_size = mem32(bus, descriptor + 4u);
    if (output_size != kFieldChunkBytes)
        return false;

    if (kind == 0u) {
        if (descriptor - 0x08000000u + 8u + output_size >
            bus->rom_size())
            return false;
        for (std::size_t i = 0; i < out->size(); ++i)
            (*out)[i] = mem8(bus, descriptor + 8u +
                static_cast<std::uint32_t>(i));
        return true;
    }
    if (kind != 1u && kind != 2u)
        return false;

    FieldBitReader reader(bus, descriptor + 8u);
    std::size_t produced = 0u;
    unsigned distance_bits = 8u;
    unsigned literal_bits = kind;
    std::uint32_t literal_base = 0u;
    std::uint32_t last_distance = 1u;

    auto append = [&](std::uint32_t value) {
        if (produced >= out->size()) return false;
        (*out)[produced++] = static_cast<std::uint8_t>(value);
        return true;
    };
    auto copy = [&](std::uint32_t distance, std::uint32_t length) {
        if (distance == 0u || distance > produced ||
            length > out->size() - produced)
            return false;
        while (length--) {
            (*out)[produced] = (*out)[produced - distance];
            ++produced;
        }
        return true;
    };
    auto gamma = [&]() {
        std::uint32_t value = 1u;
        do {
            value = (value << 1) | reader.bits(1u);
        } while (reader.bits(1u) != 0u && reader.ok());
        return value;
    };

    bool terminated = false;
    for (unsigned commands = 0; commands < 65536u && !terminated;
         ++commands) {
        if (reader.bits(1u) != 0u) {
            if (literal_bits > 24u ||
                !append(reader.bits(literal_bits) + literal_base))
                return false;
            continue;
        }

        if (reader.bits(1u) != 0u) {
            const std::uint32_t code = gamma();
            std::uint32_t distance = 0u;
            std::uint32_t length = 0u;
            if (code == 2u) {
                distance = last_distance;
                length = gamma();
            } else {
                if (distance_bits > 24u || code < 3u)
                    return false;
                distance =
                    reader.bits(distance_bits) +
                    ((code - 3u) << distance_bits);
                last_distance = distance;
                length = gamma();
                if (distance >= 0x10000u)
                    length += 3u;
                else if (distance >= 0x37FFu)
                    length += 2u;
                else if (distance >= 0x027Fu)
                    length += 1u;
                else if (distance <= 0x007Fu)
                    length += 4u;
            }
            if (!copy(distance, length))
                return false;
            continue;
        }

        if (reader.bits(1u) == 0u) {
            const std::uint32_t distance = reader.bits(7u);
            if (distance != 0u) {
                last_distance = distance;
                if (!copy(distance, reader.bits(2u) + 2u))
                    return false;
            } else {
                const unsigned code = reader.bits(2u);
                if (code == 0u) {
                    terminated = true;
                } else {
                    distance_bits = reader.bits(code + 3u);
                }
            }
            continue;
        }

        const int short_code =
            static_cast<int>(reader.bits(4u)) - 1;
        if (short_code == 0) {
            if (!append(0u)) return false;
        } else if (short_code > 0) {
            if (!copy(static_cast<unsigned>(short_code), 1u))
                return false;
        } else if (reader.bits(1u) != 0u) {
            do {
                for (unsigned i = 0; i < 256u; ++i) {
                    if (!append(reader.bits(8u))) return false;
                }
            } while (reader.bits(1u) != 0u && reader.ok());
        } else {
            literal_base = 0u;
            literal_bits = 7u + reader.bits(1u);
            if (literal_bits == 7u)
                literal_base = reader.bits(8u);
        }
        if (!reader.ok())
            return false;
    }
    return reader.ok() && terminated && produced == out->size();
}

const std::uint8_t* decoded_field_chunk(
    gba::GbaBus* bus, std::uint32_t descriptor) {
    const auto found = g_decoded_chunks.find(descriptor);
    if (found != g_decoded_chunks.end())
        return found->second.data();

    std::array<std::uint8_t, kFieldChunkBytes> decoded{};
    if (!decode_field_chunk(bus, descriptor, &decoded))
        return nullptr;
    const auto inserted =
        g_decoded_chunks.emplace(descriptor, std::move(decoded));
    return inserted.first->second.data();
}

bool field_object_valid(
    gba::GbaBus* bus, std::uint32_t object, int layer) {
    if (!bus || object < 0x02000000u || object + 0x4Bu >= 0x02040000u)
        return false;
    if (ewram32(bus, object) != kFieldLayerVtable ||
        (ewram32(bus, object + 4u) & 0xFFu) !=
            static_cast<unsigned>(layer)) {
        return false;
    }
    const std::uint32_t resource = ewram32(bus, object + 8u);
    const std::uint32_t width = ewram32(bus, object + 0x3Cu);
    const std::uint32_t height = ewram32(bus, object + 0x40u);
    return resource >= 0x08000000u &&
        resource - 0x08000000u + 0x18u < bus->rom_size() &&
        width > 0u && width <= 4096u &&
        height > 0u && height <= 4096u;
}

void resolve_field_objects(gba::GbaBus* bus) {
    bool current = true;
    for (int layer = 0; layer < 4; ++layer)
        current &= field_object_valid(bus, g_field_objects[layer], layer);
    if (current) return;

    for (std::uint32_t& object : g_field_objects)
        object = 0u;
    for (std::uint32_t offset = 0; offset + 0x4Cu <= 0x40000u;
         offset += 4u) {
        const std::uint32_t object = 0x02000000u + offset;
        if (ewram32(bus, object) != kFieldLayerVtable)
            continue;
        const unsigned layer = ewram32(bus, object + 4u) & 0xFFu;
        if (layer < 4u &&
            field_object_valid(bus, object, static_cast<int>(layer))) {
            g_field_objects[layer] = object;
        }
    }
}

bool is_overworld(gba::GbaBus* bus) {
    if (!bus || !g_ws_active) return false;
    const std::uint16_t dispcnt = bus->io().read16(0x000u);
    if ((dispcnt & 7u) != 0u || (dispcnt & 0x1F40u) != 0x1F40u)
        return false;

    for (int bg = 0; bg < 4; ++bg) {
        const std::uint16_t cnt =
            bus->io().read16(0x008u + static_cast<std::uint32_t>(bg * 2));
        if (((cnt >> 8) & 0x1Fu) != static_cast<unsigned>(28 + bg) ||
            ((cnt >> 14) & 3u) != 0u) {
            return false;
        }
    }
    return true;
}

bool field_entry(
    gba::GbaBus* bus, std::uint32_t object, int world_x, int world_y,
    std::uint16_t* out_entry) {
    if (!bus || !out_entry || world_x < 0 || world_y < 0)
        return false;

    const int width = static_cast<std::int32_t>(
        ewram32(bus, object + 0x3Cu));
    const int height = static_cast<std::int32_t>(
        ewram32(bus, object + 0x40u));
    if (world_x >= width || world_y >= height)
        return false;

    const std::uint32_t resource = ewram32(bus, object + 0x08u);
    const unsigned chunk_columns = mem8(bus, resource + 0x14u);
    if (chunk_columns == 0u)
        return false;

    const unsigned tile_x = static_cast<unsigned>(world_x) >> 3;
    const unsigned tile_y = static_cast<unsigned>(world_y) >> 3;
    const unsigned chunk_x = tile_x >> 5;
    const unsigned chunk_y = tile_y >> 5;
    const unsigned chunk_index = chunk_y * chunk_columns + chunk_x;
    if (chunk_index > 0xFEu)
        return false;

    // The renderer owns four 32x32 physical caches. object+0x20 records
    // which resource chunk currently occupies each cache, while object+0x10
    // holds its tile-data pointer. Resolving through that owner table is the
    // critical distinction from treating the caches as one fixed 64x64 map.
    std::uint32_t chunk = 0;
    for (unsigned slot = 0; slot < 4; ++slot) {
        if (mem8(bus, object + 0x20u + slot) == chunk_index) {
            chunk = ewram32(bus, object + 0x10u + slot * 4u);
            break;
        }
    }
    const unsigned local_x = tile_x & 31u;
    const unsigned local_y = tile_y & 31u;
    const std::uint32_t offset =
        static_cast<std::uint32_t>((local_y * 32u + local_x) * 2u);
    if (chunk == 0u) {
        const std::uint32_t descriptor =
            mem32(bus, resource + 0x18u + chunk_index * 4u);
        const std::uint8_t* decoded =
            decoded_field_chunk(bus, descriptor);
        if (!decoded)
            return false;
        *out_entry = static_cast<std::uint16_t>(decoded[offset]) |
            static_cast<std::uint16_t>(decoded[offset + 1u] << 8);
        return true;
    }

    if (!((chunk >= 0x02000000u && chunk + offset + 1u < 0x02040000u) ||
          (chunk >= 0x08000000u &&
           chunk + offset + 1u - 0x08000000u < bus->rom_size()))) {
        return false;
    }
    *out_entry = mem16(bus, chunk + offset);
    return true;
}

extern "C" int buus_fury_tilemap_provider(
    int bg, int hardware_x, int screen_y, std::uint16_t* out_entry) {
    gba::GbaBus* bus = active_bus();
    if (!out_entry || bg < 0 || bg > 3 || !is_overworld(bus))
        return gba::kWsTilemapUnavailable;

    resolve_field_objects(bus);
    const std::uint32_t object = g_field_objects[bg];
    if (!object)
        return gba::kWsTilemapUnavailable;

    // +0x34/+0x38 are the layer's parallax offsets, not the camera.
    // The renderer writes the absolute layer scroll to BGxHOFS/VOFS and
    // records floor((scroll - {8,48}) / 256) at +0x24/+0x28. Recombine
    // those values to recover the authored coordinate rather than using the
    // wrapped 256-pixel hardware ring. The bias disambiguates the interval
    // when the register's low byte lies just before a chunk boundary.
    const int chunk_x = static_cast<std::int32_t>(
        ewram32(bus, object + 0x24u));
    const int chunk_y = static_cast<std::int32_t>(
        ewram32(bus, object + 0x28u));
    const std::uint32_t bg_io =
        0x010u + static_cast<std::uint32_t>(bg * 4);
    const int hofs = bus->io().read16(bg_io) & 0xFF;
    const int vofs = bus->io().read16(bg_io + 2u) & 0xFF;
    const int layer_scroll_x =
        chunk_x * 256 + hofs + (hofs < kFieldCacheBiasX ? 256 : 0);
    const int layer_scroll_y =
        chunk_y * 256 + vofs + (vofs < kFieldCacheBiasY ? 256 : 0);
    const int world_x = layer_scroll_x + hardware_x;
    const int world_y = layer_scroll_y + screen_y;
    if (!field_entry(bus, object, world_x, world_y, out_entry))
        return gba::kWsTilemapUnavailable;
    return gba::kWsTilemapReplace;
}

extern "C" int buus_fury_obj_x_provider(
    int oam_index, std::uint16_t attr0, std::uint16_t attr1,
    std::uint16_t, int* out_x) {
    gba::GbaBus* bus = active_bus();
    if (!out_x || !is_overworld(bus)) return 0;

    const int x = attr1 & 0x1FFu;
    const int y = attr0 & 0xFFu;

    // Buu's Fury retains the Webfoot convention in which the first three OAM
    // slots are the top-left portrait/health/energy group. World actors begin
    // at slot 3 and must stay camera-relative.
    if (oam_index >= 0 && oam_index <= 2 && x < 96 && y < 32) {
        *out_x = x - static_cast<int>(g_ws_extra_left);
        return 1;
    }

    const int widened_right = 240 + static_cast<int>(g_ws_extra_right);
    *out_x = x <= widened_right ? x : x - 512;
    return 1;
}

extern "C" int buus_fury_bus_read_override(
    std::uint32_t pc, std::uint32_t address, std::uint32_t width,
    std::uint32_t original, std::uint32_t* out_value) {
    gba::GbaBus* bus = active_bus();
    if (!out_value || width != 4u || !is_overworld(bus))
        return 0;

    // The actor renderer intersects each object's world rectangle with the
    // camera rectangle at 0x030019B[C..C8]. Widen only the horizontal bound
    // reads in that predicate. The camera, streaming, and actor coordinates
    // remain byte-for-byte native.
    if (pc == 0x08019C06u && address == kCameraLeft) {
        *out_value =
            original - static_cast<std::uint32_t>(g_ws_extra_left);
        return 1;
    }
    if (pc == 0x08019BFEu && address == kCameraRight) {
        *out_value =
            original + static_cast<std::uint32_t>(g_ws_extra_right);
        return 1;
    }
    return 0;
}

}  // namespace

void install_extended_view(std::uint32_t, std::uint32_t) {
    for (std::uint32_t& object : g_field_objects)
        object = 0u;
    g_decoded_chunks.clear();
    gba::g_ws_tilemap_provider = &buus_fury_tilemap_provider;
    gba::g_ws_authored_margin_layers = 1;
    gba::g_ws_bg_x_provider = nullptr;
    gba::g_ws_bg_x_provider_layers = 0;
    gba::g_ws_obj_attr_x_provider = &buus_fury_obj_x_provider;
    g_runtime_bus_read_override = &buus_fury_bus_read_override;
    std::fprintf(stderr,
        "[buus-fury:view] authored BG0..BG3 field resources + exact chunk "
        "decode + edge HUD + expanded actor visibility enabled\n");
}

}  // namespace buus_fury
