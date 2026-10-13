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
        uint32_t publish_acks;
        /// Subset of `transport_errors` the SDK itself proved happened at the
        /// TCP/TLS layer, strictly before any MQTT authentication could
        /// occur (`MQTT_ERROR_TYPE_TCP_TRANSPORT`, aliased by
        /// `MQTT_ERROR_TYPE_ESP_TLS`). A credential-lifecycle caller may
        /// treat exactly this subset as a provable pre-authentication
        /// failure; every other transport error remains ambiguous.
        uint32_t pre_auth_transport_errors;
    };

    /// Broker acknowledgement of one QoS1 enqueue, as last observed by this
    /// session -- never a durable record, and never consulted by this class
    /// itself (the application owns what, if anything, is retried on
    /// Pending/Unknown).
    enum class PublishAck {
        Pending,      ///< Enqueued; no PUBACK observed yet, and no disconnect since.
        Acknowledged, ///< The broker's PUBACK for this exact msg_id was observed.
        Unknown,      ///< Not tracked: invalid id, evicted by newer publishes,
                      ///< or a disconnect happened after this id was enqueued --
                      ///< callers should treat this the same as "not yet confirmed".
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
    /// Enqueue success starts tracking for publishStatus(); it is not itself
    /// delivery proof -- see PublishAck.
    /// @param topic Exact destination topic selected by the product application.
    /// @param payload Payload bytes, not necessarily NUL-terminated.
    /// @param size Payload byte count, at most kMaxMessageBytes.
    /// @param retained Whether the broker should retain this publication.
    /// @return Nonnegative MQTT message ID, or negative SDK/local rejection.
    int publish(const char *topic, const char *payload, size_t size, bool retained = false);
    /// Checks this exact enqueued message's last known acknowledgement state.
    /// Bounded tracking (kMaxTrackedPublishes slots): a newer publish may
    /// evict an older unacknowledged one, surfacing as Unknown. A disconnect
    /// observed after this id was enqueued also surfaces as Unknown, even if
    /// still physically sitting in the SDK's own QoS1 outbox -- this session
    /// makes no claim about automatic SDK-level redelivery; the caller decides
    /// whether and how to retry.
    /// @param msg_id A value previously returned by publish().
    /// @return Pending, Acknowledged, or Unknown; see PublishAck.
    PublishAck publishStatus(int msg_id) const;
    /// Pops one complete message without blocking; application owns parsing/dedup.
    /// @param message Destination that is unchanged if no complete message exists.
    /// @return True when one message was copied, otherwise false.
    bool receive(Message &message);
    /// Samples diagnostic counters; not a synchronized product state snapshot.
    /// @return Current connection flags and counters, containing no credentials.
    Status status() const;

    /// Dispatches one MQTT event: the registered ESP-MQTT callback's
    /// production target, and -- because it depends on nothing beyond this
    /// object and a plain `esp_mqtt_event_t` -- also the deterministic test
    /// seam for the connect/subscribe/publish-ack state machine. A test
    /// constructs a session with a syntactically valid Config (init() only;
    /// never start()), then calls this directly with synthesized events to
    /// drive CONNECTED/SUBSCRIBED/DISCONNECTED/PUBLISHED/ERROR sequences
    /// without any real Wi-Fi, broker or FreeRTOS task. Production code never
    /// calls this directly; it arrives only through the registered callback.
    /// On MQTT_EVENT_CONNECTED this still calls the real
    /// esp_mqtt_client_subscribe() unless setSubscribeOverrideForTest() has
    /// replaced it -- that SDK call requires an actually-established
    /// connection to succeed, which a fake MQTT_EVENT_CONNECTED cannot
    /// provide on its own.
    /// @param id One ESP-MQTT event id (MQTT_EVENT_*).
    /// @param event Event payload; only the fields each case reads are used.
    void event(int32_t id, esp_mqtt_event_t &event);

    /// Replaces the SUBSCRIBE call issued on MQTT_EVENT_CONNECTED. Exists
    /// solely so a test can make that call succeed deterministically:
    /// ESP-MQTT's real esp_mqtt_client_subscribe() rejects it until the
    /// client has completed an actual broker handshake, which this session
    /// has no way to fake without a real broker. Pass nullptr to restore the
    /// real SDK call (the default; production code never calls this setter).
    /// @param subscribe Returns a nonnegative message id, or a negative
    ///        rejection, exactly like esp_mqtt_client_subscribe() would.
    void setSubscribeOverrideForTest(int (*subscribe)(const char *topic, int qos));

private:
    static void onEvent(void *arg, esp_event_base_t base, int32_t id, void *data);

    static constexpr size_t kMaxTrackedPublishes = 8;
    struct TrackedPublish {
        // One atomic word prevents the MQTT callback from acknowledging a
        // slot while the application task is replacing its three logical
        // fields. Bits 0..15 are the MQTT packet id, bits 16..30 are the
        // connection epoch and bit 31 is the acknowledgement flag. Epoch
        // wrap cannot affect a five-second pending publication in practice.
        std::atomic<uint32_t> state{0};
    };

    static constexpr uint32_t kTrackedIdMask = 0xffffu;
    static constexpr uint32_t kTrackedEpochMask = 0x7fffu;
    static constexpr uint32_t kTrackedAcknowledged = 0x80000000u;
    static uint32_t trackedState(int msg_id, uint32_t epoch, bool acknowledged);

    Config config_{};
    esp_mqtt_client_handle_t client_ = nullptr;
    bool mqtt_started_ = false; // Application task only.
    int64_t next_wifi_retry_us_ = 0;
    int64_t next_mqtt_retry_us_ = 0;
    uint32_t wifi_retry_ms_ = 1000;
    int subscribe_id_ = -1; // MQTT event task only.
    size_t next_tracked_slot_ = 0; // publish() caller only (single control task).
    std::atomic<bool> running_{false};
    std::atomic<bool> subscribed_{false};
    std::atomic<bool> reconnect_requested_{false};
    std::atomic<uint32_t> connections_{0}, rejected_{0}, overflow_{0}, errors_{0}, acknowledged_{0};
    std::atomic<uint32_t> pre_auth_transport_errors_{0};
    std::atomic<uint32_t> connection_epoch_{1}; // bumped on every disconnect
    TrackedPublish tracked_[kMaxTrackedPublishes]{};
    int (*subscribe_override_)(const char *topic, int qos) = nullptr; // test-only; see setSubscribeOverrideForTest()
    QueueHandle_t messages_ = nullptr;
    StaticQueue_t queue_control_{};
    alignas(Message) uint8_t queue_storage_[kQueueLength * sizeof(Message)]{};
    MessageBuffer<kMaxMessageBytes> incoming_; // MQTT event task only.
    Message delivery_{}; // MQTT callback scratch, avoids a 1 KiB task-stack temporary.
};

} // namespace pixels_network
