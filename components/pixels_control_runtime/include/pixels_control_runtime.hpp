#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "firmware_contract.hpp"
#include "network_session.hpp"
#include "pixels_command_executor.hpp"
#include "pixels_durable_storage.hpp"
#include "pixels_payload_adapter.hpp"

namespace pixels_control {

/// One immutable resource available to EmbeddedPayloadSource.
struct EmbeddedResource {
    pixels_firmware::CommandType type = pixels_firmware::CommandType::ApplyConfig;
    /// Config URL for ApplyConfig, or preset UUID for ActivatePreset.
    const char *key = nullptr;
    /// Exact payload bytes. The backing storage must outlive the source.
    const char *bytes = nullptr;
    size_t size = 0;
    /// Expected SHA-256 digest. The backing storage must outlive the source.
    const uint8_t *checksum = nullptr;
};

/// Bounded application payload source for versioned embedded development
/// fixtures. It is the offline substitute for the pending authenticated HTTPS
/// delivery endpoint and accepts only exact configured URLs/preset UUIDs.
class EmbeddedPayloadSource final : public pixels_executor::PayloadSource {
public:
    /// Creates a source over immutable application-owned entries.
    /// @param resources Entry table that must outlive this object.
    /// @param count Number of entries in resources.
    EmbeddedPayloadSource(const EmbeddedResource *resources, size_t count);

    /// Resolves the exact entry matching the command URL or preset UUID.
    /// @param command Parsed command.
    /// @param buffer Destination buffer.
    /// @param capacity Destination capacity.
    /// @param size Receives the exact byte count.
    /// @param expected_checksum Receives the entry digest.
    /// @return ESP_OK, ESP_ERR_NOT_FOUND, ESP_ERR_INVALID_ARG, or ESP_ERR_INVALID_SIZE.
    esp_err_t resolve(const pixels_firmware::Command &command, char *buffer, size_t capacity,
                      size_t &size,
                      uint8_t expected_checksum[pixels_firmware::kSha256Bytes]) override;

private:
    const EmbeddedResource *resources_ = nullptr;
    size_t count_ = 0;
};

/// Publishes executor events through the application-owned MQTT session.
class MqttEventSink final : public pixels_executor::EventSink {
public:
    /// Binds an exact event topic whose string must outlive this object.
    /// @param session Started application MQTT session.
    /// @param topic Exact event topic without wildcards.
    MqttEventSink(pixels_network::NetworkSession &session, const char *topic);

    /// Enqueues one non-retained QoS1 event.
    /// @param event_json Serialized event bytes.
    /// @param size Event byte count.
    /// @return ESP_OK when MQTT accepts the enqueue, otherwise ESP_FAIL.
    esp_err_t publish(const char *event_json, size_t size) override;

private:
    pixels_network::NetworkSession &session_;
    const char *topic_ = nullptr;
};

/// Publishes compact retained state and non-retained capability reports over
/// the application-owned MQTT session. Single control-task use only.
class MqttTelemetrySink final {
public:
    /// Binds the exact state and event topics; both strings must outlive this object.
    /// @param session Started application MQTT session.
    /// @param state_topic Exact retained state topic without wildcards.
    /// @param event_topic Exact event topic used by capability_reported.
    MqttTelemetrySink(pixels_network::NetworkSession &session, const char *state_topic,
                      const char *event_topic);

    /// Serializes and enqueues retained current state at QoS1.
    /// @param state Current device truth.
    /// @return ESP_OK, a serialization error, or ESP_FAIL when MQTT rejects it.
    esp_err_t publishState(const pixels_firmware::StateReport &state);

    /// Serializes and enqueues a non-retained capability_reported event at QoS1.
    /// @param report Current capability measurement.
    /// @return ESP_OK, a serialization error, or ESP_FAIL when MQTT rejects it.
    esp_err_t publishCapability(const pixels_firmware::CapabilityReport &report);

private:
    pixels_network::NetworkSession &session_;
    const char *state_topic_ = nullptr;
    const char *event_topic_ = nullptr;
    char buffer_[pixels_firmware::kMaxCapabilityBytes]{};
};

/// Supplies UTC wall-clock timestamps after network session time bootstrap.
class UtcTimestampSource final : public pixels_executor::TimestampSource {
public:
    /// Writes an RFC3339 UTC timestamp with whole-second precision.
    /// @param out Destination for YYYY-MM-DDTHH:MM:SSZ plus NUL.
    void now(char out[36]) override;
};

/// Exposes the durable staging layer's active preset identity to the executor.
class DurableActiveProbe final : public pixels_executor::ActiveTargetProbe {
public:
    /// Binds initialized durable staging.
    /// @param staging Staging object that outlives this probe.
    explicit DurableActiveProbe(const pixels_storage::NvsDurableStaging &staging);

    /// Checks the active preset UUID and digest.
    /// @param preset_id Requested preset UUID.
    /// @param checksum Requested payload digest.
    /// @return True only when both match durable active state.
    bool alreadyActive(const char *preset_id,
                       const uint8_t checksum[pixels_firmware::kSha256Bytes]) const override;

private:
    const pixels_storage::NvsDurableStaging &staging_;
};

/// Applies persisted pixel payloads to one already-built, managed Composer.
/// The physical output topology is fixed for the Composer lifetime; config
/// payloads must match it exactly, while presets inherit it.
class ComposerActivationTarget final : public pixels_storage::ActivationTarget {
public:
    /// Creates a target over a running Composer and its fixed physical outputs.
    /// @param composer Built Composer whose managed render task adopts scenes.
    /// @param outputs Fixed output table copied by this object.
    /// @param output_count Number of configured outputs, one or two.
    ComposerActivationTarget(esp_pixels_composer::Composer &composer,
                             const pixels_payload::OutputCandidate *outputs,
                             uint8_t output_count);

    /// Parses, topology-checks and confirms adoption of persisted bytes.
    /// @param type ApplyConfig or ActivatePreset.
    /// @param payload Exact payload JSON bytes.
    /// @param size Payload byte count.
    /// @return ESP_OK only after direct reclaim proves frame-boundary adoption.
    esp_err_t activateAtFrameBoundary(pixels_firmware::CommandType type,
                                       const char *payload, size_t size) override;

    /// Publishes and confirms an empty segment table.
    /// @return ESP_OK only after direct reclaim proves frame-boundary clearing.
    esp_err_t clearAtFrameBoundary() override;

private:
    bool topologyMatches(const pixels_payload::Candidate &candidate) const;
    esp_err_t applyAndConfirm(const esp_pixels_composer::AtScene &scene);

    esp_pixels_composer::Composer &composer_;
    pixels_payload::OutputCandidate outputs_[pixels_payload::kMaxOutputs]{};
    uint8_t output_count_ = 0;
    pixels_payload::Candidate candidate_{};
};

} // namespace pixels_control
