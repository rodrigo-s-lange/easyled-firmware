#pragma once

#include <cstddef>
#include <cstring>

namespace pixels_network {

/// Bounded assembly of one MQTT message; used only by the MQTT event task.
template <size_t Capacity>
class MessageBuffer {
public:
    enum class Result { Incomplete, Complete, Rejected };

    /// Discards any partial message after a disconnect or invalid fragment.
    void reset() { used_ = 0; total_ = 0; active_ = false; }

    /// Appends a contiguous fragment; a new offset-zero fragment starts a message.
    /// @param bytes Fragment data; may be null only for a zero-length fragment.
    /// @param length Fragment byte count.
    /// @param offset Fragment offset in the complete message.
    /// @param total Complete message byte count announced by MQTT.
    /// @return Complete, Incomplete, or Rejected without exposing partial content.
    Result append(const char *bytes, int length, int offset, int total)
    {
        if (length < 0 || offset < 0 || total < 0 ||
            static_cast<size_t>(total) > Capacity || offset > total ||
            length > total - offset || (length > 0 && bytes == nullptr)) {
            reset();
            return Result::Rejected;
        }
        if (offset == 0) {
            reset();
            total_ = static_cast<size_t>(total);
            active_ = true;
        }
        if (!active_ || total_ != static_cast<size_t>(total) ||
            used_ != static_cast<size_t>(offset) || (length == 0 && total > 0)) {
            reset();
            return Result::Rejected;
        }
        if (length > 0) { std::memcpy(bytes_ + used_, bytes, static_cast<size_t>(length)); }
        used_ += static_cast<size_t>(length);
        if (used_ != total_) { return Result::Incomplete; }
        bytes_[used_] = '\0'; // Convenience only; embedded NUL remains payload data.
        active_ = false;
        return Result::Complete;
    }

    /// Returns storage for a just-completed message, valid until the next append.
    /// @return Payload storage; callers must use size(), not strlen().
    const char *data() const { return bytes_; }
    /// Returns the accumulated byte count.
    /// @return Payload length, excluding the convenience trailing NUL.
    size_t size() const { return used_; }

private:
    char bytes_[Capacity + 1]{};
    size_t used_ = 0;
    size_t total_ = 0;
    bool active_ = false;
};

} // namespace pixels_network
