#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "firmware_contract.hpp"
#include "network_session.hpp"
#include "pixels_command_executor.hpp"
#include "pixels_durable_storage.hpp"
#include "pixels_payload_adapter.hpp"
#include "esp_pixels.hpp"
#include "esp_pixels_composer_adapter_esp_pixels.hpp"

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

/// One bounded authenticated HTTP download request. The URL, bearer token and
/// optional CA certificate are borrowed for the duration of download().
struct HttpPayloadRequest {
    const char *url = nullptr;
    const char *bearer_token = nullptr;
    const char *server_ca_pem = nullptr;
    uint32_t timeout_ms = 0;
    char *destination = nullptr;
    size_t capacity = 0;
};

/// Metadata returned after a complete HTTP response body has been received.
struct HttpPayloadResult {
    int status_code = 0;
    size_t size = 0;
    bool application_json = false;
    /// True when the response carried X-Config-Checksum.
    bool has_config_checksum = false;
    /// NUL-terminated hexadecimal response checksum when present.
    char config_checksum[pixels_firmware::kSha256HexBytes + 1]{};
};

/// Injectable HTTP boundary used by HttpsPayloadSource. Implementations must
/// never log request credentials or response bodies.
class HttpPayloadClient {
public:
    /// Releases an HTTP client through its interface.
    virtual ~HttpPayloadClient() = default;

    /// Downloads one complete response into a caller-owned bounded buffer.
    /// @param request HTTPS URL, credential, trust source, timeout and destination.
    /// @param result Receives status, byte count, content type and optional checksum header.
    /// @return ESP_OK after a complete HTTP exchange, or a transport/size error.
    virtual esp_err_t download(const HttpPayloadRequest &request,
                               HttpPayloadResult &result) = 0;
};

/// ESP-IDF HTTP implementation with mandatory TLS verification, disabled
/// redirects and bounded response collection. Single control-task use only.
class EspHttpPayloadClient final : public HttpPayloadClient {
public:
    /// Performs one authenticated GET using a supplied CA or the IDF bundle.
    /// @param request HTTPS request parameters and destination buffer.
    /// @param result Receives response metadata after the complete transfer.
    /// @return ESP_OK, ESP_ERR_INVALID_ARG/SIZE, or an ESP-IDF TLS/HTTP error.
    esp_err_t download(const HttpPayloadRequest &request,
                       HttpPayloadResult &result) override;

private:
    static constexpr size_t kAuthorizationBytes = 392;
    char authorization_[kAuthorizationBytes]{};
};

/// Authenticated HTTPS configuration resolver. It accepts apply_config only;
/// presets remain routed to EmbeddedPayloadSource until the platform publishes
/// its second download contract package.
class HttpsPayloadSource final : public pixels_executor::PayloadSource {
public:
    static constexpr size_t kMaxBearerTokenBytes = 384;

    /// Borrowed runtime configuration; no endpoint, credential or certificate
    /// is compiled into this component.
    struct Config {
        const char *bearer_token = nullptr;
        const char *server_ca_pem = nullptr;
        uint32_t timeout_ms = 10000;
        size_t max_payload_bytes = pixels_payload::kMaxPayloadBytes;
    };

    /// Creates a resolver over an injected HTTP client and runtime credentials.
    /// @param client HTTP implementation that outlives this source.
    /// @param config Borrowed strings that outlive this source.
    HttpsPayloadSource(HttpPayloadClient &client, const Config &config);

    /// Downloads, bounds and verifies one apply_config resource before returning it.
    /// The exact response bytes must match the command checksum and, when the
    /// server supplies X-Config-Checksum, that independently parsed digest too.
    /// @param command Parsed apply_config command whose HTTPS URL/checksum are authoritative.
    /// @param buffer Destination for exact canonical response bytes.
    /// @param capacity Destination capacity.
    /// @param size Receives the verified byte count on success only.
    /// @param expected_checksum Receives command.checksum on success only.
    /// @return ESP_OK, ESP_ERR_NOT_SUPPORTED for presets, an argument/size/checksum
    ///         error, ESP_FAIL for a non-200/JSON response, or a transport error.
    esp_err_t resolve(const pixels_firmware::Command &command, char *buffer, size_t capacity,
                      size_t &size,
                      uint8_t expected_checksum[pixels_firmware::kSha256Bytes]) override;

private:
    HttpPayloadClient &client_;
    Config config_{};
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

/// One immutable application-owned binding from a platform output slot to
/// physical board wiring. Remote payloads never supply or override the GPIO.
struct BoardOutputBinding {
    uint8_t slot_index = 0;
    int gpio = -1;
    uint16_t max_pixels = 0;
    /// Pixels cleared on a boot with no durable configuration. Zero excludes
    /// this binding from the safe blank topology.
    uint16_t safe_pixel_count = 0;
    esp_pixels::PixelOrder safe_order = esp_pixels::PixelOrder::GRB;
};

/// Immutable board profile supplied by the product application.
struct BoardProfile {
    const BoardOutputBinding *outputs = nullptr;
    uint8_t output_count = 0;
};

/// One resolved output ready for a concrete pixel transport.
struct PhysicalOutput {
    uint8_t slot_index = 0;
    int gpio = -1;
    uint16_t pixel_count = 0;
    esp_pixels::PixelOrder order = esp_pixels::PixelOrder::GRB;
};

/// Injectable lifecycle boundary for rebuilding a pixel sink and Composer.
class PixelRuntimeBackend {
public:
    /// Releases a backend through its interface.
    virtual ~PixelRuntimeBackend() = default;

    /// Rebuilds transport and Composer with an optional ledmap, initially empty.
    /// @param outputs Resolved physical outputs in logical concatenation order.
    /// @param output_count Number of outputs, one or two.
    /// @param ledmap Validated canvas summary; configured=false selects identity.
    /// @param entries Ledmap entries when configured, otherwise nullptr.
    /// @return ESP_OK when the managed renderer is running with this topology.
    virtual esp_err_t rebuild(const PhysicalOutput *outputs, uint8_t output_count,
                              const pixels_payload::LedmapCandidate &ledmap,
                              const int32_t *entries) = 0;

    /// Publishes and confirms a scene on the running renderer.
    /// @param scene Validated scene candidate.
    /// @return ESP_OK only after frame-boundary adoption is confirmed.
    virtual esp_err_t apply(const esp_pixels_composer::AtScene &scene) = 0;

    /// Stops and releases all renderer and transport resources.
    /// @return ESP_OK after release, or the first drain/release error.
    virtual esp_err_t shutdown() = 0;
};

/// ESP-IDF/RMT implementation of the rebuildable pixel runtime.
class EspPixelRuntimeBackend final : public PixelRuntimeBackend {
public:
    /// Stops the runtime and releases owned transport buffers.
    ~EspPixelRuntimeBackend() override;

    /// Rebuilds one or two esp_pixels outputs and a managed Composer.
    /// @param outputs Resolved physical outputs.
    /// @param output_count Number of outputs.
    /// @param ledmap Canvas summary.
    /// @param entries Ledmap entries when configured.
    /// @return ESP_OK when the renderer is running.
    esp_err_t rebuild(const PhysicalOutput *outputs, uint8_t output_count,
                      const pixels_payload::LedmapCandidate &ledmap,
                      const int32_t *entries) override;

    /// Publishes and confirms a scene.
    /// @param scene Scene to adopt.
    /// @return ESP_OK only after confirmed adoption.
    esp_err_t apply(const esp_pixels_composer::AtScene &scene) override;

    /// Stops Composer, drains the sink and releases all outputs.
    /// @return ESP_OK after release, or the first release error.
    esp_err_t shutdown() override;

    /// Exposes current transport capabilities for telemetry.
    /// @return Stable sink owned by this backend.
    const esp_pixels_composer_adapter_esp_pixels::EspPixelsSink &sink() const { return sink_; }

private:
    esp_pixels::Pixels pixels_[pixels_payload::kMaxOutputs]{};
    esp_pixels_composer_adapter_esp_pixels::EspPixelsSink sink_{};
    esp_pixels_composer::Composer composer_{};
};

/// Transactional activation target that resolves logical slots through a
/// board profile and rebuilds transport/canvas for each apply_config. If a
/// rebuild or scene adoption fails, the last confirmed topology and scene are
/// restored before the error is returned.
class TransactionalPixelActivationTarget final : public pixels_storage::ActivationTarget {
public:
    /// Copies and validates the immutable board profile.
    /// @param backend Rebuildable runtime that outlives this target.
    /// @param profile Board-owned slot/GPIO bindings.
    TransactionalPixelActivationTarget(PixelRuntimeBackend &backend,
                                       const BoardProfile &profile);

    /// Releases cached ledmap storage; the backend remains caller-owned.
    ~TransactionalPixelActivationTarget() override;

    /// Builds the profile's safe topology and transmits an empty black scene.
    /// @return ESP_OK when the physical outputs are confirmed black.
    esp_err_t beginSafeBlank();

    /// Parses, rebuilds when needed and confirms adoption of persisted bytes.
    /// @param type ApplyConfig or ActivatePreset.
    /// @param payload Exact payload JSON bytes.
    /// @param size Payload byte count.
    /// @return ESP_OK only after the live runtime matches the payload.
    esp_err_t activateAtFrameBoundary(pixels_firmware::CommandType type,
                                       const char *payload, size_t size) override;

    /// Restores the board profile's safe black topology.
    /// @return ESP_OK only after physical outputs are confirmed black.
    esp_err_t clearAtFrameBoundary() override;

private:
    bool validProfile() const;
    esp_err_t resolveOutputs(const pixels_payload::Candidate &candidate,
                             PhysicalOutput outputs[pixels_payload::kMaxOutputs]) const;
    esp_err_t restoreConfirmed();
    esp_err_t buildSafeBlank();

    PixelRuntimeBackend &backend_;
    BoardOutputBinding bindings_[pixels_payload::kMaxOutputs]{};
    uint8_t binding_count_ = 0;
    bool profile_valid_ = false;
    pixels_payload::Candidate candidate_{};
    PhysicalOutput base_outputs_[pixels_payload::kMaxOutputs]{};
    uint8_t base_output_count_ = 0;
    pixels_payload::LedmapCandidate base_ledmap_{};
    int32_t *base_ledmap_entries_ = nullptr;
    esp_pixels_composer::AtScene active_scene_{};
    bool base_confirmed_ = false;
};

} // namespace pixels_control
