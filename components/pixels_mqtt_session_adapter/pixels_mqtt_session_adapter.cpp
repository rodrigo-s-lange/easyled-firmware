#include "pixels_mqtt_session_adapter.hpp"

#include <cstring>

namespace pixels_mqtt_session {
namespace {
constexpr int64_t kInitialConnectTimeoutUs = 20LL * 1000 * 1000; // generous: Wi-Fi/TLS/CONNACK handshake
constexpr int64_t kReconnectDeadlineUs = 60LL * 1000 * 1000; // many ESP-MQTT auto-reconnect tries (3 s interval)
} // namespace

void rotateProvisionedTopics(const pixels_mqtt::StoredCredential &credential, ProvisionedTopics &topics)
{
    std::memcpy(topics.state, credential.topic_state, sizeof(topics.state));
    std::memcpy(topics.event, credential.topic_event, sizeof(topics.event));
    std::memcpy(topics.telemetry, credential.topic_telemetry, sizeof(topics.telemetry));
    std::memcpy(topics.cmd, credential.topic_cmd, sizeof(topics.cmd));
}

NetworkSessionMqttTransport::NetworkSessionMqttTransport(pixels_network::NetworkSession &session,
                                                          pixels_mqtt::MonotonicClock &clock)
    : session_(session), clock_(clock)
{
}

void NetworkSessionMqttTransport::copyBounded(char *destination, size_t capacity, const char *source)
{
    const size_t len = source == nullptr ? 0 : strnlen(source, capacity);
    std::memcpy(destination, source == nullptr ? "" : source, len);
    destination[len] = '\0';
}

esp_err_t NetworkSessionMqttTransport::restart(const pixels_mqtt::MqttSessionIdentity &identity)
{
    // Copy every bounded identity string before touching the transport:
    // NetworkSession's MqttConfig only borrows these pointers, so they must
    // outlive it independently of whatever the caller's own StoredCredential/
    // pending buffer does next.
    copyBounded(broker_uri_, sizeof(broker_uri_), identity.broker_uri);
    copyBounded(client_id_, sizeof(client_id_), identity.client_id);
    copyBounded(username_, sizeof(username_), identity.username);
    copyBounded(password_, sizeof(password_), identity.password);
    copyBounded(command_topic_, sizeof(command_topic_), identity.command_topic);
    server_ca_pem_ = identity.server_ca_pem; // firmware-static trust-table entry; see header comment

    pixels_network::NetworkSession::MqttConfig config{};
    config.broker_uri = broker_uri_;
    config.client_id = client_id_;
    config.username = username_;
    config.password = password_;
    config.command_topic = command_topic_;
    config.server_ca_pem = server_ca_pem_;
    const esp_err_t err = session_.restartMqtt(config);

    const auto status = session_.status();
    baseline_connections_ = status.connections;
    baseline_errors_ = status.transport_errors;
    baseline_epoch_ = status.connection_epoch;
    attempt_started_us_ = clock_.nowUs();
    connected_this_attempt_ = false;
    disconnected_since_us_ = -1;
    return err;
}

void NetworkSessionMqttTransport::end()
{
    session_.endMqtt();
}

NetworkSessionMqttTransport::Outcome NetworkSessionMqttTransport::poll()
{
    const auto status = session_.status();

    if (!connected_this_attempt_) {
        if (status.connections != baseline_connections_) {
            connected_this_attempt_ = true;
            last_connections_ = status.connections;
            last_epoch_ = status.connection_epoch;
            disconnected_since_us_ = -1;
            return Outcome::Connected;
        }
        if (status.transport_errors != baseline_errors_ || status.connection_epoch != baseline_epoch_) {
            return Outcome::Ambiguous; // disconnect/transport error before any CONNACK
        }
        if (clock_.nowUs() - attempt_started_us_ > kInitialConnectTimeoutUs) {
            return Outcome::Ambiguous; // monotonic attempt timeout
        }
        return Outcome::StillTrying;
    }

    // Monitoring phase: already reported Connected once for this restart().
    // Repeated automatic-reconnect CONNACKs under the SAME identity are
    // evidence of health, never a new credential promotion to re-report.
    if (status.connections != last_connections_) {
        last_connections_ = status.connections;
        last_epoch_ = status.connection_epoch;
        disconnected_since_us_ = -1;
        return Outcome::StillTrying;
    }
    if (status.connection_epoch != last_epoch_) {
        // Disconnected since last observed; NetworkSession/ESP-MQTT retry on
        // their own -- ordinary brief loss needs no action here. Only start
        // the bounded deadline once, at the first sign of this gap.
        last_epoch_ = status.connection_epoch;
        if (disconnected_since_us_ < 0) { disconnected_since_us_ = clock_.nowUs(); }
    }
    if (disconnected_since_us_ >= 0 && clock_.nowUs() - disconnected_since_us_ > kReconnectDeadlineUs) {
        return Outcome::Ambiguous; // bounded persistent failure -> existing retry/BootstrapRecovery path
    }
    return Outcome::StillTrying;
}

} // namespace pixels_mqtt_session
