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
#include <cstring>

#include "esp_netif_sntp.h"
#include "esp_log.h"
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
    const char *event_topic = nullptr;
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

void stopRenderer(esp_pixels_composer::Composer &composer,
                  esp_pixels_composer_adapter_esp_pixels::EspPixelsSink &sink)
{
    (void)composer.end();
    (void)sink.end();
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

    // Product-bench topology for this cut: the currently connected 50-pixel
    // GRB WS2812B strip on GPIO16. Payload topology must match this exactly.
    static esp_pixels::Pixels pixels;
    static esp_pixels_composer_adapter_esp_pixels::EspPixelsSink sink;
    static esp_pixels_composer::Composer composer;
    esp_err_t err = sink.init(pixels, 16, 50, esp_pixels::PixelOrder::GRB);
    if (err == ESP_OK) { err = composer.build(sink); }
    if (err == ESP_OK) { err = composer.start(); }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "pixel runtime startup failed: %s", esp_err_to_name(err));
        stopRenderer(composer, sink);
        return;
    }
    // Boot performs several independent driver/NVS initialization phases.
    // Give both idle tasks a scheduling point between them; this also keeps
    // the default task watchdog meaningful on slow emulators and slow flash.
    vTaskDelay(1);

    static constexpr pixels_payload::OutputCandidate fixed_outputs[] = {
        {.gpio = 16, .pixel_count = 50, .order = esp_pixels::PixelOrder::GRB},
    };
    static pixels_control::ComposerActivationTarget activation_target(
        composer, fixed_outputs, sizeof(fixed_outputs) / sizeof(fixed_outputs[0]));
    static pixels_storage::NvsTerminalJournal journal;
    static pixels_storage::NvsDurableStaging staging;
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
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "durable control startup failed: %s", esp_err_to_name(err));
        staging.end();
        journal.end();
        stopRenderer(composer, sink);
        return;
    }

    static uint8_t config_digest[pixels_firmware::kSha256Bytes]{};
    static uint8_t preset_digest[pixels_firmware::kSha256Bytes]{};
    if (pixels_firmware::parseSha256(kFixtureConfigSha256, config_digest) != ESP_OK ||
        pixels_firmware::parseSha256(kFixturePresetSha256, preset_digest) != ESP_OK) {
        ESP_LOGE(kTag, "embedded resource digest is invalid");
        return;
    }
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
    static pixels_control::EmbeddedPayloadSource payload_source(
        resources, sizeof(resources) / sizeof(resources[0]));
    static pixels_control::DurableActiveProbe active_probe(staging);

    static pixels_network::NetworkSession session;
    static const ApplicationConfig config = applicationConfig();
    if (config.network.ssid == nullptr) {
        ESP_LOGI(kTag, "no local profile; restored pixels remain active and network stays disabled");
        return;
    }
    if (!validUuid(config.device_id) || !validTopic(config.event_topic)) {
        ESP_LOGE(kTag, "local control identity/topic is invalid");
        return;
    }

    static pixels_control::MqttEventSink event_sink(session, config.event_topic);
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
    static pixels_network::NetworkSession::Message message;
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
            previously_ready = ready;
        }
        while (session.receive(message)) {
            char command_id[pixels_firmware::kUuidBytes + 1]{};
            const bool identified = pixels_firmware::extractCommandId(
                message.data, message.size, command_id) == ESP_OK;
            const esp_err_t executed = executor.execute(message.data, message.size);
            if (identified) {
                ESP_LOGI(kTag, "command %s terminal handling: %s", command_id,
                         esp_err_to_name(executed));
            } else {
                ESP_LOGW(kTag, "unaddressable command rejected locally: %s",
                         esp_err_to_name(executed));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}
