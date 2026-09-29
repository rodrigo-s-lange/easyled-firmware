#include "pixels_payload_adapter.hpp"

#include <cstring>
#include "cJSON.h"
#include "esp_bit_defs.h"
#include "esp_pixels_fx.hpp"
#include "esp_pixels_fx_ids.hpp"
#include "soc/soc_caps.h"

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

bool validOutputPin(uint32_t pin)
{
    return pin < SOC_GPIO_PIN_COUNT && ((SOC_GPIO_VALID_OUTPUT_GPIO_MASK >> pin) & 1ULL) != 0 &&
           !(pin >= 6 && pin <= 11) && pin != 1 && pin != 3;
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
        if (slot_result != ValueResult::Ok || slot_index != static_cast<uint32_t>(index) ||
            !copyString(member(entry, "type"), type, sizeof(type)) || !cJSON_IsObject(config)) {
            return OutputsResult::Malformed;
        }
        if (std::strcmp(type, "digital_out") != 0) { return OutputsResult::Unsupported; }
        uint32_t gpio = 0, pixel_count = 0;
        bool reversed = false;
        const cJSON *reversed_item = member(config, "reversed");
        const ValueResult gpio_result = unsignedValue(member(config, "gpio"), 0, SOC_GPIO_PIN_COUNT - 1, gpio);
        const ValueResult pixels_result = unsignedValue(member(config, "pixel_count"), 1,
                                                        kMaxPixelsPerOutput, pixel_count);
        if (gpio_result == ValueResult::OutOfRange || pixels_result == ValueResult::OutOfRange) {
            return OutputsResult::OutOfRange;
        }
        if (gpio_result != ValueResult::Ok || pixels_result != ValueResult::Ok ||
            !copyString(member(config, "strip_type"), strip_type, sizeof(strip_type)) ||
            !copyString(member(config, "color_order"), color_order, sizeof(color_order))) {
            return OutputsResult::Malformed;
        }
        if (!validOutputPin(gpio)) { return OutputsResult::OutOfRange; }
        for (int prior = 0; prior < index; ++prior) {
            if (candidate.outputs[prior].gpio == gpio) { return OutputsResult::OutOfRange; }
        }
        if (std::strcmp(strip_type, "WS2812B") != 0) { return OutputsResult::Unsupported; }
        if (!cJSON_IsBool(reversed_item)) { return OutputsResult::Malformed; }
        reversed = cJSON_IsTrue(reversed_item);
        if (reversed) { return OutputsResult::Unsupported; } // physical reversed:true, never silently ignored
        esp_pixels::PixelOrder order{};
        if (!parsePixelOrder(color_order, order)) { return OutputsResult::Unsupported; }
        candidate.outputs[index].gpio = static_cast<uint16_t>(gpio);
        candidate.outputs[index].pixel_count = static_cast<uint16_t>(pixel_count);
        candidate.outputs[index].order = order;
        candidate.output_count = static_cast<uint8_t>(index + 1);
    }
    return OutputsResult::Ok;
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
// segment is Malformed; a segment this delivery does not implement
// (is_2d:true, an unregistered/dimension-mismatched effect id, or a
// present mirror/reversed/custom/color_source field) is Unsupported.
SegmentResult parseSegment(const cJSON *entry, comp::SegmentSpec &segment, bool seen_index[kMaxSegments])
{
    if (!cJSON_IsObject(entry)) { return SegmentResult::Malformed; }
    char device_id[37]{};
    bool is_2d = false;
    uint32_t segment_index = 0, effect_id = 0, start_px = 0, stop_px = 0;
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
    value_result = unsignedValue(member(entry, "start_px"), 0,
                                 kMaxOutputs * kMaxPixelsPerOutput, start_px);
    if (value_result == ValueResult::OutOfRange) { return SegmentResult::OutOfRange; }
    if (value_result != ValueResult::Ok) { return SegmentResult::Malformed; }
    value_result = unsignedValue(member(entry, "stop_px"), 0,
                                 kMaxOutputs * kMaxPixelsPerOutput, stop_px);
    if (value_result == ValueResult::OutOfRange) { return SegmentResult::OutOfRange; }
    if (value_result != ValueResult::Ok) { return SegmentResult::Malformed; }
    if (start_px >= stop_px) { return SegmentResult::OutOfRange; }
    is_2d = cJSON_IsTrue(is_2d_item);
    if (is_2d) { return SegmentResult::Unsupported; } // this delivery's fixture set is 1D only
    const fx::FxInfo *info = fx::describe(static_cast<uint8_t>(effect_id));
    if (info == nullptr || (info->dim & fx::FX_DIM_1D) == 0) {
        return SegmentResult::Unsupported; // unregistered id, or a 2D-only effect on a 1D segment
    }
    if (info->work.fixed_bytes != 0 || info->work.bytes_per_cell != 0) {
        return SegmentResult::Unsupported; // a working-buffer effect, not bound by Composer yet
    }
    segment = comp::SegmentSpec{};
    segment.id = segment_index + 1; // Composer reserves id 0 for ephemeral segments
    segment.start = static_cast<uint16_t>(start_px);
    segment.stop = static_cast<uint16_t>(stop_px);
    segment.effect_id = static_cast<uint8_t>(effect_id);
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
    // `ledmap` establishes physical topology, so only `apply_config` (which
    // also owns `outputs`) must carry it at all; a preset never touches
    // topology and may omit it entirely. When present for either payload
    // kind, it must still be null -- no canonical 2D ledmap schema exists
    // yet in this increment.
    if (want_outputs && ledmap == nullptr) {
        error = SchemaError::Malformed;
        return ESP_ERR_INVALID_ARG;
    }
    if (ledmap != nullptr && !cJSON_IsNull(ledmap)) {
        error = SchemaError::Unsupported;
        return ESP_ERR_NOT_SUPPORTED;
    }
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
    } else {
        // A preset must not redefine physical outputs -- present and
        // non-empty is rejected outright, never silently kept/ignored.
        if (outputs != nullptr) {
            if (!cJSON_IsArray(outputs)) { error = SchemaError::Malformed; return ESP_ERR_INVALID_ARG; }
            if (cJSON_GetArraySize(outputs) != 0) { error = SchemaError::Unsupported; return ESP_ERR_NOT_SUPPORTED; }
        }
        candidate.has_outputs = false;
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
        const SegmentResult result = parseSegment(entry, segment, seen_index);
        if (result == SegmentResult::Malformed) { error = SchemaError::Malformed; return ESP_ERR_INVALID_ARG; }
        if (result == SegmentResult::OutOfRange) { error = SchemaError::OutOfRange; return ESP_ERR_INVALID_ARG; }
        if (result == SegmentResult::Unsupported) { error = SchemaError::Unsupported; return ESP_ERR_NOT_SUPPORTED; }
        candidate.scene.segments[written] = segment;
        ++written;
    }
    candidate.scene.count = static_cast<uint8_t>(written);
    if (want_outputs) {
        uint32_t total_pixels = 0;
        for (uint8_t i = 0; i < candidate.output_count; ++i) {
            total_pixels += candidate.outputs[i].pixel_count;
        }
        for (uint8_t i = 0; i < candidate.scene.count; ++i) {
            if (candidate.scene.segments[i].stop > total_pixels) {
                error = SchemaError::OutOfRange;
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
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
