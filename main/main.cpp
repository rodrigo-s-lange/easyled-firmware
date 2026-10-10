#include "network_session.hpp"
#include "firmware_contract.hpp"
#include "pixels_command_executor.hpp"
#include "pixels_control_runtime.hpp"
#include "pixels_durable_storage.hpp"
#include "esp_pixels.hpp"
#include "esp_pixels_fx.hpp"
#include "esp_pixels_composer.hpp"
#include "esp_pixels_composer_adapter_esp_pixels.hpp"

#include <cctype>
#include <climits>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "nvs_flash.h"

namespace {

constexpr char kTag[] = "PIXELS_NET";
constexpr char kFixtureConfigUrl[] =
    "https://example.invalid/api/v1/devices/11111111-1111-4111-8111-111111111111/config/13";
constexpr char kFixturePresetId[] = "55555555-5555-4555-8555-555555555555";
constexpr char kFixtureConfigSha256[] =
    "b62868aef25369d8b00db8dbc6af34c7c82b060b5570b0b7b24859a7d5547c5a";
constexpr char kFixturePresetSha256[] =
    "c9be5621fa0109c6acbf4461a47191b80b3b6ebae2d50714d5c56172f7cc5174";

struct ApplicationConfig {
    pixels_network::NetworkSession::Config network{};
    const char *device_id = nullptr;
    const char *hardware_binding_id = nullptr;
    uint32_t generation = 0;
    const char *firmware_version = nullptr;
    const char *event_topic = nullptr;
    const char *state_topic = nullptr;
    /// A non-null token is this profile's claim to provisioned/production
    /// mode: apply_config then resolves over HTTPS exclusively, with no
    /// embedded fallback on any failure. Null selects laboratory mode
    /// instead, where apply_config resolves from the embedded fixture --
    /// never a silent downgrade, only ever this device's own explicit choice.
    const char *https_bearer_token = nullptr;
    /// Null selects the ESP-IDF certificate bundle (the default for a
    /// platform-issued server certificate); set only for a pinned/private CA.
    /// Ignored in laboratory mode.
    const char *https_server_ca_pem = nullptr;
    /// Zero selects HttpsPayloadSource::Config's own default (10 s). Ignored
    /// in laboratory mode.
    uint32_t https_timeout_ms = 0;
    /// Zero selects HttpsPayloadSource::Config's own default
    /// (pixels_payload::kMaxPayloadBytes). Ignored in laboratory mode.
    size_t https_max_payload_bytes = 0;
};

// An optional, gitignored application profile supplies applicationConfig().
// Credentials do not belong in Kconfig, source control or diagnostic output.
#if __has_include("network_credentials.hpp")
#include "network_credentials.hpp"
#else
ApplicationConfig applicationConfig() { return {}; }
#endif

extern "C" {
extern const uint8_t config_payload_bench_gpio16_50_provisional_json_start[] asm(
    "_binary_config_payload_bench_gpio16_50_provisional_json_start");
extern const uint8_t config_payload_bench_gpio16_50_provisional_json_end[] asm(
    "_binary_config_payload_bench_gpio16_50_provisional_json_end");
extern const uint8_t preset_payload_provisional_json_start[] asm(
    "_binary_preset_payload_provisional_json_start");
extern const uint8_t preset_payload_provisional_json_end[] asm(
    "_binary_preset_payload_provisional_json_end");
}

const char *fixtureBytes(const uint8_t *start)
{
    return reinterpret_cast<const char *>(start);
}

size_t fixtureSize(const uint8_t *start, const uint8_t *end)
{
    return static_cast<size_t>(end - start) - 1;
}

bool validUuid(const char *value)
{
    if (value == nullptr || std::strlen(value) != pixels_firmware::kUuidBytes) { return false; }
    for (size_t i = 0; i < pixels_firmware::kUuidBytes; ++i) {
        const bool separator = i == 8 || i == 13 || i == 18 || i == 23;
        if ((separator && value[i] != '-') ||
            (!separator && !std::isxdigit(static_cast<unsigned char>(value[i])))) {
            return false;
        }
    }
    return true;
}

bool validTopic(const char *value)
{
    return value != nullptr && value[0] != '\0' && std::strlen(value) <= 192 &&
           std::strpbrk(value, "+#") == nullptr;
}

bool validVersion(const char *value)
{
    return value != nullptr && value[0] != '\0' && std::strlen(value) <= 32;
}

uint32_t boundedU32(size_t value)
{
    return value > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(value);
}

enum class HttpsProfileState {
    Absent,     // no bearer token -- laboratory mode, not an error
    Incomplete, // a token is present (claims provisioned mode) but the rest is malformed
    Complete,   // provisioned mode, ready to construct HttpsPayloadSource
};

HttpsProfileState httpsProfileState(const ApplicationConfig &config,
                                    pixels_control::HttpsPayloadSource::Config &out)
{
    if (config.https_bearer_token == nullptr) { return HttpsProfileState::Absent; }
    out.bearer_token = config.https_bearer_token;
    out.server_ca_pem = config.https_server_ca_pem;
    if (config.https_timeout_ms != 0) { out.timeout_ms = config.https_timeout_ms; }
    if (config.https_max_payload_bytes != 0) { out.max_payload_bytes = config.https_max_payload_bytes; }
    return pixels_control::validHttpsConfig(out) ? HttpsProfileState::Complete
                                                  : HttpsProfileState::Incomplete;
}

pixels_firmware::CapabilityManifest capabilityManifest(
    const esp_pixels_composer_adapter_esp_pixels::EspPixelsSink &sink)
{
    static constexpr uint8_t kEffectIds[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21};
    const auto transport = sink.capabilities();
    uint32_t total_pixels = 0;
    for (uint8_t slot = 0; slot < sink.outputCount(); ++slot) {
        total_pixels += sink.pixelCount(slot);
    }
    pixels_firmware::CapabilityManifest manifest{};
    manifest.free_internal_heap_bytes = boundedU32(
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    manifest.largest_internal_heap_block_bytes = boundedU32(
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    manifest.psram_total_bytes = boundedU32(heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    manifest.psram_free_bytes = boundedU32(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    manifest.max_output_slots = transport.max_output_slots;
    manifest.max_pixels_per_output = transport.max_pixels_per_output;
    manifest.synchronized_start = transport.synchronized_start;
    manifest.configured_output_count = sink.outputCount();
    manifest.configured_total_pixels = static_cast<uint16_t>(total_pixels);
    manifest.max_segments = esp_pixels_composer::MAX_SEGMENTS;
    manifest.effect_ids = kEffectIds;
    manifest.effect_count = sizeof(kEffectIds);
    manifest.supports_ledmap = true;
    manifest.supports_2d = true;
    manifest.supports_mirror = true;
    manifest.custom_param_slots = 3;
    manifest.preset_cache_slots = 100;
    return manifest;
}

bool terminalApplied(const pixels_storage::NvsTerminalJournal &journal, const char *command_id,
                     pixels_firmware::TerminalRecord &record)
{
    return journal.find(command_id, record) == ESP_OK &&
           std::strstr(record.event_json, "\"event_type\":\"command_applied\"") != nullptr;
}

} // namespace

extern "C" void app_main()
{
    static_assert(esp_pixels_composer_adapter_esp_pixels::EspPixelsSink::kMaxOutputs == 2);
    ESP_LOGI(kTag, "combined build Pixels=%s FX=%s Composer=%s",
             ESP_PIXELS_VERSION, ESP_PIXELS_FX_VERSION, ESP_PIXELS_COMPOSER_VERSION);
    if (esp_pixels_fx::describe(0) == nullptr) {
        ESP_LOGE(kTag, "effect catalog unavailable");
        return;
    }

    const esp_err_t nvs = nvs_flash_init();
    if (nvs != ESP_OK) {
        ESP_LOGE(kTag, "NVS unavailable: %s; no erase attempted", esp_err_to_name(nvs));
        return;
    }

    // Lab-board wiring is firmware-owned. Remote configuration selects only
    // logical slots and can never redirect a signal to another GPIO. The safe
    // boot count covers the currently connected 31x12 matrix on GPIO16 so an
    // empty store can actively clear every physical pixel.
    static constexpr pixels_control::BoardOutputBinding board_outputs[] = {
        {.slot_index = 0, .gpio = 16, .max_pixels = pixels_payload::kMaxPixelsPerOutput,
         .safe_pixel_count = 372, .safe_order = esp_pixels::PixelOrder::GRB},
        {.slot_index = 1, .gpio = 17, .max_pixels = pixels_payload::kMaxPixelsPerOutput,
         .safe_pixel_count = 0, .safe_order = esp_pixels::PixelOrder::GRB},
    };
    static constexpr pixels_control::BoardProfile board_profile{
        .outputs = board_outputs,
        .output_count = sizeof(board_outputs) / sizeof(board_outputs[0]),
    };
    static pixels_control::EspPixelRuntimeBackend pixel_runtime;
    static pixels_control::TransactionalPixelActivationTarget activation_target(
        pixel_runtime, board_profile);
    esp_err_t err = activation_target.beginSafeBlank();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "pixel runtime startup failed: %s", esp_err_to_name(err));
        (void)pixel_runtime.shutdown();
        return;
    }
    // Boot performs several independent driver/NVS initialization phases.
    // Give both idle tasks a scheduling point between them; this also keeps
    // the default task watchdog meaningful on slow emulators and slow flash.
    vTaskDelay(1);

    static pixels_storage::NvsTerminalJournal journal;
    static pixels_storage::NvsDurableStaging staging;
    static pixels_storage::NvsTelemetryState telemetry_state;
    err = journal.init();
    if (err == ESP_OK) {
        vTaskDelay(1);
        err = staging.init(activation_target, journal);
        if (err == ESP_ERR_NOT_FOUND) {
            ESP_LOGI(kTag, "durable pixel state is empty");
            err = ESP_OK;
        } else if (err == ESP_OK) {
            ESP_LOGI(kTag, "durable pixel state restored");
        }
    }
    if (err == ESP_OK) { err = telemetry_state.init(); }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "durable control startup failed: %s", esp_err_to_name(err));
        telemetry_state.end();
        staging.end();
        journal.end();
        (void)pixel_runtime.shutdown();
        return;
    }

    static uint8_t config_digest[pixels_firmware::kSha256Bytes]{};
    static uint8_t preset_digest[pixels_firmware::kSha256Bytes]{};
    if (pixels_firmware::parseSha256(kFixtureConfigSha256, config_digest) != ESP_OK ||
        pixels_firmware::parseSha256(kFixturePresetSha256, preset_digest) != ESP_OK) {
        ESP_LOGE(kTag, "embedded resource digest is invalid");
        return;
    }
    // Always available: the embedded apply_config fixture backs laboratory
    // mode (no HTTPS credential configured), and the preset fixture backs
    // activate_preset in every mode until the platform publishes its second
    // download contract package.
    static const pixels_control::EmbeddedResource resources[] = {
        {pixels_firmware::CommandType::ApplyConfig, kFixtureConfigUrl,
         fixtureBytes(config_payload_bench_gpio16_50_provisional_json_start),
         fixtureSize(config_payload_bench_gpio16_50_provisional_json_start,
                     config_payload_bench_gpio16_50_provisional_json_end),
         config_digest},
        {pixels_firmware::CommandType::ActivatePreset, kFixturePresetId,
         fixtureBytes(preset_payload_provisional_json_start),
         fixtureSize(preset_payload_provisional_json_start,
                     preset_payload_provisional_json_end),
         preset_digest},
    };
    static pixels_control::EmbeddedPayloadSource embedded_source(
        resources, sizeof(resources) / sizeof(resources[0]));
    static pixels_control::DurableActiveProbe active_probe(staging);

    static pixels_network::NetworkSession session;
    static const ApplicationConfig config = applicationConfig();
    if (config.network.ssid == nullptr) {
        ESP_LOGI(kTag, "no local profile; restored pixels remain active and network stays disabled");
        return;
    }
    if (!validUuid(config.device_id) || !validUuid(config.hardware_binding_id) ||
        config.generation == 0 || !validVersion(config.firmware_version) ||
        !validTopic(config.event_topic) || !validTopic(config.state_topic)) {
        ESP_LOGE(kTag, "local control identity/topic is invalid");
        return;
    }

    static pixels_control::HttpsPayloadSource::Config https_config{};
    const HttpsProfileState https_state = httpsProfileState(config, https_config);
    if (https_state == HttpsProfileState::Incomplete) {
        // Rendering/local recovery stay independent of network availability
        // (AGENTS.md): restored pixels remain active, only networking (and
        // therefore any config/preset delivery) is refused.
        ESP_LOGE(kTag, "HTTPS profile claims provisioned mode but is incomplete; refusing to start"
                       " networking (never silently downgrading a claimed-production device to"
                       " laboratory mode)");
        return;
    }
    static pixels_control::EspHttpPayloadClient http_client;
    static pixels_control::HttpsPayloadSource https_source(http_client, https_config);
    const bool provisioned = https_state == HttpsProfileState::Complete;
    ESP_LOGI(kTag, "apply_config mode: %s", provisioned ? "provisioned (HTTPS)" : "laboratory (embedded)");
    static pixels_control::RoutedPayloadSource payload_source(
        provisioned ? &https_source : nullptr, embedded_source);

    static pixels_control::MqttEventSink event_sink(session, config.event_topic);
    static pixels_control::MqttTelemetrySink telemetry_sink(
        session, config.state_topic, config.event_topic);
    static pixels_control::UtcTimestampSource timestamp_source;
    static pixels_executor::Executor executor(journal, staging, payload_source, event_sink,
                                               timestamp_source, active_probe, config.device_id);

    err = session.init(config.network);
    if (err == ESP_OK) { err = session.start(); }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "network startup failed: %s", esp_err_to_name(err));
        session.end();
        return;
    }

    bool sntp_started = false;
    bool previously_ready = false;
    uint32_t state_revision = 0;
    uint32_t published_state_revision = 0;
    uint32_t pending_state_revision = 0;
    uint32_t pending_state_sequence = 0;
    char pending_state_timestamp[36]{};
    int64_t next_state_attempt_us = 0;
    int64_t next_capability_attempt_us = 0;
    bool capability_due = false;
    constexpr int64_t kRetryIntervalUs = 5LL * 1000 * 1000;
    constexpr int64_t kCapabilityIntervalUs = 60LL * 60 * 1000 * 1000;
    static pixels_network::NetworkSession::Message message;
    static pixels_firmware::TerminalRecord existing_terminal;
    for (;;) {
        const auto state = session.status();
        if (state.wifi_connected && !sntp_started) {
            esp_sntp_config_t time_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            sntp_started = esp_netif_sntp_init(&time_config) == ESP_OK;
        }
        session.poll();
        const bool ready = session.status().subscribed;
        if (ready != previously_ready) {
            ESP_LOGI(kTag, "command subscription %s", ready ? "ready" : "offline");
            if (ready) {
                ++state_revision;
                capability_due = true;
                next_state_attempt_us = 0;
                next_capability_attempt_us = 0;
            }
            previously_ready = ready;
        }
        while (session.receive(message)) {
            char command_id[pixels_firmware::kUuidBytes + 1]{};
            const bool identified = pixels_firmware::extractCommandId(
                message.data, message.size, command_id) == ESP_OK;
            const bool duplicate = identified &&
                journal.find(command_id, existing_terminal) == ESP_OK;
            const esp_err_t executed = executor.execute(message.data, message.size);
            if (identified) {
                ESP_LOGI(kTag, "command %s terminal handling: %s", command_id,
                         esp_err_to_name(executed));
                if (!duplicate && terminalApplied(journal, command_id, existing_terminal)) {
                    const esp_err_t noted = telemetry_state.noteApplied(command_id);
                    if (noted != ESP_OK) {
                        ESP_LOGE(kTag, "last-applied state persistence failed: %s",
                                 esp_err_to_name(noted));
                    }
                    ++state_revision;
                    next_state_attempt_us = 0;
                }
            } else {
                ESP_LOGW(kTag, "unaddressable command rejected locally: %s",
                         esp_err_to_name(executed));
            }
        }

        const int64_t now_us = esp_timer_get_time();
        if (ready && state_revision != published_state_revision && now_us >= next_state_attempt_us) {
            if (pending_state_sequence == 0 || pending_state_revision != state_revision) {
                pending_state_sequence = 0;
                const esp_err_t sequence_err = telemetry_state.nextStateSequence(
                    pending_state_sequence);
                if (sequence_err == ESP_OK) {
                    timestamp_source.now(pending_state_timestamp);
                    pending_state_revision = state_revision;
                } else {
                    ESP_LOGE(kTag, "state sequence persistence failed: %s",
                             esp_err_to_name(sequence_err));
                }
            }
            if (pending_state_sequence != 0) {
                pixels_storage::ActiveSnapshot active{};
                err = staging.activeSnapshot(active);
                if (err == ESP_OK) {
                    const char *last_applied = telemetry_state.lastAppliedCommandId();
                    if (last_applied == nullptr && active.active) {
                        last_applied = active.command_id;
                    }
                    pixels_firmware::StateReport report{};
                    report.device_id = config.device_id;
                    report.hardware_binding_id = config.hardware_binding_id;
                    report.generation = config.generation;
                    report.boot_counter = telemetry_state.bootCounter();
                    report.state_sequence = pending_state_sequence;
                    report.reported_at = pending_state_timestamp;
                    report.firmware_version = config.firmware_version;
                    report.config_version = active.active ? active.config_version : 0;
                    report.last_applied_command_id = last_applied;
                    if (active.active &&
                        active.type == pixels_firmware::CommandType::ActivatePreset) {
                        report.mode = pixels_firmware::DeviceMode::PresetActive;
                        report.active_preset_id = active.preset_id;
                    } else if (active.active) {
                        report.mode = pixels_firmware::DeviceMode::ConfigActive;
                    }
                    err = telemetry_sink.publishState(report);
                }
                if (err == ESP_OK) {
                    published_state_revision = pending_state_revision;
                    pending_state_sequence = 0;
                } else {
                    ESP_LOGW(kTag, "state publish deferred: %s", esp_err_to_name(err));
                    next_state_attempt_us = now_us + kRetryIntervalUs;
                }
            } else {
                next_state_attempt_us = now_us + kRetryIntervalUs;
            }
        }

        if (ready && (capability_due || now_us >= next_capability_attempt_us)) {
            char reported_at[36]{};
            timestamp_source.now(reported_at);
            pixels_firmware::CapabilityReport report{};
            report.device_id = config.device_id;
            report.firmware_version = config.firmware_version;
            report.reported_at = reported_at;
            report.capabilities = capabilityManifest(pixel_runtime.sink());
            err = telemetry_sink.publishCapability(report);
            if (err == ESP_OK) {
                capability_due = false;
                next_capability_attempt_us = now_us + kCapabilityIntervalUs;
            } else {
                ESP_LOGW(kTag, "capability publish deferred: %s", esp_err_to_name(err));
                capability_due = true;
                next_capability_attempt_us = now_us + kRetryIntervalUs;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}
