#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "firmware_contract.hpp"
#include "nvs.h"

namespace pixels_storage {

constexpr const char *kPartitionLabel = "easyled";
constexpr size_t kTerminalCapacity = 8;

/// Durable metadata describing the pixel payload currently selected by the
/// A/B staging store.
struct ActiveSnapshot {
    bool active = false;
    pixels_firmware::CommandType type = pixels_firmware::CommandType::ApplyConfig;
    uint32_t config_version = 0;
    char command_id[pixels_firmware::kUuidBytes + 1]{};
    char preset_id[pixels_firmware::kUuidBytes + 1]{};
};

/// Application target that can atomically replace or clear the live pixel
/// configuration at a render-frame boundary.
class ActivationTarget {
public:
    /// Releases an activation target through its interface.
    virtual ~ActivationTarget() = default;

    /// Applies exact persisted payload bytes at a render-frame boundary.
    /// @param type Payload kind; ApplyConfig or ActivatePreset.
    /// @param payload Exact JSON payload bytes.
    /// @param size Payload byte count.
    /// @return ESP_OK only after the live target matches the payload.
    virtual esp_err_t activateAtFrameBoundary(pixels_firmware::CommandType type,
                                               const char *payload, size_t size) = 0;

    /// Clears the live target at a render-frame boundary.
    /// @return ESP_OK only after the live target is empty.
    virtual esp_err_t clearAtFrameBoundary() = 0;
};

/// Fixed-capacity terminal-result journal stored in the dedicated EasyLED NVS
/// partition. Existing command IDs are immutable; new records replace the
/// oldest of eight entries only after the replacement commits and reads back.
class NvsTerminalJournal final : public pixels_firmware::TerminalJournal {
public:
    /// Closes an open NVS namespace; persisted entries remain.
    ~NvsTerminalJournal() override;

    /// Opens and scans the dedicated EasyLED NVS partition without erasing it.
    /// @return ESP_OK, or the NVS initialization/open error.
    esp_err_t init();

    /// Closes the namespace and forgets RAM metadata; persisted entries remain.
    void end();

    /// Finds a terminal result without modifying it.
    /// @param command_id Canonical UUID.
    /// @param record Receives the exact stored result.
    /// @return ESP_OK, ESP_ERR_NOT_FOUND, or a storage/data error.
    esp_err_t find(const char *command_id, pixels_firmware::TerminalRecord &record) const override;

    /// Stores a terminal result durably and verifies its bytes by readback.
    /// @param record Complete immutable terminal result.
    /// @return ESP_OK after verification, or a storage/data error.
    esp_err_t store(const pixels_firmware::TerminalRecord &record) override;

private:
    static constexpr size_t kWireBytes = 576;
    nvs_handle_t handle_ = 0;
    bool initialized_ = false;
    uint64_t next_sequence_ = 1;
    bool occupied_[kTerminalCapacity]{};
    uint64_t sequences_[kTerminalCapacity]{};
    char command_ids_[kTerminalCapacity][pixels_firmware::kUuidBytes + 1]{};
    mutable uint8_t scratch_[kWireBytes]{};
};

/// NVS-backed implementation of the command executor's durable staging
/// transaction. It stores exact payload bytes, retains the prior A/B active
/// record until the executor has durably journaled success, and keeps the last
/// successful base configuration separately so a scene-only preset can recover
/// its output topology and canvas after reboot. Single control-task use only.
class NvsDurableStaging final : public pixels_firmware::DurableStaging {
public:
    /// Closes an open NVS namespace; persisted active data remains.
    ~NvsDurableStaging() override;

    /// Opens storage, restores the newest valid active payload through target,
    /// and discards an unactivated pending candidate from an interrupted boot.
    /// An active preset restores the retained base configuration first.
    /// No partition erase or format migration is attempted.
    /// Pending activation is reconciled against the terminal journal: completed
    /// commands stay active; missing terminal results restore the prior state.
    /// @param target Live activation boundary; must outlive this instance.
    /// @param journal Initialized terminal journal used for boot reconciliation.
    /// @return ESP_OK, ESP_ERR_NOT_FOUND when no active payload exists, or an error.
    esp_err_t init(ActivationTarget &target,
                   const pixels_firmware::TerminalJournal &journal);

    /// Closes storage and clears transaction RAM; persisted active data remains.
    void end();

    /// Reports whether init() or a completed activation found an active payload.
    /// @return True when a valid active record is selected.
    bool hasActive() const { return active_slot_ >= 0; }

    /// Checks the durable active preset identity and exact payload checksum.
    /// @param preset_id Canonical preset UUID.
    /// @param checksum Expected exact payload digest.
    /// @return True only for the same active preset and digest.
    bool activePresetMatches(const char *preset_id,
                             const uint8_t checksum[pixels_firmware::kSha256Bytes]) const;

    /// Copies the durable active command metadata used to construct retained state.
    /// @param snapshot Receives an owned snapshot; `active` is false for an empty store.
    /// @return ESP_OK, or ESP_ERR_INVALID_STATE before init().
    esp_err_t activeSnapshot(ActiveSnapshot &snapshot) const;

    /// Starts one staging transaction after finalizing the prior success.
    /// @param command Parsed command whose payload will be stored.
    /// @param total_size Exact payload byte count expected from append().
    /// @return ESP_OK, or an argument, state, or storage error.
    esp_err_t begin(const pixels_firmware::Command &command, size_t total_size) override;

    /// Appends exact payload bytes to the current bounded transaction.
    /// @param bytes Payload bytes; may be null only when size is zero.
    /// @param size Number of bytes to append.
    /// @return ESP_OK, or an argument, size, or state error.
    esp_err_t append(const void *bytes, size_t size) override;

    /// Verifies SHA-256, commits the pending record, and verifies it by readback.
    /// @param expected Expected digest of the exact appended payload bytes.
    /// @return ESP_OK only after durable verification, or an error.
    esp_err_t verifyAndCommit(
        const uint8_t expected[pixels_firmware::kSha256Bytes]) override;

    /// Applies the committed candidate and durably selects its A/B active slot.
    /// @return ESP_OK only after live activation and durable readback.
    esp_err_t activateAtFrameBoundary() override;

    /// Removes pending/rollback metadata after terminal success is durable.
    /// @return ESP_OK after cleanup, or a storage/state error.
    esp_err_t finalize() override;

    /// Restores the prior durable/live state and removes the candidate.
    /// @return ESP_OK after rollback, or an activation/storage error.
    esp_err_t rollback() override;

private:
    static constexpr size_t kMaxPayloadBytes = 8192;
    static constexpr size_t kRecordHeaderBytes = 130;
    static constexpr size_t kWireBytes = kRecordHeaderBytes + kMaxPayloadBytes + 4;

    esp_err_t scanActive();
    esp_err_t loadSlot(int slot, pixels_firmware::CommandType &type, const char *&payload,
                       size_t &size, uint64_t &sequence, char command_id[37],
                       uint32_t &config_version, char preset_id[37],
                       uint8_t checksum[pixels_firmware::kSha256Bytes]);
    esp_err_t eraseKey(const char *key);
    esp_err_t commitAndVerify(const char *key, const uint8_t *bytes, size_t size);
    esp_err_t finalizePrevious();
    void resetTransaction();

    nvs_handle_t handle_ = 0;
    bool initialized_ = false;
    ActivationTarget *target_ = nullptr;
    const pixels_firmware::TerminalJournal *journal_ = nullptr;

    int active_slot_ = -1;
    uint64_t active_sequence_ = 0;
    pixels_firmware::CommandType active_type_ = pixels_firmware::CommandType::ApplyConfig;
    uint32_t active_config_version_ = 0;
    char active_command_id_[pixels_firmware::kUuidBytes + 1]{};
    char active_preset_id_[37]{};
    uint8_t active_checksum_[pixels_firmware::kSha256Bytes]{};

    pixels_firmware::Command command_{};
    size_t expected_size_ = 0;
    size_t staged_size_ = 0;
    uint8_t staged_checksum_[pixels_firmware::kSha256Bytes]{};
    bool begun_ = false;
    bool committed_ = false;
    bool live_applied_ = false;
    int prior_slot_ = -1;
    int transaction_slot_ = -1;

    mutable uint8_t record_[kWireBytes]{};
    mutable pixels_firmware::TerminalRecord terminal_scratch_{};
};

/// Small durable metadata store for retained-state sequencing. It increments
/// the boot counter once per init, reserves monotonically increasing state
/// sequence numbers before publication, and remembers the newest applied
/// command ID. Single control-task use only.
class NvsTelemetryState final {
public:
    /// Closes an open NVS namespace; persisted counters remain.
    ~NvsTelemetryState();

    /// Opens the telemetry namespace and durably increments the boot counter.
    /// @return ESP_OK after commit/readback, or an NVS/data error.
    esp_err_t init();

    /// Closes the namespace and clears RAM state; persisted values remain.
    void end();

    /// Returns the boot counter committed by init().
    /// @return Current boot counter, or zero before init().
    uint32_t bootCounter() const { return boot_counter_; }

    /// Durably reserves the next retained-state sequence value.
    /// @param sequence Receives the committed nonzero sequence on success.
    /// @return ESP_OK after readback, or a state/NVS/data error.
    esp_err_t nextStateSequence(uint32_t &sequence);

    /// Durably records the most recent command whose terminal result is applied.
    /// @param command_id Canonical command UUID.
    /// @return ESP_OK after readback, or an argument/state/NVS error.
    esp_err_t noteApplied(const char *command_id);

    /// Returns the durable last-applied UUID, or null when none was recorded.
    /// @return Stable pointer owned by this object, or null.
    const char *lastAppliedCommandId() const
    {
        return last_applied_command_id_[0] == '\0' ? nullptr : last_applied_command_id_;
    }

private:
    nvs_handle_t handle_ = 0;
    bool initialized_ = false;
    uint32_t boot_counter_ = 0;
    uint32_t next_state_sequence_ = 1;
    char last_applied_command_id_[pixels_firmware::kUuidBytes + 1]{};
};

} // namespace pixels_storage
