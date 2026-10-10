#pragma once

#include <cstddef>
#include <cstdint>
#include "esp_err.h"
#include "esp_pixels.hpp"
#include "esp_pixels_composer.hpp"
#include "esp_pixels_composer_at_scene.hpp"

// Bounded, application-owned adapter that turns exact `apply_config`/
// `activate_preset` payload JSON bytes into a validated pixel candidate.
// Reuses esp_pixels_fx::describe() as the ONE effect catalog (never a
// second one) and esp_pixels_composer::AtScene as the ONE segment
// representation (never a second scene struct). Independent of MQTT,
// HTTP, storage and clock selection -- this component only ever sees
// bytes already resolved and checksum-verified by its caller.

namespace pixels_payload {

/// Matches esp_pixels_composer_adapter_esp_pixels::EspPixelsSink::kMaxOutputs
/// -- restated here, not included from the adapter header, since this
/// component must not depend on a specific PixelFrameSink implementation.
constexpr uint8_t kMaxOutputs = 2;
/// Matches esp_pixels_composer::MAX_SEGMENTS.
constexpr uint8_t kMaxSegments = esp_pixels_composer::MAX_SEGMENTS;
/// Application-owned upper bound on one output's own pixel count -- not an
/// esp_pixels_composer_adapter_esp_pixels limit (that adapter only rejects
/// `pixel_count == 0`); chosen generously for any realistic bench/product
/// strip while still bounding a malformed/hostile payload's own arithmetic.
constexpr uint16_t kMaxPixelsPerOutput = 2048;
/// Application-owned upper bound on a whole config/preset payload's own
/// raw byte count, checked before cJSON ever parses it.
constexpr size_t kMaxPayloadBytes = 8192;
enum class SchemaError : uint8_t {
    None,
    /// Invalid JSON, wrong member type, duplicate consumed member, or a
    /// required member missing entirely.
    Malformed,
    /// A consumed numeric member's value (or fractional integer) is outside
    /// the real application/Composer limit for that field.
    OutOfRange,
    /// A structurally valid member this delivery does not implement yet:
    /// a non-empty `inputs`, a physical output `reversed:true`, a present
    /// segment-level `mirror`/`reversed`/
    /// `custom`/`color_source` field (regardless of its own value), an
    /// unregistered or dimension-mismatched `effect_id`, or a preset
    /// payload's own non-empty `outputs`, or a 2D preset before the active
    /// canvas is supplied to this adapter. Never silently applied/ignored.
    Unsupported,
};

/// One logical output slot from a config payload's own `outputs[]` entry.
struct OutputCandidate {
    /// Logical hardware slot from the platform contract. GPIO binding belongs
    /// to the application board profile and never comes from remote JSON.
    uint8_t slot_index = 0;
    uint16_t pixel_count = 0;
    esp_pixels::PixelOrder order = esp_pixels::PixelOrder::GRB;
};

/// Validated canvas topology summary. The raw table stays in the exact durable
/// payload and is reparsed by the activation layer when it rebuilds Composer.
struct LedmapCandidate {
    bool configured = false; ///< False means Composer's flat identity canvas.
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t entry_count = 0;
};

/// Fixed-capacity, application-owned candidate produced from validated
/// payload bytes. Never placed on a small FreeRTOS task stack by a caller --
/// `sizeof(Candidate)` is dominated by `AtScene`'s own `MAX_SEGMENTS` array.
struct Candidate {
    /// True only for a candidate produced by parseConfigPayload() -- a
    /// preset candidate (parsePresetPayload()) never redefines outputs, so
    /// a caller must keep whatever outputs are already active/durable.
    bool has_outputs = false;
    uint8_t output_count = 0;
    OutputCandidate outputs[kMaxOutputs]{};
    LedmapCandidate ledmap{};
    /// Segments + count + global brightness. `paused` is always left at its
    /// own default (false): pause is a separate, unrelated control this
    /// provisional contract does not expose yet.
    esp_pixels_composer::AtScene scene{};
};

/// Parses and validates one complete `apply_config` payload (outputs, global
/// brightness, full scene) into `candidate`. Every consumed JSON member
/// rejects a duplicate name, a wrong type, a fractional value for an integer
/// field, and a value outside the real application/Composer limit; unknown
/// members are accepted for forward compatibility. `segment_index` (0..31)
/// is unique and maps to Composer stable id `segment_index + 1` (id 0 is
/// reserved for ephemeral segments). Colors are strict `#RRGGBB`; blend
/// names map to the existing `BlendMode`; an effect parameter omitted from
/// a segment's own `config` object keeps `FxParams`'s real default.
/// `ledmap:null` means identity. A version-1 ledmap validates dimensions, map
/// length, holes, physical-index range and uniqueness. `inputs` must be empty.
/// Missing/wrong-type required members are malformed. A physical output
/// `reversed:true`, or a present segment-level `mirror`/
/// `reversed`/`custom`/`color_source` field are rejected the same way,
/// never silently ignored or half-applied. Configs require one or two
/// outputs. Remote configuration identifies logical `slot_index` values only;
/// the application board profile owns GPIO binding. Every 1D segment must fit
/// inside the configured logical span, and every 2D region must fit the ledmap.
/// @param json UTF-8 JSON bytes; no trailing NUL required.
/// @param size Exact byte count, at most kMaxPayloadBytes.
/// @param candidate Receives the validated candidate (has_outputs == true) on success only.
/// @param error Receives a stable rejection category; SchemaError::None on success.
/// @return ESP_OK, or ESP_ERR_INVALID_ARG/SIZE/NOT_SUPPORTED.
esp_err_t parseConfigPayload(const char *json, size_t size, Candidate &candidate, SchemaError &error);

/// Parses and validates one complete `activate_preset` payload (a
/// replacement scene only, never outputs) into `candidate`. A non-empty
/// `outputs` member is rejected as SchemaError::Unsupported -- a preset must
/// not redefine physical outputs. `ledmap` may be omitted entirely (a preset
/// never touches topology); when present it must be null. This entry point
/// accepts 1D preset segments only because it has no active canvas argument;
/// 2D preset activation remains fail-closed until that context is wired.
/// @param json UTF-8 JSON bytes; no trailing NUL required.
/// @param size Exact byte count, at most kMaxPayloadBytes.
/// @param candidate Receives the validated candidate (has_outputs == false) on success only.
/// @param error Receives a stable rejection category; SchemaError::None on success.
/// @return ESP_OK, or ESP_ERR_INVALID_ARG/SIZE/NOT_SUPPORTED.
esp_err_t parsePresetPayload(const char *json, size_t size, Candidate &candidate, SchemaError &error);

/// Returns the stable wire-oriented reason code for a schema rejection.
/// @param error Rejection category.
/// @return Static NUL-terminated reason code.
const char *schemaErrorCode(SchemaError error);

} // namespace pixels_payload
