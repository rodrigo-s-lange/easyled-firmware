#include "pixels_payload_adapter.hpp"

#include <cstring>
#include "cJSON.h"
#include "esp_pixels_fx.hpp"
#include "esp_pixels_fx_ids.hpp"

namespace pixels_payload {
namespace {

namespace comp = esp_pixels_composer;
namespace fx = esp_pixels_fx;

// Mirrors pixels_firmware::kUuidBytes (36) -- this component intentionally
// does not depend on firmware_contract.hpp (it parses a different payload
// and must stay independently testable), so the constant is restated here,
// not included.
constexpr size_t kUuidBytes = 36;

// --- Small cJSON helpers ---------------------------------------------------
//
// Deliberately duplicated from pixels_firmware_contract.cpp's own equally
// small, file-static helpers rather than making that component's internals
// public: this adapter parses a DIFFERENT payload (the downloaded config/
// preset bytes, not the command envelope), and the task brief's own "do not
// create a second JSON parser" instruction is about the command envelope
// parser specifically (there is still exactly one of those). These few
// generic cJSON primitives are small enough that duplicating them here costs
// far less than exporting firmware_contract's own internals would.

enum class MemberResult : uint8_t { Missing, Found, Duplicate };

MemberResult findMember(const cJSON *object, const char *name, const cJSON *&found)
{
    found = nullptr;
    if (!cJSON_IsObject(object)) { return MemberResult::Missing; }
    for (const cJSON *item = object->child; item != nullptr; item = item->next) {
        if (item->string != nullptr && std::strcmp(item->string, name) == 0) {
            if (found != nullptr) { return MemberResult::Duplicate; }
            found = item;
        }
    }
    return found == nullptr ? MemberResult::Missing : MemberResult::Found;
}

const cJSON *member(const cJSON *object, const char *name)
{
    const cJSON *found = nullptr;
    return findMember(object, name, found) == MemberResult::Found ? found : nullptr;
}

bool hasMember(const cJSON *object, const char *name)
{
    // Presence-only probe for a field this delivery does not implement yet
    // (mirror/segment-reversed/custom/color_source) -- deliberately does not
    // route through member()'s own duplicate-rejection, since ANY presence
    // (duplicated or not) must reject as Unsupported, not Malformed.
    if (!cJSON_IsObject(object)) { return false; }
    for (const cJSON *item = object->child; item != nullptr; item = item->next) {
        if (item->string != nullptr && std::strcmp(item->string, name) == 0) { return true; }
    }
    return false;
}

bool copyString(const cJSON *item, char *output, size_t capacity, bool allow_empty = false)
{
    if (!cJSON_IsString(item) || item->valuestring == nullptr || capacity == 0) { return false; }
    const size_t length = strnlen(item->valuestring, capacity);
    if (length >= capacity || (!allow_empty && length == 0)) { return false; }
    std::memcpy(output, item->valuestring, length + 1);
    return true;
}

bool isHex(char value)
{
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

uint8_t hexValue(char value)
{
    if (value >= '0' && value <= '9') { return static_cast<uint8_t>(value - '0'); }
    if (value >= 'a' && value <= 'f') { return static_cast<uint8_t>(value - 'a' + 10); }
    return static_cast<uint8_t>(value - 'A' + 10);
}

bool uuid(const char *value)
{
    if (value == nullptr || std::strlen(value) != kUuidBytes) { return false; }
    for (size_t i = 0; i < kUuidBytes; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') { return false; }
        } else if (!isHex(value[i])) {
            return false;
        }
    }
    return true;
}

enum class ValueResult : uint8_t { Ok, Malformed, OutOfRange };

ValueResult unsignedValue(const cJSON *item, uint32_t minimum, uint32_t maximum, uint32_t &value)
{
    if (!cJSON_IsNumber(item)) { return ValueResult::Malformed; }
    if (item->valuedouble < static_cast<double>(minimum) ||
        item->valuedouble > static_cast<double>(maximum)) { return ValueResult::OutOfRange; }
    const uint32_t candidate = static_cast<uint32_t>(item->valuedouble);
    if (item->valuedouble != static_cast<double>(candidate)) { return ValueResult::OutOfRange; }
    value = candidate;
    return ValueResult::Ok;
}

ValueResult optionalUnsigned(const cJSON *object, const char *name, uint32_t minimum,
                             uint32_t maximum, uint32_t &value)
{
    const cJSON *item = nullptr;
    const MemberResult found = findMember(object, name, item);
    if (found == MemberResult::Missing) { return ValueResult::Ok; }
    if (found == MemberResult::Duplicate) { return ValueResult::Malformed; }
    return unsignedValue(item, minimum, maximum, value);
}

bool strictHexColor(const char *text, comp::CRGB &out)
{
    if (text == nullptr || std::strlen(text) != 7 || text[0] != '#') { return false; }
    for (int i = 1; i < 7; ++i) {
        if (!isHex(text[i])) { return false; }
    }
    out.r = static_cast<uint8_t>((hexValue(text[1]) << 4) | hexValue(text[2]));
    out.g = static_cast<uint8_t>((hexValue(text[3]) << 4) | hexValue(text[4]));
    out.b = static_cast<uint8_t>((hexValue(text[5]) << 4) | hexValue(text[6]));
    return true;
}

bool parseBlend(const char *text, comp::BlendMode &out)
{
    if (text == nullptr) { return false; }
    if (std::strcmp(text, "replace") == 0) { out = comp::BlendMode::Replace; return true; }
    if (std::strcmp(text, "add") == 0) { out = comp::BlendMode::Add; return true; }
    if (std::strcmp(text, "alpha") == 0) { out = comp::BlendMode::Alpha; return true; }
    return false;
}

bool parsePixelOrder(const char *text, esp_pixels::PixelOrder &out)
{
    if (text == nullptr) { return false; }
    if (std::strcmp(text, "RGB") == 0) { out = esp_pixels::PixelOrder::RGB; return true; }
    if (std::strcmp(text, "RBG") == 0) { out = esp_pixels::PixelOrder::RBG; return true; }
    if (std::strcmp(text, "GRB") == 0) { out = esp_pixels::PixelOrder::GRB; return true; }
    if (std::strcmp(text, "GBR") == 0) { out = esp_pixels::PixelOrder::GBR; return true; }
    if (std::strcmp(text, "BRG") == 0) { out = esp_pixels::PixelOrder::BRG; return true; }
    if (std::strcmp(text, "BGR") == 0) { out = esp_pixels::PixelOrder::BGR; return true; }
    return false;
}

// --- outputs[] --------------------------------------------------------------

// Result of parsing the outputs[] array: four-way so a genuinely absent
// `outputs` array (an empty preset) is never confused with a present-but-
// invalid one, and exceeding the real `kMaxOutputs` limit is reported as
// out-of-range rather than lumped into a generic malformed category.
enum class OutputsResult : uint8_t { Ok, Malformed, OutOfRange, Unsupported };

OutputsResult parseOutputs(const cJSON *outputs_array, Candidate &candidate)
{
    if (!cJSON_IsArray(outputs_array)) { return OutputsResult::Malformed; }
    const int count = cJSON_GetArraySize(outputs_array);
    if (count < 0) { return OutputsResult::Malformed; }
    if (count == 0 || count > kMaxOutputs) { return OutputsResult::OutOfRange; }
    candidate.output_count = 0;
    bool seen_slots[kMaxOutputs] = {};
    int index = 0;
    for (const cJSON *entry = outputs_array->child; entry != nullptr; entry = entry->next, ++index) {
        // Defensive bound, even though `count <= kMaxOutputs` was already
        // checked above: never index `candidate.outputs[]` past its own
        // fixed capacity, regardless of how `count` was computed.
        if (index >= kMaxOutputs) { return OutputsResult::OutOfRange; }
        if (!cJSON_IsObject(entry)) { return OutputsResult::Malformed; }
        char type[24]{};
        char strip_type[16]{};
        char color_order[8]{};
        uint32_t slot_index = 0;
        const cJSON *config = member(entry, "config");
        const ValueResult slot_result = unsignedValue(member(entry, "slot_index"), 0, kMaxOutputs - 1,
                                                      slot_index);
        if (slot_result == ValueResult::OutOfRange) { return OutputsResult::OutOfRange; }
        if (slot_result != ValueResult::Ok || seen_slots[slot_index] ||
            !copyString(member(entry, "type"), type, sizeof(type)) || !cJSON_IsObject(config)) {
            return OutputsResult::Malformed;
        }
        if (std::strcmp(type, "digital_out") != 0) { return OutputsResult::Unsupported; }
        uint32_t pixel_count = 0;
        bool reversed = false;
        const cJSON *reversed_item = nullptr;
        const MemberResult reversed_result = findMember(config, "reversed", reversed_item);
        const ValueResult pixels_result = unsignedValue(member(config, "pixel_count"), 1,
                                                         kMaxPixelsPerOutput, pixel_count);
        if (pixels_result == ValueResult::OutOfRange) { return OutputsResult::OutOfRange; }
        if (pixels_result != ValueResult::Ok ||
            !copyString(member(config, "strip_type"), strip_type, sizeof(strip_type)) ||
            !copyString(member(config, "color_order"), color_order, sizeof(color_order))) {
            return OutputsResult::Malformed;
        }
        if (reversed_result == MemberResult::Duplicate ||
            (reversed_result == MemberResult::Found && !cJSON_IsBool(reversed_item))) {
            return OutputsResult::Malformed;
        }
        if (std::strcmp(strip_type, "WS2812B") != 0) { return OutputsResult::Unsupported; }
        reversed = reversed_result == MemberResult::Found && cJSON_IsTrue(reversed_item);
        if (reversed) { return OutputsResult::Unsupported; } // physical reversed:true, never silently ignored
        esp_pixels::PixelOrder order{};
        if (!parsePixelOrder(color_order, order)) { return OutputsResult::Unsupported; }
        candidate.outputs[index].slot_index = static_cast<uint8_t>(slot_index);
        candidate.outputs[index].pixel_count = static_cast<uint16_t>(pixel_count);
        candidate.outputs[index].order = order;
        seen_slots[slot_index] = true;
        candidate.output_count = static_cast<uint8_t>(index + 1);
    }
    return OutputsResult::Ok;
}

// --- ledmap -----------------------------------------------------------------

enum class LedmapResult : uint8_t { Ok, Malformed, OutOfRange };

LedmapResult parseLedmap(const cJSON *ledmap, uint32_t total_pixels, Candidate &candidate)
{
    candidate.ledmap = LedmapCandidate{};
    if (cJSON_IsNull(ledmap)) {
        if (total_pixels == 0 || total_pixels > UINT16_MAX) { return LedmapResult::OutOfRange; }
        candidate.ledmap.width = static_cast<uint16_t>(total_pixels);
        candidate.ledmap.height = 1;
        candidate.ledmap.entry_count = total_pixels;
        return LedmapResult::Ok;
    }
    if (!cJSON_IsObject(ledmap)) { return LedmapResult::Malformed; }

    uint32_t version = 0, width = 0, height = 0;
    const ValueResult version_result = unsignedValue(member(ledmap, "version"), 1, 1, version);
    const ValueResult width_result = unsignedValue(member(ledmap, "width"), 1, UINT16_MAX, width);
    const ValueResult height_result = unsignedValue(member(ledmap, "height"), 1, UINT16_MAX, height);
    if (version_result == ValueResult::OutOfRange || width_result == ValueResult::OutOfRange ||
        height_result == ValueResult::OutOfRange) {
        return LedmapResult::OutOfRange;
    }
    if (version_result != ValueResult::Ok || width_result != ValueResult::Ok ||
        height_result != ValueResult::Ok) {
        return LedmapResult::Malformed;
    }
    const uint64_t cell_count_64 = static_cast<uint64_t>(width) * height;
    const uint32_t max_cells = static_cast<uint32_t>(kMaxOutputs) * kMaxPixelsPerOutput;
    if (cell_count_64 == 0 || cell_count_64 > max_cells) { return LedmapResult::OutOfRange; }
    const uint32_t cell_count = static_cast<uint32_t>(cell_count_64);

    const cJSON *map = member(ledmap, "map");
    if (!cJSON_IsArray(map)) { return LedmapResult::Malformed; }
    const int map_count = cJSON_GetArraySize(map);
    if (map_count < 0 || static_cast<uint32_t>(map_count) != cell_count) {
        return LedmapResult::OutOfRange;
    }
    int outer_index = 0;
    for (const cJSON *entry = map->child; entry != nullptr; entry = entry->next, ++outer_index) {
        if (!cJSON_IsNumber(entry) || entry->valuedouble < -1.0 ||
            entry->valuedouble >= static_cast<double>(total_pixels)) {
            return cJSON_IsNumber(entry) ? LedmapResult::OutOfRange : LedmapResult::Malformed;
        }
        const int32_t value = static_cast<int32_t>(entry->valuedouble);
        if (entry->valuedouble != static_cast<double>(value)) { return LedmapResult::OutOfRange; }
        if (value < 0) { continue; }
        int inner_index = 0;
        for (const cJSON *other = map->child; other != nullptr; other = other->next, ++inner_index) {
            if (inner_index <= outer_index) { continue; }
            if (cJSON_IsNumber(other) && other->valuedouble == static_cast<double>(value)) {
                return LedmapResult::OutOfRange;
            }
        }
    }

    char checksum[65]{};
    if (!copyString(member(ledmap, "checksum"), checksum, sizeof(checksum))) {
        return LedmapResult::Malformed;
    }
    for (size_t i = 0; i < 64; ++i) {
        if (!isHex(checksum[i])) { return LedmapResult::Malformed; }
    }
    candidate.ledmap.configured = true;
    candidate.ledmap.width = static_cast<uint16_t>(width);
    candidate.ledmap.height = static_cast<uint16_t>(height);
    candidate.ledmap.entry_count = cell_count;
    return LedmapResult::Ok;
}

// --- segments[] ---------------------------------------------------------

enum class SegmentResult : uint8_t { Ok, Malformed, OutOfRange, Unsupported };

SegmentResult parseSegmentEffectConfig(const cJSON *config, comp::SegmentSpec &segment)
{
    // Present at all -- regardless of value -- means this delivery's fixed
    // firmware defaults are not what the caller actually wants, and the
    // EasyLED wire name/type for the field is not closed yet: reject rather
    // than guess or silently keep the default.
    if (hasMember(config, "mirror") || hasMember(config, "reversed") ||
        hasMember(config, "custom") || hasMember(config, "color_source")) {
        return SegmentResult::Unsupported;
    }
    uint32_t brightness = 255, opacity = 255;
    ValueResult value_result = optionalUnsigned(config, "brightness", 0, 255, brightness);
    if (value_result == ValueResult::Malformed) { return SegmentResult::Malformed; }
    if (value_result == ValueResult::OutOfRange) { return SegmentResult::OutOfRange; }
    value_result = optionalUnsigned(config, "opacity", 0, 255, opacity);
    if (value_result == ValueResult::Malformed) { return SegmentResult::Malformed; }
    if (value_result == ValueResult::OutOfRange) { return SegmentResult::OutOfRange; }
    segment.brightness = static_cast<uint8_t>(brightness);
    segment.opacity = static_cast<uint8_t>(opacity);
    const cJSON *optional = nullptr;
    MemberResult member_result = findMember(config, "blend", optional);
    if (member_result == MemberResult::Duplicate) { return SegmentResult::Malformed; }
    if (member_result == MemberResult::Found) {
        char blend_text[16]{};
        comp::BlendMode blend{};
        if (!copyString(optional, blend_text, sizeof(blend_text))) { return SegmentResult::Malformed; }
        if (!parseBlend(blend_text, blend)) { return SegmentResult::Unsupported; }
        segment.blend = blend;
    }
    uint32_t palette = segment.params.palette;
    value_result = optionalUnsigned(config, "palette_id", 0, 255, palette);
    if (value_result == ValueResult::Malformed) { return SegmentResult::Malformed; }
    if (value_result == ValueResult::OutOfRange) { return SegmentResult::OutOfRange; }
    segment.params.palette = static_cast<uint8_t>(palette);
    uint32_t speed = segment.params.speed;
    value_result = optionalUnsigned(config, "speed", 0, 255, speed);
    if (value_result == ValueResult::Malformed) { return SegmentResult::Malformed; }
    if (value_result == ValueResult::OutOfRange) { return SegmentResult::OutOfRange; }
    segment.params.speed = static_cast<uint8_t>(speed);
    uint32_t intensity = segment.params.intensity;
    value_result = optionalUnsigned(config, "intensity", 0, 255, intensity);
    if (value_result == ValueResult::Malformed) { return SegmentResult::Malformed; }
    if (value_result == ValueResult::OutOfRange) { return SegmentResult::OutOfRange; }
    segment.params.intensity = static_cast<uint8_t>(intensity);

    member_result = findMember(config, "colors", optional);
    if (member_result == MemberResult::Duplicate) { return SegmentResult::Malformed; }
    if (member_result == MemberResult::Found) {
        if (!cJSON_IsArray(optional)) { return SegmentResult::Malformed; }
        const int color_count = cJSON_GetArraySize(optional);
        if (color_count < 0) { return SegmentResult::Malformed; }
        if (color_count > 3) { return SegmentResult::OutOfRange; }
        int slot = 0;
        for (const cJSON *entry = optional->child; entry != nullptr; entry = entry->next, ++slot) {
            if (!cJSON_IsString(entry) ||
                !strictHexColor(entry->valuestring, segment.params.color[slot])) {
                return SegmentResult::Malformed;
            }
        }
    }
    return SegmentResult::Ok;
}

// Four-way, same reasoning as OutputsResult: a structurally invalid
// segment is Malformed; an unregistered/dimension-mismatched effect id,
// 2D without a config-owned ledmap, or a present mirror/reversed/custom/
// color_source field is Unsupported.
SegmentResult parseSegment(const cJSON *entry, comp::SegmentSpec &segment,
                           bool seen_index[kMaxSegments], const LedmapCandidate &ledmap,
                           uint32_t total_pixels, bool allow_2d)
{
    if (!cJSON_IsObject(entry)) { return SegmentResult::Malformed; }
    char device_id[37]{};
    bool is_2d = false;
    uint32_t segment_index = 0, effect_id = 0;
    char updated_at[36]{};
    const cJSON *is_2d_item = member(entry, "is_2d");
    const cJSON *config = member(entry, "config");
    if (!copyString(member(entry, "device_id"), device_id, sizeof(device_id)) || !uuid(device_id) ||
        !cJSON_IsBool(is_2d_item) || !cJSON_IsObject(config) ||
        !copyString(member(entry, "updated_at"), updated_at, sizeof(updated_at))) {
        return SegmentResult::Malformed;
    }
    ValueResult value_result = unsignedValue(member(entry, "segment_index"), 0, kMaxSegments - 1,
                                             segment_index);
    if (value_result == ValueResult::OutOfRange) { return SegmentResult::OutOfRange; }
    if (value_result != ValueResult::Ok || seen_index[segment_index]) { return SegmentResult::Malformed; }
    value_result = unsignedValue(member(entry, "effect_id"), 0, 255, effect_id);
    if (value_result == ValueResult::OutOfRange) { return SegmentResult::OutOfRange; }
    if (value_result != ValueResult::Ok) { return SegmentResult::Malformed; }
    is_2d = cJSON_IsTrue(is_2d_item);
    const fx::FxInfo *info = fx::describe(static_cast<uint8_t>(effect_id));
    if (info == nullptr) { return SegmentResult::Unsupported; }
    segment = comp::SegmentSpec{};
    segment.id = segment_index + 1; // Composer reserves id 0 for ephemeral segments
    segment.effect_id = static_cast<uint8_t>(effect_id);
    segment.is2D = is_2d;
    if (is_2d) {
        if (!allow_2d || !ledmap.configured || (info->dim & fx::FX_DIM_2D) == 0) {
            return SegmentResult::Unsupported;
        }
        const cJSON *region = member(entry, "region");
        if (!cJSON_IsObject(region)) { return SegmentResult::Malformed; }
        uint32_t x = 0, y = 0, w = 0, h = 0;
        const ValueResult x_result = unsignedValue(member(region, "x"), 0, UINT16_MAX, x);
        const ValueResult y_result = unsignedValue(member(region, "y"), 0, UINT16_MAX, y);
        const ValueResult w_result = unsignedValue(member(region, "w"), 1, UINT16_MAX, w);
        const ValueResult h_result = unsignedValue(member(region, "h"), 1, UINT16_MAX, h);
        if (x_result == ValueResult::OutOfRange || y_result == ValueResult::OutOfRange ||
            w_result == ValueResult::OutOfRange || h_result == ValueResult::OutOfRange ||
            static_cast<uint64_t>(x) + w > ledmap.width ||
            static_cast<uint64_t>(y) + h > ledmap.height || h == 1) {
            return SegmentResult::OutOfRange;
        }
        if (x_result != ValueResult::Ok || y_result != ValueResult::Ok ||
            w_result != ValueResult::Ok || h_result != ValueResult::Ok) {
            return SegmentResult::Malformed;
        }
        segment.x = static_cast<uint16_t>(x);
        segment.y = static_cast<uint16_t>(y);
        segment.w = static_cast<uint16_t>(w);
        segment.h = static_cast<uint16_t>(h);
    } else {
        if ((info->dim & fx::FX_DIM_1D) == 0) { return SegmentResult::Unsupported; }
        uint32_t start_px = 0, stop_px = 0;
        value_result = unsignedValue(member(entry, "start_px"), 0,
                                     kMaxOutputs * kMaxPixelsPerOutput, start_px);
        if (value_result == ValueResult::OutOfRange) { return SegmentResult::OutOfRange; }
        if (value_result != ValueResult::Ok) { return SegmentResult::Malformed; }
        value_result = unsignedValue(member(entry, "stop_px"), 0,
                                     kMaxOutputs * kMaxPixelsPerOutput, stop_px);
        if (value_result == ValueResult::OutOfRange) { return SegmentResult::OutOfRange; }
        if (value_result != ValueResult::Ok) { return SegmentResult::Malformed; }
        if (start_px >= stop_px || stop_px > total_pixels) { return SegmentResult::OutOfRange; }
        segment.start = static_cast<uint16_t>(start_px);
        segment.stop = static_cast<uint16_t>(stop_px);
    }
    const SegmentResult config_result = parseSegmentEffectConfig(config, segment);
    if (config_result != SegmentResult::Ok) { return config_result; }
    seen_index[segment_index] = true;
    return SegmentResult::Ok;
}

esp_err_t parseScene(const cJSON *root, Candidate &candidate, SchemaError &error, bool want_outputs)
{
    if (!cJSON_IsObject(root)) {
        error = SchemaError::Malformed;
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *ledmap = nullptr;
    const cJSON *inputs = nullptr;
    const cJSON *outputs = nullptr;
    const cJSON *segments = nullptr;
    if (findMember(root, "ledmap", ledmap) == MemberResult::Duplicate ||
        findMember(root, "inputs", inputs) == MemberResult::Duplicate ||
        findMember(root, "outputs", outputs) == MemberResult::Duplicate ||
        findMember(root, "segments", segments) == MemberResult::Duplicate) {
        error = SchemaError::Malformed;
        return ESP_ERR_INVALID_ARG;
    }
    if (segments == nullptr) {
        error = SchemaError::Malformed;
        return ESP_ERR_INVALID_ARG;
    }
    // `ledmap` establishes topology, so apply_config must carry it. Presets
    // inherit the active topology and therefore cannot replace a real map.
    if (want_outputs && ledmap == nullptr) {
        error = SchemaError::Malformed;
        return ESP_ERR_INVALID_ARG;
    }
    if (!want_outputs && ledmap != nullptr && !cJSON_IsNull(ledmap)) {
        error = SchemaError::Unsupported;
        return ESP_ERR_NOT_SUPPORTED;
    }
    uint32_t total_pixels = static_cast<uint32_t>(kMaxOutputs) * kMaxPixelsPerOutput;
    if (want_outputs) {
        if (outputs == nullptr) { error = SchemaError::Malformed; return ESP_ERR_INVALID_ARG; }
        if (inputs == nullptr || !cJSON_IsArray(inputs)) {
            error = SchemaError::Malformed;
            return ESP_ERR_INVALID_ARG;
        }
        if (cJSON_GetArraySize(inputs) != 0) {
            error = SchemaError::Unsupported;
            return ESP_ERR_NOT_SUPPORTED;
        }
        uint32_t global_brightness = 255;
        const ValueResult brightness_result = unsignedValue(member(root, "global_brightness"), 0, 255,
                                                            global_brightness);
        if (brightness_result == ValueResult::OutOfRange) {
            error = SchemaError::OutOfRange;
            return ESP_ERR_INVALID_ARG;
        }
        if (brightness_result != ValueResult::Ok) {
            error = SchemaError::Malformed;
            return ESP_ERR_INVALID_ARG;
        }
        candidate.scene.brightness = static_cast<uint8_t>(global_brightness);
        const OutputsResult outputs_result = parseOutputs(outputs, candidate);
        if (outputs_result == OutputsResult::Malformed) { error = SchemaError::Malformed; return ESP_ERR_INVALID_ARG; }
        if (outputs_result == OutputsResult::OutOfRange) { error = SchemaError::OutOfRange; return ESP_ERR_INVALID_ARG; }
        if (outputs_result == OutputsResult::Unsupported) { error = SchemaError::Unsupported; return ESP_ERR_NOT_SUPPORTED; }
        candidate.has_outputs = true;
        total_pixels = 0;
        for (uint8_t i = 0; i < candidate.output_count; ++i) {
            total_pixels += candidate.outputs[i].pixel_count;
        }
        const LedmapResult ledmap_result = parseLedmap(ledmap, total_pixels, candidate);
        if (ledmap_result == LedmapResult::Malformed) { error = SchemaError::Malformed; return ESP_ERR_INVALID_ARG; }
        if (ledmap_result == LedmapResult::OutOfRange) { error = SchemaError::OutOfRange; return ESP_ERR_INVALID_ARG; }
    } else {
        // A preset must not redefine physical outputs -- present and
        // non-empty is rejected outright, never silently kept/ignored.
        if (outputs != nullptr) {
            if (!cJSON_IsArray(outputs)) { error = SchemaError::Malformed; return ESP_ERR_INVALID_ARG; }
            if (cJSON_GetArraySize(outputs) != 0) { error = SchemaError::Unsupported; return ESP_ERR_NOT_SUPPORTED; }
        }
        candidate.has_outputs = false;
        candidate.ledmap = LedmapCandidate{};
    }
    if (!cJSON_IsArray(segments)) { error = SchemaError::Malformed; return ESP_ERR_INVALID_ARG; }
    const int count = cJSON_GetArraySize(segments);
    if (count < 0 || count > kMaxSegments) {
        error = SchemaError::OutOfRange;
        return ESP_ERR_INVALID_ARG;
    }
    bool seen_index[kMaxSegments] = {};
    int written = 0;
    for (const cJSON *entry = segments->child; entry != nullptr; entry = entry->next) {
        comp::SegmentSpec segment{};
        const SegmentResult result = parseSegment(entry, segment, seen_index, candidate.ledmap,
                                                  total_pixels, want_outputs);
        if (result == SegmentResult::Malformed) { error = SchemaError::Malformed; return ESP_ERR_INVALID_ARG; }
        if (result == SegmentResult::OutOfRange) { error = SchemaError::OutOfRange; return ESP_ERR_INVALID_ARG; }
        if (result == SegmentResult::Unsupported) { error = SchemaError::Unsupported; return ESP_ERR_NOT_SUPPORTED; }
        candidate.scene.segments[written] = segment;
        ++written;
    }
    candidate.scene.count = static_cast<uint8_t>(written);
    error = SchemaError::None;
    return ESP_OK;
}

} // namespace

esp_err_t parseConfigPayload(const char *json, size_t size, Candidate &candidate, SchemaError &error)
{
    candidate = Candidate{};
    error = SchemaError::Malformed;
    if (json == nullptr || size == 0 || size > kMaxPayloadBytes) {
        error = size > kMaxPayloadBytes ? SchemaError::OutOfRange : SchemaError::Malformed;
        return ESP_ERR_INVALID_SIZE;
    }
    const char *end = nullptr;
    cJSON *root = cJSON_ParseWithLengthOpts(json, size, &end, false);
    if (root == nullptr || end != json + size) {
        cJSON_Delete(root);
        error = SchemaError::Malformed;
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t err = parseScene(root, candidate, error, /*want_outputs=*/true);
    cJSON_Delete(root);
    if (err != ESP_OK) { candidate = Candidate{}; return err; }
    return ESP_OK;
}

esp_err_t parsePresetPayload(const char *json, size_t size, Candidate &candidate, SchemaError &error)
{
    candidate = Candidate{};
    error = SchemaError::Malformed;
    if (json == nullptr || size == 0 || size > kMaxPayloadBytes) {
        error = size > kMaxPayloadBytes ? SchemaError::OutOfRange : SchemaError::Malformed;
        return ESP_ERR_INVALID_SIZE;
    }
    const char *end = nullptr;
    cJSON *root = cJSON_ParseWithLengthOpts(json, size, &end, false);
    if (root == nullptr || end != json + size) {
        cJSON_Delete(root);
        error = SchemaError::Malformed;
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t err = parseScene(root, candidate, error, /*want_outputs=*/false);
    cJSON_Delete(root);
    if (err != ESP_OK) { candidate = Candidate{}; return err; }
    return ESP_OK;
}

esp_err_t materializeLedmap(const char *json, size_t size, const LedmapCandidate &ledmap,
                            int32_t *entries, uint32_t capacity)
{
    if (json == nullptr || size == 0 || size > kMaxPayloadBytes || !ledmap.configured ||
        ledmap.width == 0 || ledmap.height == 0 || ledmap.entry_count == 0 ||
        entries == nullptr || capacity < ledmap.entry_count) {
        return size > kMaxPayloadBytes ? ESP_ERR_INVALID_SIZE : ESP_ERR_INVALID_ARG;
    }
    const char *end = nullptr;
    cJSON *root = cJSON_ParseWithLengthOpts(json, size, &end, false);
    if (root == nullptr || end != json + size || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *ledmap_object = nullptr;
    if (findMember(root, "ledmap", ledmap_object) != MemberResult::Found ||
        !cJSON_IsObject(ledmap_object)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t width = 0;
    uint32_t height = 0;
    const cJSON *map = member(ledmap_object, "map");
    if (unsignedValue(member(ledmap_object, "width"), 1, UINT16_MAX, width) != ValueResult::Ok ||
        unsignedValue(member(ledmap_object, "height"), 1, UINT16_MAX, height) != ValueResult::Ok ||
        width != ledmap.width || height != ledmap.height || !cJSON_IsArray(map) ||
        cJSON_GetArraySize(map) != static_cast<int>(ledmap.entry_count)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t index = 0;
    for (const cJSON *entry = map->child; entry != nullptr; entry = entry->next, ++index) {
        if (index >= ledmap.entry_count || !cJSON_IsNumber(entry) || entry->valuedouble < -1.0 ||
            entry->valuedouble > static_cast<double>(INT32_MAX)) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        const int32_t value = static_cast<int32_t>(entry->valuedouble);
        if (entry->valuedouble != static_cast<double>(value)) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        entries[index] = value;
    }
    cJSON_Delete(root);
    return index == ledmap.entry_count ? ESP_OK : ESP_ERR_INVALID_ARG;
}

const char *schemaErrorCode(SchemaError error)
{
    switch (error) {
    case SchemaError::None: return "none";
    case SchemaError::Malformed: return "malformed_payload";
    case SchemaError::OutOfRange: return "payload_out_of_range";
    case SchemaError::Unsupported: return "unsupported_schema";
    }
    return "malformed_payload";
}

} // namespace pixels_payload
