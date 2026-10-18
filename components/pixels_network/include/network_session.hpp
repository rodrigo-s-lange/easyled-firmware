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
    /// MQTT/TLS certificate-date validation needs a plausible wall clock.
    /// This session will not start MQTT, and a caller should not call into
    /// any MQTT-identity controller/tick either, before `std::time(nullptr)`
    /// reaches this value (2024-01-01T00:00:00Z) -- see poll()'s own gate and
    /// docs/MQTT-SESSION-INTEGRATION-TASK.md, "o mesmo portão de relógio".
    static constexpr int64_t kMinPlausibleUnixTime = 1704067200;

    /// Wi-Fi-only identity: ssid/password alone, no MQTT broker/credential.
    struct WifiConfig {
        const char *ssid = nullptr;
        const char *wifi_password = nullptr;
    };

    /// MQTT-only identity: one broker/credential/command-topic set. Strings
    /// must remain valid until the next configureMqtt()/restartMqtt()/endMqtt()
    /// or end().
    struct MqttConfig {
        const char *broker_uri = nullptr; // wss:// only; no userinfo in URI.
        const char *client_id = nullptr;
        const char *username = nullptr;
        const char *password = nullptr;
        const char *command_topic = nullptr; // Exact topic, never a wildcard.
        const char *server_ca_pem = nullptr; // Null uses the ESP-IDF certificate bundle.
    };

    /// Combined Wi-Fi + MQTT identity, for init()'s compatibility wrapper.
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
        /// Bumped on every MQTT disconnect and every deliberate MQTT
        /// teardown (restartMqtt()/endMqtt()/stop()/end()), including one
        /// that delivers no DISCONNECTED callback at all (for example a
        /// deliberate client destruction). A caller that snapshots this
        /// value right after restartMqtt()/configureMqtt() returns can
        /// detect "something about this connection already changed since my
        /// attempt started" without needing any further per-attempt state.
        uint32_t connection_epoch;
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
    /// Compatibility wrapper for laboratory mode and existing callers/tests:
    /// equivalent to initWifi() followed by configureMqtt() with this same
    /// Config's respective fields, with the same all-or-nothing validation.
    /// @param config Configuration whose strings must remain valid until end().
    /// @return ESP_OK, ESP_ERR_INVALID_ARG/STATE, or an SDK allocation/setup error.
    esp_err_t init(const Config &config);
    /// Prepares Wi-Fi and the bounded incoming-message queue only. No MQTT
    /// client exists yet -- configureMqtt()/restartMqtt() may be called
    /// later, at any point before or after start(), to add or change the
    /// MQTT identity without ever repeating this call.
    /// @param config Configuration whose strings must remain valid until end().
    /// @return ESP_OK, ESP_ERR_INVALID_ARG/STATE, or ESP_ERR_NO_MEM.
    esp_err_t initWifi(const WifiConfig &config);
    /// Creates the MQTT client for a session that has no MQTT identity yet
    /// (initWifi() only, or after endMqtt()). Does not start Wi-Fi and does
    /// not touch any existing Wi-Fi connection. Use restartMqtt() instead
    /// once an MQTT client already exists.
    /// @param config Configuration whose strings must remain valid until the
    ///        next configureMqtt()/restartMqtt()/endMqtt() or end().
    /// @return ESP_OK, ESP_ERR_INVALID_ARG/STATE (Wi-Fi not yet initialized,
    ///         or an MQTT client already exists), or an SDK setup error.
    esp_err_t configureMqtt(const MqttConfig &config);
    /// Ends whatever MQTT identity is live (safe if none yet) and creates a
    /// new one, without disconnecting Wi-Fi, without touching SNTP/trusted
    /// time, and without affecting durable pixel state or rendering. Clears
    /// queued/incomplete inbound commands from the previous identity and
    /// invalidates every previously tracked publish to PublishAck::Unknown
    /// (see Status::connection_epoch); a callback already in flight from the
    /// destroyed client cannot mark the new identity connected or subscribed
    /// -- see event()'s own stale-client guard.
    /// @param config Configuration whose strings must remain valid until the
    ///        next configureMqtt()/restartMqtt()/endMqtt() or end().
    /// @return ESP_OK, ESP_ERR_INVALID_ARG/STATE (Wi-Fi not yet initialized),
    ///         or an SDK stop/destroy/setup error.
    esp_err_t restartMqtt(const MqttConfig &config);
    /// Ends the current MQTT identity, if any, leaving Wi-Fi, IP, trusted
    /// wall clock, durable pixel state and rendering untouched. Idempotent.
    /// @return ESP_OK, or an SDK stop/destroy error.
    esp_err_t endMqtt();
    /// Starts Wi-Fi; MQTT starts from poll() once IP, a valid TLS clock and a
    /// configured MQTT client (from init()/configureMqtt()/restartMqtt())
    /// all exist. Valid to call before any MQTT identity exists.
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
    /// calls this directly; it arrives only through the registered callback,
    /// which (unlike this method) also drops any event whose own
    /// `event.client` no longer matches the live client -- a late callback
    /// queued from a client that restartMqtt()/endMqtt()/end() has since
    /// destroyed. That filter lives in the callback glue, not here, so this
    /// test seam is unaffected by it.
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

    static bool validWifiConfig(const WifiConfig &config);
    static bool validMqttConfig(const MqttConfig &config);
    /// Builds the esp_mqtt_client_config_t, calls esp_mqtt_client_init() and
    /// registers onEvent(). On any failure client_ is destroyed and left
    /// null. Does not touch config_, trackers or the incoming queue -- every
    /// caller (init(), configureMqtt()) does that itself afterward.
    esp_err_t createMqttClient(const MqttConfig &config);
    /// Shared MQTT-only teardown used by both endMqtt() and restartMqtt():
    /// stops (if started), destroys the client, bumps connection_epoch_ so
    /// every previously tracked publish becomes Unknown, and clears
    /// subscription/queue state from the old identity. Requires client_ !=
    /// nullptr; leaves client_ null on return.
    esp_err_t teardownMqtt();

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
    bool wifi_ready_ = false; // true once initWifi()/init() has prepared Wi-Fi + the queue.
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
    std::atomic<uint32_t> connection_epoch_{1}; // bumped on every disconnect/deliberate MQTT teardown; see Status::connection_epoch
    TrackedPublish tracked_[kMaxTrackedPublishes]{};
    int (*subscribe_override_)(const char *topic, int qos) = nullptr; // test-only; see setSubscribeOverrideForTest()
    QueueHandle_t messages_ = nullptr;
    StaticQueue_t queue_control_{};
    alignas(Message) uint8_t queue_storage_[kQueueLength * sizeof(Message)]{};
    MessageBuffer<kMaxMessageBytes> incoming_; // MQTT event task only.
    Message delivery_{}; // MQTT callback scratch, avoids a 1 KiB task-stack temporary.
};

} // namespace pixels_network
