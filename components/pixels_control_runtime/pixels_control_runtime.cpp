#include "pixels_control_runtime.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace pixels_control {
namespace {

constexpr uint32_t kAdoptTimeoutMs = 150;
constexpr uint32_t kGraceTimeoutMs = 100;
constexpr uint32_t kMaximumHttpTimeoutMs = 60000;

bool httpsUrl(const char *url)
{
    return url != nullptr && std::strncmp(url, "https://", 8) == 0 && url[8] != '\0';
}

char asciiLower(char value)
{
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

bool sameHeader(const char *left, const char *right)
{
    if (left == nullptr || right == nullptr) { return false; }
    while (*left != '\0' && *right != '\0') {
        if (asciiLower(*left++) != asciiLower(*right++)) { return false; }
    }
    return *left == '\0' && *right == '\0';
}

bool jsonContentType(const char *value)
{
    constexpr char kJson[] = "application/json";
    if (value == nullptr) { return false; }
    for (size_t i = 0; i < sizeof(kJson) - 1; ++i) {
        if (value[i] == '\0' || asciiLower(value[i]) != kJson[i]) { return false; }
    }
    const char suffix = value[sizeof(kJson) - 1];
    return suffix == '\0' || suffix == ';' || suffix == ' ' || suffix == '\t';
}

struct HttpDownloadContext {
    char *destination = nullptr;
    size_t capacity = 0;
    size_t size = 0;
    bool overflow = false;
    bool application_json = false;
};

esp_err_t onHttpEvent(esp_http_client_event_t *event)
{
    if (event == nullptr || event->user_data == nullptr) { return ESP_ERR_INVALID_ARG; }
    auto &context = *static_cast<HttpDownloadContext *>(event->user_data);
    if (event->event_id == HTTP_EVENT_ON_HEADER &&
        sameHeader(event->header_key, "Content-Type")) {
        context.application_json = jsonContentType(event->header_value);
    }
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len == 0) { return ESP_OK; }
    if (event->data == nullptr || event->data_len < 0) { return ESP_ERR_INVALID_RESPONSE; }
    const size_t incoming = static_cast<size_t>(event->data_len);
    if (incoming > context.capacity - context.size) {
        context.overflow = true;
        return ESP_ERR_INVALID_SIZE;
    }
    std::memcpy(context.destination + context.size, event->data, incoming);
    context.size += incoming;
    return ESP_OK;
}

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

esp_err_t EspHttpPayloadClient::download(const HttpPayloadRequest &request,
                                         HttpPayloadResult &result)
{
    result = HttpPayloadResult{};
    const size_t token_size = request.bearer_token == nullptr ? 0 :
        strnlen(request.bearer_token, HttpsPayloadSource::kMaxBearerTokenBytes + 1);
    if (!httpsUrl(request.url) || token_size == 0 ||
        token_size > HttpsPayloadSource::kMaxBearerTokenBytes ||
        request.timeout_ms == 0 || request.timeout_ms > kMaximumHttpTimeoutMs ||
        request.destination == nullptr || request.capacity == 0 ||
        request.capacity > pixels_payload::kMaxPayloadBytes ||
        (request.server_ca_pem != nullptr && request.server_ca_pem[0] == '\0')) {
        return ESP_ERR_INVALID_ARG;
    }

    const int written = std::snprintf(authorization_, sizeof(authorization_),
                                      "Bearer %s", request.bearer_token);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(authorization_)) {
        std::memset(authorization_, 0, sizeof(authorization_));
        return ESP_ERR_INVALID_SIZE;
    }

    HttpDownloadContext context{request.destination, request.capacity};
    esp_http_client_config_t config{};
    config.url = request.url;
    config.cert_pem = request.server_ca_pem;
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = static_cast<int>(request.timeout_ms);
    config.disable_auto_redirect = true;
    config.max_authorization_retries = -1;
    config.event_handler = onHttpEvent;
    config.transport_type = HTTP_TRANSPORT_OVER_SSL;
    config.user_data = &context;
    config.skip_cert_common_name_check = false;
    config.crt_bundle_attach = request.server_ca_pem == nullptr ? esp_crt_bundle_attach : nullptr;
    config.keep_alive_enable = false;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        std::memset(authorization_, 0, sizeof(authorization_));
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_http_client_set_header(client, "Authorization", authorization_);
    if (err == ESP_OK) { err = esp_http_client_set_header(client, "Accept", "application/json"); }
    if (err == ESP_OK) { err = esp_http_client_perform(client); }
    if (err == ESP_OK) {
        result.status_code = esp_http_client_get_status_code(client);
        result.size = context.size;
        result.application_json = context.application_json;
    }
    const esp_err_t cleanup = esp_http_client_cleanup(client);
    std::memset(authorization_, 0, sizeof(authorization_));
    if (context.overflow) { return ESP_ERR_INVALID_SIZE; }
    if (err != ESP_OK) { return err; }
    return cleanup;
}

HttpsPayloadSource::HttpsPayloadSource(HttpPayloadClient &client, const Config &config)
    : client_(client), config_(config)
{
}

esp_err_t HttpsPayloadSource::resolve(
    const pixels_firmware::Command &command, char *buffer, size_t capacity,
    size_t &size, uint8_t expected_checksum[pixels_firmware::kSha256Bytes])
{
    size = 0;
    if (expected_checksum != nullptr) {
        std::memset(expected_checksum, 0, pixels_firmware::kSha256Bytes);
    }
    if (buffer == nullptr || expected_checksum == nullptr) { return ESP_ERR_INVALID_ARG; }
    if (command.type != pixels_firmware::CommandType::ApplyConfig) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const size_t token_size = config_.bearer_token == nullptr ? 0 :
        strnlen(config_.bearer_token, kMaxBearerTokenBytes + 1);
    if (!command.has_checksum || !httpsUrl(command.resource_url) || token_size == 0 ||
        token_size > kMaxBearerTokenBytes || config_.timeout_ms == 0 ||
        config_.timeout_ms > kMaximumHttpTimeoutMs || config_.max_payload_bytes == 0 ||
        config_.max_payload_bytes > pixels_payload::kMaxPayloadBytes || capacity == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    HttpPayloadRequest request{};
    request.url = command.resource_url;
    request.bearer_token = config_.bearer_token;
    request.server_ca_pem = config_.server_ca_pem;
    request.timeout_ms = config_.timeout_ms;
    request.destination = buffer;
    request.capacity = std::min(capacity, config_.max_payload_bytes);
    HttpPayloadResult result{};
    esp_err_t err = client_.download(request, result);
    if (err != ESP_OK) { return err; }
    if (result.status_code != HttpStatus_Ok || !result.application_json) { return ESP_FAIL; }
    if (result.size == 0 || result.size > request.capacity) { return ESP_ERR_INVALID_SIZE; }
    err = pixels_firmware::verifySha256(buffer, result.size, command.checksum);
    if (err != ESP_OK) { return err; }
    std::memcpy(expected_checksum, command.checksum, pixels_firmware::kSha256Bytes);
    size = result.size;
    return ESP_OK;
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
