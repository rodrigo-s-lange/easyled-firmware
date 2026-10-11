#include "pixels_control_runtime.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <new>

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
    bool has_config_checksum = false;
    bool invalid_config_checksum = false;
    char config_checksum[pixels_firmware::kSha256HexBytes + 1]{};
};

esp_err_t onHttpEvent(esp_http_client_event_t *event)
{
    if (event == nullptr || event->user_data == nullptr) { return ESP_ERR_INVALID_ARG; }
    auto &context = *static_cast<HttpDownloadContext *>(event->user_data);
    if (event->event_id == HTTP_EVENT_ON_HEADER) {
        if (sameHeader(event->header_key, "Content-Type")) {
            context.application_json = jsonContentType(event->header_value);
        } else if (sameHeader(event->header_key, "X-Config-Checksum")) {
            const size_t size = event->header_value == nullptr ? 0 :
                strnlen(event->header_value, pixels_firmware::kSha256HexBytes + 1);
            if (context.has_config_checksum || size != pixels_firmware::kSha256HexBytes) {
                context.invalid_config_checksum = true;
            } else {
                std::memcpy(context.config_checksum, event->header_value, size);
                context.config_checksum[size] = '\0';
                context.has_config_checksum = true;
            }
        }
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
        result.has_config_checksum = context.has_config_checksum;
        if (context.has_config_checksum) {
            std::memcpy(result.config_checksum, context.config_checksum,
                        sizeof(result.config_checksum));
        }
    }
    const esp_err_t cleanup = esp_http_client_cleanup(client);
    std::memset(authorization_, 0, sizeof(authorization_));
    if (context.overflow) { return ESP_ERR_INVALID_SIZE; }
    if (context.invalid_config_checksum) { return ESP_ERR_INVALID_RESPONSE; }
    if (err != ESP_OK) { return err; }
    return cleanup;
}

bool validHttpsConfig(const HttpsPayloadSource::Config &config)
{
    const size_t token_size = config.bearer_token == nullptr ? 0 :
        strnlen(config.bearer_token, HttpsPayloadSource::kMaxBearerTokenBytes + 1);
    if (token_size == 0 || token_size > HttpsPayloadSource::kMaxBearerTokenBytes) { return false; }
    if (config.server_ca_pem != nullptr && config.server_ca_pem[0] == '\0') { return false; }
    if (config.timeout_ms == 0 || config.timeout_ms > kMaximumHttpTimeoutMs) { return false; }
    if (config.max_payload_bytes == 0 ||
        config.max_payload_bytes > pixels_payload::kMaxPayloadBytes) {
        return false;
    }
    return true;
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
    if (!command.has_checksum || !httpsUrl(command.resource_url) || capacity == 0 ||
        !validHttpsConfig(config_)) {
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
    if (result.has_config_checksum) {
        uint8_t header_checksum[pixels_firmware::kSha256Bytes]{};
        if (pixels_firmware::parseSha256(result.config_checksum, header_checksum) != ESP_OK) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        err = pixels_firmware::verifySha256(buffer, result.size, header_checksum);
        if (err != ESP_OK) { return err; }
    }
    std::memcpy(expected_checksum, command.checksum, pixels_firmware::kSha256Bytes);
    size = result.size;
    return ESP_OK;
}

RoutedPayloadSource::RoutedPayloadSource(HttpsPayloadSource *https_source,
                                         EmbeddedPayloadSource &embedded_source)
    : https_source_(https_source), embedded_source_(embedded_source)
{
}

esp_err_t RoutedPayloadSource::resolve(
    const pixels_firmware::Command &command, char *buffer, size_t capacity,
    size_t &size, uint8_t expected_checksum[pixels_firmware::kSha256Bytes])
{
    if (https_source_ != nullptr && command.type == pixels_firmware::CommandType::ApplyConfig) {
        return https_source_->resolve(command, buffer, capacity, size, expected_checksum);
    }
    return embedded_source_.resolve(command, buffer, capacity, size, expected_checksum);
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

esp_err_t MqttTelemetrySink::publishState(const pixels_firmware::StateReport &state, int &msg_id)
{
    msg_id = -1;
    size_t size = 0;
    esp_err_t err = pixels_firmware::serializeState(state, buffer_, sizeof(buffer_), size);
    if (err != ESP_OK) { return err; }
    msg_id = session_.publish(state_topic_, buffer_, size, true);
    return msg_id >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t MqttTelemetrySink::publishCapability(
    const pixels_firmware::CapabilityReport &report, int &msg_id)
{
    msg_id = -1;
    size_t size = 0;
    esp_err_t err = pixels_firmware::serializeCapabilityReport(
        report, buffer_, sizeof(buffer_), size);
    if (err != ESP_OK) { return err; }
    msg_id = session_.publish(event_topic_, buffer_, size, false);
    return msg_id >= 0 ? ESP_OK : ESP_FAIL;
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

EspPixelRuntimeBackend::~EspPixelRuntimeBackend() { (void)shutdown(); }

esp_err_t EspPixelRuntimeBackend::rebuild(
    const PhysicalOutput *outputs, uint8_t output_count,
    const pixels_payload::LedmapCandidate &ledmap, const int32_t *entries)
{
    if (outputs == nullptr || output_count == 0 || output_count > pixels_payload::kMaxOutputs ||
        (ledmap.configured && (entries == nullptr || ledmap.entry_count == 0))) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = shutdown();
    if (err != ESP_OK) { return err; }
    err = sink_.init(pixels_[0], outputs[0].gpio, outputs[0].pixel_count, outputs[0].order);
    for (uint8_t i = 1; err == ESP_OK && i < output_count; ++i) {
        err = sink_.addOutput(pixels_[i], outputs[i].gpio, outputs[i].pixel_count,
                              outputs[i].order);
    }
    if (err == ESP_OK) { err = composer_.build(sink_); }
    if (err == ESP_OK && ledmap.configured) {
        err = composer_.setLedmap(ledmap.width, ledmap.height, entries, ledmap.entry_count);
    }
    if (err == ESP_OK) { err = composer_.start(); }
    if (err != ESP_OK) { (void)shutdown(); }
    return err;
}

esp_err_t EspPixelRuntimeBackend::apply(const esp_pixels_composer::AtScene &scene)
{
    if (!composer_.running()) { return ESP_ERR_INVALID_STATE; }
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

esp_err_t EspPixelRuntimeBackend::shutdown()
{
    const esp_err_t composer_err = composer_.end();
    if (composer_err != ESP_OK) { return composer_err; }
    return sink_.end();
}

TransactionalPixelActivationTarget::TransactionalPixelActivationTarget(
    PixelRuntimeBackend &backend, const BoardProfile &profile)
    : backend_(backend)
{
    if (profile.outputs != nullptr && profile.output_count > 0 &&
        profile.output_count <= pixels_payload::kMaxOutputs) {
        binding_count_ = profile.output_count;
        for (uint8_t i = 0; i < binding_count_; ++i) { bindings_[i] = profile.outputs[i]; }
    }
    profile_valid_ = validProfile();
}

TransactionalPixelActivationTarget::~TransactionalPixelActivationTarget()
{
    delete[] base_ledmap_entries_;
}

bool TransactionalPixelActivationTarget::validProfile() const
{
    if (binding_count_ == 0 || binding_count_ > pixels_payload::kMaxOutputs) { return false; }
    bool has_safe_output = false;
    for (uint8_t i = 0; i < binding_count_; ++i) {
        const auto &binding = bindings_[i];
        if (binding.slot_index >= pixels_payload::kMaxOutputs || binding.gpio < 0 ||
            binding.max_pixels == 0 || binding.max_pixels > pixels_payload::kMaxPixelsPerOutput ||
            binding.safe_pixel_count > binding.max_pixels) {
            return false;
        }
        has_safe_output = has_safe_output || binding.safe_pixel_count != 0;
        for (uint8_t j = 0; j < i; ++j) {
            if (bindings_[j].slot_index == binding.slot_index || bindings_[j].gpio == binding.gpio) {
                return false;
            }
        }
    }
    return has_safe_output;
}

esp_err_t TransactionalPixelActivationTarget::resolveOutputs(
    const pixels_payload::Candidate &candidate,
    PhysicalOutput outputs[pixels_payload::kMaxOutputs]) const
{
    if (!profile_valid_ || !candidate.has_outputs || candidate.output_count == 0 ||
        candidate.output_count > pixels_payload::kMaxOutputs || outputs == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    for (uint8_t i = 0; i < candidate.output_count; ++i) {
        const auto &requested = candidate.outputs[i];
        const BoardOutputBinding *binding = nullptr;
        for (uint8_t j = 0; j < binding_count_; ++j) {
            if (bindings_[j].slot_index == requested.slot_index) { binding = &bindings_[j]; break; }
        }
        if (binding == nullptr || requested.pixel_count > binding->max_pixels) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        outputs[i] = {
            .slot_index = requested.slot_index,
            .gpio = binding->gpio,
            .pixel_count = requested.pixel_count,
            .order = requested.order,
        };
    }
    return ESP_OK;
}

esp_err_t TransactionalPixelActivationTarget::buildSafeBlank()
{
    if (!profile_valid_) { return ESP_ERR_INVALID_ARG; }
    PhysicalOutput safe[pixels_payload::kMaxOutputs]{};
    uint8_t count = 0;
    uint32_t total = 0;
    for (uint8_t i = 0; i < binding_count_; ++i) {
        if (bindings_[i].safe_pixel_count == 0) { continue; }
        safe[count++] = {
            .slot_index = bindings_[i].slot_index,
            .gpio = bindings_[i].gpio,
            .pixel_count = bindings_[i].safe_pixel_count,
            .order = bindings_[i].safe_order,
        };
        total += bindings_[i].safe_pixel_count;
    }
    pixels_payload::LedmapCandidate identity{};
    identity.width = static_cast<uint16_t>(total);
    identity.height = 1;
    identity.entry_count = total;
    esp_err_t err = backend_.rebuild(safe, count, identity, nullptr);
    if (err != ESP_OK) { return err; }
    esp_pixels_composer::AtScene black{};
    black.brightness = 0;
    return backend_.apply(black);
}

esp_err_t TransactionalPixelActivationTarget::beginSafeBlank()
{
    delete[] base_ledmap_entries_;
    base_ledmap_entries_ = nullptr;
    base_confirmed_ = false;
    base_output_count_ = 0;
    base_ledmap_ = pixels_payload::LedmapCandidate{};
    active_scene_ = esp_pixels_composer::AtScene{};
    return buildSafeBlank();
}

esp_err_t TransactionalPixelActivationTarget::restoreConfirmed()
{
    if (!base_confirmed_) { return buildSafeBlank(); }
    esp_err_t err = backend_.rebuild(base_outputs_, base_output_count_, base_ledmap_,
                                     base_ledmap_entries_);
    if (err == ESP_OK) { err = backend_.apply(active_scene_); }
    return err;
}

esp_err_t TransactionalPixelActivationTarget::activateAtFrameBoundary(
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
    if (type == pixels_firmware::CommandType::ActivatePreset) {
        if (!base_confirmed_) { return ESP_ERR_INVALID_STATE; }
        err = backend_.apply(candidate_.scene);
        if (err == ESP_OK) {
            active_scene_ = candidate_.scene;
            return ESP_OK;
        }
        const esp_err_t restored = restoreConfirmed();
        return restored == ESP_OK ? err : restored;
    }

    PhysicalOutput resolved[pixels_payload::kMaxOutputs]{};
    err = resolveOutputs(candidate_, resolved);
    if (err != ESP_OK) { return err; }
    int32_t *entries = nullptr;
    if (candidate_.ledmap.configured) {
        entries = new (std::nothrow) int32_t[candidate_.ledmap.entry_count];
        if (entries == nullptr) { return ESP_ERR_NO_MEM; }
        err = pixels_payload::materializeLedmap(payload, size, candidate_.ledmap, entries,
                                                candidate_.ledmap.entry_count);
        if (err != ESP_OK) { delete[] entries; return err; }
    }
    err = backend_.rebuild(resolved, candidate_.output_count, candidate_.ledmap, entries);
    if (err == ESP_OK) { err = backend_.apply(candidate_.scene); }
    if (err != ESP_OK) {
        delete[] entries;
        const esp_err_t restored = restoreConfirmed();
        return restored == ESP_OK ? err : restored;
    }

    delete[] base_ledmap_entries_;
    base_ledmap_entries_ = entries;
    base_output_count_ = candidate_.output_count;
    for (uint8_t i = 0; i < base_output_count_; ++i) { base_outputs_[i] = resolved[i]; }
    base_ledmap_ = candidate_.ledmap;
    active_scene_ = candidate_.scene;
    base_confirmed_ = true;
    return ESP_OK;
}

esp_err_t TransactionalPixelActivationTarget::clearAtFrameBoundary()
{
    return beginSafeBlank();
}

} // namespace pixels_control
