#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include "esp_err.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "mqtt_client.h"
#include "message_buffer.hpp"

namespace pixels_network {

/// Application-owned Wi-Fi/MQTT session; lifecycle/poll/publish/receive share
/// one application task. MQTT callbacks only update atomics and a bounded queue.
/// Call init/start explicitly, poll at least every 250 ms from a non-render task,
/// and end before releasing this object or any Config string. No Composer or NVS
/// ownership, command execution, provisioning or device-identity policy lives here.
class NetworkSession {
public:
    static constexpr size_t kMaxMessageBytes = 1024;
    static constexpr UBaseType_t kQueueLength = 2;

    struct Config {
        const char *ssid = nullptr;
        const char *wifi_password = nullptr;
        const char *broker_uri = nullptr; // wss:// only; no userinfo in URI.
        const char *client_id = nullptr;
        const char *username = nullptr;
        const char *password = nullptr;
        const char *command_topic = nullptr; // Exact topic, never a wildcard.
        const char *server_ca_pem = nullptr; // Null uses the ESP-IDF certificate bundle.
    };

    struct Message {
        size_t size = 0;
        char data[kMaxMessageBytes + 1]{};
    };

    struct Status {
        bool running;
        bool wifi_connected;
        bool subscribed;
        uint32_t connections;
        uint32_t rejected_messages;
        uint32_t queue_overflows;
        uint32_t transport_errors;
    };

    NetworkSession() = default;
    NetworkSession(const NetworkSession &) = delete;
    NetworkSession &operator=(const NetworkSession &) = delete;

    /// Prepares the MQTT client without starting Wi-Fi or the MQTT task.
    /// @param config Configuration whose strings must remain valid until end().
    /// @return ESP_OK, ESP_ERR_INVALID_ARG/STATE, or an SDK allocation/setup error.
    esp_err_t init(const Config &config);
    /// Starts Wi-Fi; MQTT starts from poll() once IP and a valid TLS clock exist.
    /// @return ESP_OK or the Wi-Fi initialization error; no scene is modified.
    esp_err_t start();
    /// Performs one connection-maintenance step, never waiting for network I/O.
    /// @return ESP_OK or an immediate SDK error; later poll calls retry.
    esp_err_t poll();
    /// Stops MQTT and disconnects Wi-Fi; configuration is retained for restart.
    /// @return ESP_OK or an SDK stop error; must not run from an MQTT callback.
    esp_err_t stop();
    /// Stops and frees this session, leaving shared Wi-Fi/NVS initialization intact.
    /// @return ESP_OK or an SDK error; on failure retry before releasing this object.
    esp_err_t end();
    /// Queues a QoS1 publication only while the command subscription is confirmed.
    /// @param topic Exact destination topic selected by the product application.
    /// @param payload Payload bytes, not necessarily NUL-terminated.
    /// @param size Payload byte count, at most kMaxMessageBytes.
    /// @param retained Whether the broker should retain this publication.
    /// @return Nonnegative MQTT message ID, or negative SDK/local rejection.
    int publish(const char *topic, const char *payload, size_t size, bool retained = false);
    /// Pops one complete message without blocking; application owns parsing/dedup.
    /// @param message Destination that is unchanged if no complete message exists.
    /// @return True when one message was copied, otherwise false.
    bool receive(Message &message);
    /// Samples diagnostic counters; not a synchronized product state snapshot.
    /// @return Current connection flags and counters, containing no credentials.
    Status status() const;

private:
    static void onEvent(void *arg, esp_event_base_t base, int32_t id, void *data);
    void event(int32_t id, esp_mqtt_event_t &event);

    Config config_{};
    esp_mqtt_client_handle_t client_ = nullptr;
    bool mqtt_started_ = false; // Application task only.
    int64_t next_wifi_retry_us_ = 0;
    int64_t next_mqtt_retry_us_ = 0;
    uint32_t wifi_retry_ms_ = 1000;
    int subscribe_id_ = -1; // MQTT event task only.
    std::atomic<bool> running_{false};
    std::atomic<bool> subscribed_{false};
    std::atomic<bool> reconnect_requested_{false};
    std::atomic<uint32_t> connections_{0}, rejected_{0}, overflow_{0}, errors_{0};
    QueueHandle_t messages_ = nullptr;
    StaticQueue_t queue_control_{};
    alignas(Message) uint8_t queue_storage_[kQueueLength * sizeof(Message)]{};
    MessageBuffer<kMaxMessageBytes> incoming_; // MQTT event task only.
    Message delivery_{}; // MQTT callback scratch, avoids a 1 KiB task-stack temporary.
};

} // namespace pixels_network
