#include "buus_fury_extended_view.h"

#include <array>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <limits>
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
constexpr std::uint32_t kCameraX = 0x030019CCu;
constexpr std::uint32_t kCameraY = 0x030019D0u;
constexpr std::uint32_t kActorFixedX = 0x1A0u;
constexpr std::uint32_t kActorFixedY = 0x1A4u;
constexpr std::size_t kFieldChunkBytes = 0x800u;
constexpr int kFieldCacheBiasX = 8;
constexpr int kFieldCacheBiasY = 48;
constexpr int kActorCullPadding = 32;
std::uint32_t g_field_objects[4] = {};
std::unordered_map<
    std::uint32_t, std::array<std::uint8_t, kFieldChunkBytes>>
    g_decoded_chunks;
std::unordered_map<std::uint32_t, std::pair<int, int>>
    g_content_x_bounds;
int g_render_view_shift = 0;

struct ObjXHistory {
    std::uint16_t attr0_shape = 0;
    std::uint16_t attr1_shape = 0;
    std::uint16_t attr2 = 0;
    int raw_y = 0;
    int resolved_x = 0;
    bool valid = false;
};

struct ActorHistory {
    std::uint32_t object = 0;
    std::uint32_t generation = 0;
};

std::array<ObjXHistory, 128> g_previous_obj_x{};
std::array<ObjXHistory, 128> g_current_obj_x{};
std::array<ActorHistory, 128> g_actor_history{};
std::size_t g_actor_history_cursor = 0;
std::uint32_t g_obj_x_generation = 1;

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
    if (address >= 0x03000000u && address < 0x03008000u)
        return bus->iwram_ptr()[address - 0x03000000u];
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

int distance(int a, int b) {
    return std::abs(a - b);
}

bool adjacent_obj_generation(std::uint32_t generation) {
    return generation == g_obj_x_generation ||
        generation + 1u == g_obj_x_generation;
}

void remember_actor(gba::GbaBus* bus, std::uint32_t object) {
    if (object < 0x02000000u ||
        object + kActorFixedY + 3u >= 0x02040000u) {
        return;
    }
    const std::uint32_t vtable = mem32(bus, object);
    if (vtable < 0x08000000u || vtable >= 0x0A000000u)
        return;

    for (ActorHistory& actor : g_actor_history) {
        if (actor.object == object) {
            actor.generation = g_obj_x_generation;
            return;
        }
    }
    ActorHistory& actor = g_actor_history[
        g_actor_history_cursor++ % g_actor_history.size()];
    actor.object = object;
    actor.generation = g_obj_x_generation;
}

bool match_actor_x(gba::GbaBus* bus, int raw_x, int raw_y, int* out_x) {
    if (!bus || !out_x) return false;

    const int camera_x =
        static_cast<std::int32_t>(mem32(bus, kCameraX));
    const int camera_y =
        static_cast<std::int32_t>(mem32(bus, kCameraY));
    int best_score = std::numeric_limits<int>::max();
    int best_x = raw_x;
    for (const ActorHistory& actor : g_actor_history) {
        if (!actor.object || !adjacent_obj_generation(actor.generation))
            continue;
        const std::uint32_t vtable = mem32(bus, actor.object);
        if (vtable < 0x08000000u || vtable >= 0x0A000000u)
            continue;

        const int actor_x = static_cast<std::int32_t>(
            mem32(bus, actor.object + kActorFixedX)) >> 8;
        const int actor_y = static_cast<std::int32_t>(
            mem32(bus, actor.object + kActorFixedY)) >> 8;
        const int expected_x = actor_x - camera_x;
        const int expected_y = actor_y - camera_y;
        const int dy = distance(raw_y, expected_y);
        if (dy > 96) continue;

        const int candidates[] = {raw_x, raw_x - 512};
        for (const int candidate : candidates) {
            const int dx = distance(candidate, expected_x);
            if (dx > 96) continue;
            const int score = dx + dy;
            if (score < best_score) {
                best_score = score;
                best_x = candidate;
            }
        }
    }
    if (best_score == std::numeric_limits<int>::max())
        return false;
    *out_x = best_x;
    return true;
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
    // +0x3C/+0x40 are 10-bit fixed-point parallax scales copied from
    // resource+0x04/+0x08 (1024 = 1x), not field dimensions.
    const std::uint32_t scale_x = ewram32(bus, object + 0x3Cu);
    const std::uint32_t scale_y = ewram32(bus, object + 0x40u);
    return resource >= 0x08000000u &&
        resource - 0x08000000u + 0x18u < bus->rom_size() &&
        scale_x > 0u && scale_x <= 4096u &&
        scale_y > 0u && scale_y <= 4096u;
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

    const std::uint32_t resource = ewram32(bus, object + 0x08u);
    const unsigned chunk_columns = mem8(bus, resource + 0x14u);
    const unsigned chunk_rows = mem8(bus, resource + 0x15u);
    if (chunk_columns == 0u || chunk_rows == 0u)
        return false;

    // The resource's chunk grid is the authored map extent. Each descriptor
    // expands to one 32x32-tile (256x256-pixel) field chunk.
    if (static_cast<unsigned>(world_x) >= chunk_columns * 256u ||
        static_cast<unsigned>(world_y) >= chunk_rows * 256u) {
        return false;
    }

    const unsigned tile_x = static_cast<unsigned>(world_x) >> 3;
    const unsigned tile_y = static_cast<unsigned>(world_y) >> 3;
    const unsigned chunk_x = tile_x >> 5;
    const unsigned chunk_y = tile_y >> 5;
    const unsigned chunk_index = chunk_y * chunk_columns + chunk_x;
    if (chunk_index > 0xFEu)
        return false;

    // The renderer owns four fixed 32x32 physical caches beginning at +0x4C.
    // object+0x20 records which resource chunk currently occupies each slot.
    // The four pointers at +0x10 are instead the current 2x2 screen-quadrant
    // assignment; they rotate and may point at the shared blank map when the
    // camera crosses a resource edge. Pairing an owner byte with the same
    // +0x10 index therefore produces the stale margin seam seen while walking.
    std::uint32_t chunk = 0;
    for (unsigned slot = 0; slot < 4; ++slot) {
        if (mem8(bus, object + 0x20u + slot) == chunk_index) {
            chunk = object + 0x4Cu +
                static_cast<std::uint32_t>(slot) * kFieldChunkBytes;
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

bool field_screen_entry(
    gba::GbaBus* bus, int bg, int hardware_x, int screen_y,
    std::uint16_t* out_entry) {
    if (!bus || !out_entry || bg < 0 || bg > 3)
        return false;
    resolve_field_objects(bus);
    const std::uint32_t object = g_field_objects[bg];
    if (!object)
        return false;

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
    return field_entry(
        bus, object, layer_scroll_x + hardware_x,
        layer_scroll_y + screen_y, out_entry);
}

int layer_scroll_x(gba::GbaBus* bus, int bg, std::uint32_t object) {
    const int chunk_x = static_cast<std::int32_t>(
        ewram32(bus, object + 0x24u));
    const std::uint32_t bg_io =
        0x010u + static_cast<std::uint32_t>(bg * 4);
    const int hofs = bus->io().read16(bg_io) & 0xFF;
    return chunk_x * 256 + hofs +
        (hofs < kFieldCacheBiasX ? 256 : 0);
}

bool field_content_x_bounds(
    gba::GbaBus* bus, std::uint32_t object,
    int* out_min_x, int* out_max_x) {
    if (!bus || !out_min_x || !out_max_x)
        return false;
    const std::uint32_t resource = ewram32(bus, object + 0x08u);
    const auto cached = g_content_x_bounds.find(resource);
    if (cached != g_content_x_bounds.end()) {
        *out_min_x = cached->second.first;
        *out_max_x = cached->second.second;
        return true;
    }

    const unsigned chunk_columns = mem8(bus, resource + 0x14u);
    const unsigned chunk_rows = mem8(bus, resource + 0x15u);
    if (chunk_columns == 0u || chunk_rows == 0u)
        return false;

    int minimum_tile = std::numeric_limits<int>::max();
    int maximum_tile = -1;
    for (unsigned chunk_y = 0; chunk_y < chunk_rows; ++chunk_y) {
        for (unsigned chunk_x = 0; chunk_x < chunk_columns; ++chunk_x) {
            const unsigned chunk_index =
                chunk_y * chunk_columns + chunk_x;
            const std::uint32_t descriptor =
                mem32(bus, resource + 0x18u + chunk_index * 4u);
            const std::uint8_t* decoded =
                decoded_field_chunk(bus, descriptor);
            if (!decoded)
                continue;
            for (unsigned local_y = 0; local_y < 32u; ++local_y) {
                for (unsigned local_x = 0; local_x < 32u; ++local_x) {
                    const unsigned offset =
                        (local_y * 32u + local_x) * 2u;
                    const std::uint16_t entry =
                        static_cast<std::uint16_t>(decoded[offset]) |
                        static_cast<std::uint16_t>(
                            decoded[offset + 1u] << 8);
                    if ((entry & 0x03FFu) == 0u)
                        continue;
                    const int tile_x = static_cast<int>(
                        chunk_x * 32u + local_x);
                    minimum_tile = std::min(minimum_tile, tile_x);
                    maximum_tile = std::max(maximum_tile, tile_x);
                }
            }
        }
    }
    if (maximum_tile < minimum_tile)
        return false;

    const std::pair<int, int> bounds{
        minimum_tile * 8, (maximum_tile + 1) * 8};
    g_content_x_bounds.emplace(resource, bounds);
    *out_min_x = bounds.first;
    *out_max_x = bounds.second;
    return true;
}

int calculate_view_shift(gba::GbaBus* bus) {
    if (!bus)
        return 0;
    resolve_field_objects(bus);
    int minimum = std::numeric_limits<int>::min();
    int maximum = std::numeric_limits<int>::max();
    bool constrained = false;
    for (int bg = 0; bg < 4; ++bg) {
        const std::uint32_t object = g_field_objects[bg];
        int content_min = 0;
        int content_max = 0;
        if (!object || !field_content_x_bounds(
                bus, object, &content_min, &content_max)) {
            continue;
        }
        const int scroll = layer_scroll_x(bus, bg, object);
        minimum = std::max(
            minimum, static_cast<int>(g_ws_extra_left) +
                content_min - scroll);
        maximum = std::min(
            maximum, content_max - scroll - 240 -
                static_cast<int>(g_ws_extra_right));
        constrained = true;
    }
    if (!constrained)
        return 0;

    // Keep the widened view inside the authored field. In ordinary areas the
    // centered framing (zero) already fits. Near either field boundary slide
    // the native viewport within the host output instead of requesting
    // nonexistent map pixels and then manufacturing a mirrored/repeated fill.
    if (minimum <= maximum) {
        if (minimum > 0)
            return minimum;
        if (maximum < 0)
            return maximum;
        return 0;
    }

    // A field narrower than the requested host view cannot fill it. Center
    // the authored resource; the tilemap provider will leave only the genuine
    // out-of-field remainder transparent.
    return minimum + (maximum - minimum) / 2;
}

extern "C" int buus_fury_bg_x_provider(
    int bg, int output_x, int screen_y, int* out_hardware_x) {
    gba::GbaBus* bus = active_bus();
    if (!out_hardware_x || bg < 0 || bg > 3 || !is_overworld(bus))
        return 0;

    const int hardware_x =
        output_x - static_cast<int>(g_ws_extra_left);
    // Rendering visits BG3 first and begins at output (0,0). Refresh the
    // framing once per composed frame rather than rescanning resource bounds
    // in this per-pixel callback.
    if (bg == 3 && output_x == 0 && screen_y == 0) {
        g_render_view_shift = calculate_view_shift(bus);
        g_previous_obj_x = g_current_obj_x;
        g_current_obj_x = {};
        if (++g_obj_x_generation == 0) {
            g_previous_obj_x = {};
            g_current_obj_x = {};
            g_actor_history = {};
            g_obj_x_generation = 1;
        }
    }

    // This is placement of the native viewport within a wider host surface,
    // not movement of the guest camera. Apply one screen-space displacement
    // to every layer; each layer's own scroll already encodes its parallax.
    const int shift = g_render_view_shift;
    if (shift == 0)
        return 0;

    *out_hardware_x = hardware_x + shift;
    return 1;
}

extern "C" int buus_fury_tilemap_provider(
    int bg, int hardware_x, int screen_y, std::uint16_t* out_entry) {
    gba::GbaBus* bus = active_bus();
    if (!out_entry || bg < 0 || bg > 3 || !is_overworld(bus))
        return gba::kWsTilemapUnavailable;

    if (!field_screen_entry(bus, bg, hardware_x, screen_y, out_entry))
        return gba::kWsTilemapUnavailable;
    return gba::kWsTilemapReplace;
}

extern "C" int buus_fury_obj_x_provider(
    int oam_index, std::uint16_t attr0, std::uint16_t attr1,
    std::uint16_t attr2, int* out_x) {
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

    const int view_shift = g_render_view_shift;
    // Expanded views make OAM X=256..511 ambiguous: those values may be
    // genuine coordinates in the new right margin or hardware-wrapped
    // negative coordinates leaving through the left edge. Resolve against
    // live actor anchors first. The renderer compacts surviving pieces into
    // different OAM slots as a composite actor leaves, so the continuity
    // fallback matches piece attributes across the complete previous frame
    // rather than assuming that an OAM index is a persistent identity.
    const int widened_right = 240 +
        static_cast<int>(g_ws_extra_right) + view_shift;
    int world_x = x <= widened_right ? x : x - 512;
    const int raw_y = y >= 160 ? y - 256 : y;
    bool resolved = match_actor_x(bus, x, raw_y, &world_x);
    if (oam_index >= 0 && oam_index < 128) {
        if (!resolved) {
            int best_distance = std::numeric_limits<int>::max();
            for (const ObjXHistory& history : g_previous_obj_x) {
                const bool same_piece =
                    history.valid &&
                    history.attr0_shape == (attr0 & 0xFF00u) &&
                    history.attr1_shape == (attr1 & 0xFE00u) &&
                    history.attr2 == attr2 &&
                    distance(history.raw_y, raw_y) <= 8;
                if (!same_piece) continue;

                const int candidates[] = {x, x - 512};
                for (const int candidate : candidates) {
                    const int dx = distance(candidate, history.resolved_x);
                    if (dx < best_distance) {
                        best_distance = dx;
                        world_x = candidate;
                    }
                }
            }
        }
        ObjXHistory& history =
            g_current_obj_x[static_cast<std::size_t>(oam_index)];
        history.attr0_shape = attr0 & 0xFF00u;
        history.attr1_shape = attr1 & 0xFE00u;
        history.attr2 = attr2;
        history.raw_y = raw_y;
        history.resolved_x = world_x;
        history.valid = true;
    }
    *out_x = world_x - view_shift;
    return 1;
}

extern "C" int buus_fury_bus_read_override(
    std::uint32_t pc, std::uint32_t address, std::uint32_t width,
    std::uint32_t original, std::uint32_t* out_value) {
    gba::GbaBus* bus = active_bus();
    if (!out_value || width != 4u || !is_overworld(bus))
        return 0;

    // The actor renderer intersects each object's world rectangle with the
    // camera rectangle at 0x030019B[C..C8]. 0x08010192 performs separate
    // tests for the main meta-sprite and its auxiliary OAM piece; both must
    // use the widened bounds or composite NPCs visibly shed pieces at the
    // edge. Retain the older generic-object path for other overworld scenes.
    const bool visibility_left =
        pc == 0x08019C06u ||
        pc == 0x08010216u ||
        pc == 0x0801029Cu;
    const bool visibility_right =
        pc == 0x08019BFEu ||
        pc == 0x08010210u ||
        pc == 0x08010292u;
    if (!visibility_left && !visibility_right)
        return 0;

    remember_actor(bus, g_cpu.R[4]);

    // Account for asymmetric boundary framing while leaving the camera,
    // streaming, and actor coordinates byte-for-byte native.
    const int view_shift = calculate_view_shift(bus);
    if (visibility_left && address == kCameraLeft) {
        const int extension =
            static_cast<int>(g_ws_extra_left) - view_shift +
            kActorCullPadding;
        *out_value = original - static_cast<std::uint32_t>(extension);
        return 1;
    }
    if (visibility_right && address == kCameraRight) {
        const int extension =
            static_cast<int>(g_ws_extra_right) + view_shift +
            kActorCullPadding;
        *out_value = original + static_cast<std::uint32_t>(extension);
        return 1;
    }
    return 0;
}

}  // namespace

void install_extended_view(std::uint32_t, std::uint32_t) {
    for (std::uint32_t& object : g_field_objects)
        object = 0u;
    g_decoded_chunks.clear();
    g_content_x_bounds.clear();
    g_render_view_shift = 0;
    g_previous_obj_x = {};
    g_current_obj_x = {};
    g_actor_history = {};
    g_actor_history_cursor = 0;
    g_obj_x_generation = 1;
    gba::g_ws_tilemap_provider = &buus_fury_tilemap_provider;
    gba::g_ws_authored_margin_layers = 1;
    gba::g_ws_bg_x_provider = &buus_fury_bg_x_provider;
    gba::g_ws_bg_x_provider_layers =
        (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3);
    gba::g_ws_obj_attr_x_provider = &buus_fury_obj_x_provider;
    g_runtime_bus_read_override = &buus_fury_bus_read_override;
    std::fprintf(stderr,
        "[buus-fury:view] authored BG0..BG3 field continuation + adaptive "
        "boundary framing + exact chunk decode + edge HUD + expanded actor "
        "visibility enabled\n");
}

}  // namespace buus_fury
