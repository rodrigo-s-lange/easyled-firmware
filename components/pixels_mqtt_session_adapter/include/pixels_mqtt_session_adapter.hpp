#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "network_session.hpp"
#include "pixels_mqtt_credentials.hpp"

// Product/application orchestration layer connecting the already-tested
// pixels_mqtt_credentials controller to the real pixels_network::NetworkSession,
// per docs/MQTT-SESSION-INTEGRATION-TASK.md. Deliberately its own small
// component rather than folded into pixels_control_runtime: it depends only
// on pixels_network and pixels_mqtt_credentials, never on the pixel/Composer
// stack, so a focused validation target can exercise it without pulling in
// unrelated dependencies. pixels_mqtt_credentials itself stays independent of
// pixels_network -- this component is the only thing that depends on both.

namespace pixels_mqtt_session {

/// Stable backing storage for the provisioned MQTT topic set currently in
/// effect. `pixels_control::MqttEventSink`/`MqttTelemetrySink` capture
/// pointers into this struct's own arrays at construction time (never into a
/// `StoredCredential` or any other buffer that can be cleared/reused) and are
/// never reconstructed afterward: rotating the topic set is overwriting
/// these arrays in place, from the single control task, atomically with
/// respect to every reader on that same task. Starts zeroed, so a publish
/// attempted before the first identity is live simply fails
/// `NetworkSession::publish()`'s own topic validation rather than using a
/// stale or wrong topic.
struct ProvisionedTopics {
    char state[pixels_mqtt::kMaxTopicBytes + 1]{};
    char event[pixels_mqtt::kMaxTopicBytes + 1]{};
    char telemetry[pixels_mqtt::kMaxTopicBytes + 1]{};
    char cmd[pixels_mqtt::kMaxTopicBytes + 1]{};
};

/// Copies every field of `credential`'s topic set into `topics` in one
/// synchronous call, so the whole set rotates together with the MQTT
/// identity it belongs to -- there is no window where a reader sees some
/// topics from the old identity and some from the new one.
/// @param credential The credential whose topics are now live.
/// @param topics Destination snapshot, overwritten in place.
void rotateProvisionedTopics(const pixels_mqtt::StoredCredential &credential, ProvisionedTopics &topics);

/// Thin application-layer adapter from `NetworkSession` to
/// `pixels_mqtt::MqttTransportSession`. Owns copies of every credential
/// string it needs past restart(): the real MQTT client never borrows
/// `StoredCredential`/pending buffers, so a later credential-store mutation
/// cannot change a live connection out from under it. Samples
/// `NetworkSession::status()` only -- never calls its poll(), which the
/// application's single control-loop owns exclusively.
class NetworkSessionMqttTransport final : public pixels_mqtt::MqttTransportSession {
public:
    /// @param session Wi-Fi-initialized (initWifi()) and started application
    ///        session; this adapter only ever calls its *Mqtt() methods.
    /// @param clock Monotonic clock for the connect timeout and the
    ///        post-confirmation reconnect deadline -- injected (the same
    ///        interface and rationale as MqttCredentialCoordinator's own
    ///        clock) so a test can control attempt/deadline timing without
    ///        a real multi-second/minute wait. Must outlive this object.
    NetworkSessionMqttTransport(pixels_network::NetworkSession &session, pixels_mqtt::MonotonicClock &clock);

    /// Copies `identity`'s strings, then calls `NetworkSession::restartMqtt()`.
    /// Wi-Fi is never touched.
    /// @param identity Resolved broker/credential/command-topic/CA.
    /// @return ESP_OK once restartMqtt() has accepted the new identity, or
    ///         its immediate setup error.
    esp_err_t restart(const pixels_mqtt::MqttSessionIdentity &identity) override;
    /// Calls `NetworkSession::endMqtt()`. Idempotent.
    void end() override;
    /// Reports Connected once per restart() on the first fresh CONNACK,
    /// Ambiguous on an immediate failure/disconnect/error before that
    /// CONNACK or on a bounded connect timeout, and -- once already
    /// Connected this restart() cycle -- StillTrying through any ordinary
    /// brief loss/automatic reconnect, or Ambiguous once a bounded
    /// post-confirmation reconnect deadline passes with no fresh CONNACK.
    /// @return See above; never blocks.
    Outcome poll() override;

private:
    static void copyBounded(char *destination, size_t capacity, const char *source);

    pixels_network::NetworkSession &session_;
    pixels_mqtt::MonotonicClock &clock_;
    char broker_uri_[pixels_mqtt::kMaxBrokerUriBytes + 1]{};
    char client_id_[pixels_mqtt::kUuidBytes + 1]{};
    char username_[pixels_mqtt::kUuidBytes + 1]{};
    char password_[pixels_mqtt::kMaxPasswordBytes + 1]{};
    char command_topic_[pixels_mqtt::kMaxTopicBytes + 1]{};
    // Borrowed, not copied: every TrustResolver in this codebase
    // (TableTrustResolver) resolves trust_ref to a pointer into its own
    // firmware-static entries table, which outlives every session -- never
    // a request-scoped or StoredCredential-owned buffer. See restart().
    const char *server_ca_pem_ = nullptr;
    uint32_t baseline_connections_ = 0;
    uint32_t baseline_errors_ = 0;
    uint32_t baseline_epoch_ = 0;
    int64_t attempt_started_us_ = 0;
    bool connected_this_attempt_ = false;
    uint32_t last_connections_ = 0;
    uint32_t last_epoch_ = 0;
    int64_t disconnected_since_us_ = -1;
};

} // namespace pixels_mqtt_session
