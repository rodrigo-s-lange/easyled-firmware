#include "pixels_control_runtime.hpp"

#include <cstring>
#include <ctime>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace pixels_control {
namespace {

constexpr uint32_t kAdoptTimeoutMs = 150;
constexpr uint32_t kGraceTimeoutMs = 100;

bool sameOutput(const pixels_payload::OutputCandidate &left,
                const pixels_payload::OutputCandidate &right)
{
    return left.gpio == right.gpio && left.pixel_count == right.pixel_count &&
           left.order == right.order;
}

bool pollReclaimUntil(esp_pixels_composer::Composer &composer, uint32_t timeout_ms)
{
    const TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    const TickType_t started = xTaskGetTickCount();
    for (;;) {
        if (composer.reclaim() == ESP_OK) { return true; }
        const TickType_t elapsed = xTaskGetTickCount() - started;
        if (elapsed >= timeout) { return false; }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

} // namespace

EmbeddedPayloadSource::EmbeddedPayloadSource(const EmbeddedResource *resources, size_t count)
    : resources_(resources), count_(count)
{
}

esp_err_t EmbeddedPayloadSource::resolve(
    const pixels_firmware::Command &command, char *buffer, size_t capacity,
    size_t &size, uint8_t expected_checksum[pixels_firmware::kSha256Bytes])
{
    if (buffer == nullptr || expected_checksum == nullptr || resources_ == nullptr || count_ == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *key = command.type == pixels_firmware::CommandType::ApplyConfig
        ? command.resource_url
        : command.type == pixels_firmware::CommandType::ActivatePreset ? command.preset_id : nullptr;
    if (key == nullptr || key[0] == '\0') { return ESP_ERR_NOT_SUPPORTED; }

    for (size_t i = 0; i < count_; ++i) {
        const EmbeddedResource &resource = resources_[i];
        if (resource.type != command.type || resource.key == nullptr ||
            std::strcmp(resource.key, key) != 0) {
            continue;
        }
        if (resource.bytes == nullptr || resource.checksum == nullptr || resource.size == 0) {
            return ESP_ERR_INVALID_ARG;
        }
        if (resource.size > capacity) { return ESP_ERR_INVALID_SIZE; }
        std::memcpy(buffer, resource.bytes, resource.size);
        std::memcpy(expected_checksum, resource.checksum, pixels_firmware::kSha256Bytes);
        size = resource.size;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

MqttEventSink::MqttEventSink(pixels_network::NetworkSession &session, const char *topic)
    : session_(session), topic_(topic)
{
}

esp_err_t MqttEventSink::publish(const char *event_json, size_t size)
{
    return session_.publish(topic_, event_json, size, false) >= 0 ? ESP_OK : ESP_FAIL;
}

MqttTelemetrySink::MqttTelemetrySink(pixels_network::NetworkSession &session,
                                     const char *state_topic, const char *event_topic)
    : session_(session), state_topic_(state_topic), event_topic_(event_topic)
{
}

esp_err_t MqttTelemetrySink::publishState(const pixels_firmware::StateReport &state)
{
    size_t size = 0;
    esp_err_t err = pixels_firmware::serializeState(state, buffer_, sizeof(buffer_), size);
    if (err != ESP_OK) { return err; }
    return session_.publish(state_topic_, buffer_, size, true) >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t MqttTelemetrySink::publishCapability(
    const pixels_firmware::CapabilityReport &report)
{
    size_t size = 0;
    esp_err_t err = pixels_firmware::serializeCapabilityReport(
        report, buffer_, sizeof(buffer_), size);
    if (err != ESP_OK) { return err; }
    return session_.publish(event_topic_, buffer_, size, false) >= 0 ? ESP_OK : ESP_FAIL;
}

void UtcTimestampSource::now(char out[36])
{
    if (out == nullptr) { return; }
    std::memset(out, 0, 36);
    const std::time_t current = std::time(nullptr);
    struct tm utc{};
    if (gmtime_r(&current, &utc) == nullptr ||
        std::strftime(out, 36, "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) {
        std::memcpy(out, "1970-01-01T00:00:00Z", 21);
    }
}

DurableActiveProbe::DurableActiveProbe(const pixels_storage::NvsDurableStaging &staging)
    : staging_(staging)
{
}

bool DurableActiveProbe::alreadyActive(
    const char *preset_id, const uint8_t checksum[pixels_firmware::kSha256Bytes]) const
{
    return staging_.activePresetMatches(preset_id, checksum);
}

ComposerActivationTarget::ComposerActivationTarget(
    esp_pixels_composer::Composer &composer,
    const pixels_payload::OutputCandidate *outputs, uint8_t output_count)
    : composer_(composer)
{
    if (outputs == nullptr || output_count == 0 || output_count > pixels_payload::kMaxOutputs) {
        return;
    }
    output_count_ = output_count;
    for (uint8_t i = 0; i < output_count_; ++i) { outputs_[i] = outputs[i]; }
}

bool ComposerActivationTarget::topologyMatches(const pixels_payload::Candidate &candidate) const
{
    if (!candidate.has_outputs) { return true; }
    if (candidate.output_count != output_count_ || output_count_ == 0) { return false; }
    for (uint8_t i = 0; i < output_count_; ++i) {
        if (!sameOutput(candidate.outputs[i], outputs_[i])) { return false; }
    }
    return true;
}

esp_err_t ComposerActivationTarget::applyAndConfirm(const esp_pixels_composer::AtScene &scene)
{
    if (output_count_ == 0 || !composer_.running()) { return ESP_ERR_INVALID_STATE; }
    esp_err_t err = composer_.applyConfig(scene.segments, scene.count);
    if (err != ESP_OK) { return err; }
    if (pollReclaimUntil(composer_, kAdoptTimeoutMs)) {
        composer_.brightness(scene.brightness);
        return scene.paused ? composer_.pause() : composer_.resume();
    }
    if (composer_.cancelPending() == ESP_OK) { return ESP_ERR_TIMEOUT; }
    if (!pollReclaimUntil(composer_, kGraceTimeoutMs)) { return ESP_ERR_NOT_FINISHED; }
    composer_.brightness(scene.brightness);
    return scene.paused ? composer_.pause() : composer_.resume();
}

esp_err_t ComposerActivationTarget::activateAtFrameBoundary(
    pixels_firmware::CommandType type, const char *payload, size_t size)
{
    if (payload == nullptr || size == 0 || size > pixels_payload::kMaxPayloadBytes) {
        return ESP_ERR_INVALID_ARG;
    }
    candidate_ = pixels_payload::Candidate{};
    pixels_payload::SchemaError schema_error = pixels_payload::SchemaError::None;
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
    if (type == pixels_firmware::CommandType::ApplyConfig) {
        err = pixels_payload::parseConfigPayload(payload, size, candidate_, schema_error);
    } else if (type == pixels_firmware::CommandType::ActivatePreset) {
        err = pixels_payload::parsePresetPayload(payload, size, candidate_, schema_error);
    }
    if (err != ESP_OK) { return err; }
    if (!topologyMatches(candidate_)) { return ESP_ERR_NOT_SUPPORTED; }
    return applyAndConfirm(candidate_.scene);
}

esp_err_t ComposerActivationTarget::clearAtFrameBoundary()
{
    esp_pixels_composer::AtScene empty{};
    empty.brightness = 0;
    return applyAndConfirm(empty);
}

} // namespace pixels_control
