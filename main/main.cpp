#include "network_session.hpp"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/task.h"

// An optional, gitignored application profile supplies networkConfig().
// Credentials do not belong in Kconfig, source control or diagnostic output.
#if __has_include("network_credentials.hpp")
#include "network_credentials.hpp"
#else
static pixels_network::NetworkSession::Config networkConfig() { return {}; }
#endif

extern "C" void app_main()
{
    static pixels_network::NetworkSession session;
    const auto config = networkConfig();
    if (config.ssid == nullptr) {
        ESP_LOGI("PIXELS_NET", "no local profile; network remains disabled");
        return;
    }
    const esp_err_t nvs = nvs_flash_init();
    if (nvs != ESP_OK) {
        ESP_LOGE("PIXELS_NET", "NVS unavailable: %s; no erase attempted", esp_err_to_name(nvs));
        return;
    }
    esp_err_t err = session.init(config);
    if (err == ESP_OK) { err = session.start(); }
    if (err != ESP_OK) {
        ESP_LOGE("PIXELS_NET", "startup failed: %s", esp_err_to_name(err));
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
            ESP_LOGI("PIXELS_NET", "command subscription %s", ready ? "ready" : "offline");
            previously_ready = ready;
        }
        while (session.receive(message)) {
            // Product integration parses/authenticates/deduplicates here, never
            // inside the MQTT callback. This bring-up does not execute commands.
            ESP_LOGI("PIXELS_NET", "complete message received: %u bytes", unsigned(message.size));
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}
