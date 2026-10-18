#include "pixels_bootstrap_identity.hpp"

#include <cstring>

#include "esp_mac.h"
#include "nvs_flash.h"

namespace pixels_bootstrap_identity {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";
constexpr char kBase64UrlAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

bool hardwareIdWellFormed(const char *value)
{
    if (value == nullptr) { return false; }
    for (size_t i = 0; i < kHardwareIdBytes; ++i) {
        const char c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { return false; }
    }
    return value[kHardwareIdBytes] == '\0';
}

int8_t base64UrlValue(char c)
{
    if (c >= 'A' && c <= 'Z') { return static_cast<int8_t>(c - 'A'); }
    if (c >= 'a' && c <= 'z') { return static_cast<int8_t>(c - 'a' + 26); }
    if (c >= '0' && c <= '9') { return static_cast<int8_t>(c - '0' + 52); }
    if (c == '-') { return 62; }
    if (c == '_') { return 63; }
    return -1; // not in the base64url alphabet -- covers padding, whitespace, NUL too
}

uint32_t crc32(const uint8_t *bytes, size_t size)
{
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

constexpr const char *kNamespace = "easyled_boot"; // <=15 chars: NVS_KEY_NAME_MAX_SIZE-1
constexpr const char *kRecordKey = "identity";
constexpr uint32_t kRecordMagic = 0x42534944u; // "BSID"-ish, arbitrary distinct value
constexpr uint8_t kSchema = 1;

#pragma pack(push, 1)
struct WireRecord {
    uint32_t magic;
    uint8_t schema;
    uint8_t reserved[3];
    char hardware_id[kHardwareIdBytes + 1];
    uint8_t secret[kSecretBytes];
    uint32_t crc; // over every byte above, computed last
};
#pragma pack(pop)

void sealWithCrc(WireRecord &wire)
{
    wire.crc = 0;
    wire.crc = crc32(reinterpret_cast<const uint8_t *>(&wire), sizeof(wire) - sizeof(wire.crc));
}

bool crcValid(const WireRecord &wire)
{
    return crc32(reinterpret_cast<const uint8_t *>(&wire), sizeof(wire) - sizeof(wire.crc)) == wire.crc;
}

} // namespace

void macToHardwareId(const uint8_t mac[6], char out[kHardwareIdBytes + 1])
{
    for (size_t i = 0; i < 6; ++i) {
        out[i * 2] = kHexDigits[(mac[i] >> 4) & 0x0F];
        out[i * 2 + 1] = kHexDigits[mac[i] & 0x0F];
    }
    out[kHardwareIdBytes] = '\0';
}

esp_err_t currentHardwareId(char out[kHardwareIdBytes + 1])
{
    uint8_t mac[6]{};
    const esp_err_t err = esp_efuse_mac_get_default(mac);
    if (err != ESP_OK) { return err; }
    macToHardwareId(mac, out);
    return ESP_OK;
}

void encodeSecretText(const uint8_t bytes[kSecretBytes], char out[kSecretTextBytes + 1])
{
    size_t out_i = 0;
    size_t i = 0;
    for (; i + 3 <= kSecretBytes; i += 3) {
        const uint32_t v = (static_cast<uint32_t>(bytes[i]) << 16) |
            (static_cast<uint32_t>(bytes[i + 1]) << 8) | bytes[i + 2];
        out[out_i++] = kBase64UrlAlphabet[(v >> 18) & 0x3Fu];
        out[out_i++] = kBase64UrlAlphabet[(v >> 12) & 0x3Fu];
        out[out_i++] = kBase64UrlAlphabet[(v >> 6) & 0x3Fu];
        out[out_i++] = kBase64UrlAlphabet[v & 0x3Fu];
    }
    // kSecretBytes (32) is 10 full 3-byte groups (30 bytes) plus exactly 2
    // trailing bytes -- fixed by this function's signature, never a 1-byte
    // remainder, so only this one tail shape is handled.
    const uint8_t b0 = bytes[i];
    const uint8_t b1 = bytes[i + 1];
    out[out_i++] = kBase64UrlAlphabet[(b0 >> 2) & 0x3Fu];
    out[out_i++] = kBase64UrlAlphabet[((b0 & 0x03u) << 4) | ((b1 >> 4) & 0x0Fu)];
    // Trailing sextet: only its top 4 bits come from input (b1's low
    // nibble); the bottom 2 bits have no more input and are always zero --
    // this is what makes this encoding canonical.
    out[out_i++] = kBase64UrlAlphabet[(b1 & 0x0Fu) << 2];
    out[out_i] = '\0';
}

esp_err_t decodeSecretText(const char *text, size_t size, uint8_t out[kSecretBytes])
{
    if (text == nullptr || size != kSecretTextBytes) { return ESP_ERR_INVALID_SIZE; }
    int8_t values[kSecretTextBytes];
    for (size_t i = 0; i < kSecretTextBytes; ++i) {
        values[i] = base64UrlValue(text[i]);
        if (values[i] < 0) { return ESP_ERR_INVALID_ARG; } // rejects padding/whitespace/NUL/non-alphabet
    }
    uint8_t decoded[kSecretBytes];
    size_t out_i = 0;
    size_t i = 0;
    for (; i + 4 <= kSecretTextBytes - 3; i += 4) {
        const uint32_t v = (static_cast<uint32_t>(values[i]) << 18) |
            (static_cast<uint32_t>(values[i + 1]) << 12) | (static_cast<uint32_t>(values[i + 2]) << 6) |
            static_cast<uint32_t>(values[i + 3]);
        decoded[out_i++] = static_cast<uint8_t>((v >> 16) & 0xFFu);
        decoded[out_i++] = static_cast<uint8_t>((v >> 8) & 0xFFu);
        decoded[out_i++] = static_cast<uint8_t>(v & 0xFFu);
    }
    // Exactly 3 trailing characters remain (43 - 40), decoding to 2 bytes;
    // the third character's bottom 2 bits must be exactly zero for this
    // text to be the canonical encoding of its own decoded bytes.
    if (values[i + 2] & 0x03) { return ESP_ERR_INVALID_ARG; }
    decoded[out_i++] = static_cast<uint8_t>((values[i] << 2) | (values[i + 1] >> 4));
    decoded[out_i++] = static_cast<uint8_t>(((values[i + 1] & 0x0F) << 4) | (values[i + 2] >> 2));
    std::memcpy(out, decoded, kSecretBytes);
    return ESP_OK;
}

BootstrapIdentityStore::~BootstrapIdentityStore()
{
    end();
}

esp_err_t BootstrapIdentityStore::load()
{
    std::memset(secret_, 0, sizeof(secret_));
    WireRecord wire{};
    size_t size = sizeof(wire);
    const esp_err_t err = nvs_get_blob(handle_, kRecordKey, &wire, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        state_ = SecretState::Empty;
        return ESP_OK;
    }
    if (err != ESP_OK || size != sizeof(wire) || wire.magic != kRecordMagic || wire.schema != kSchema ||
        !crcValid(wire) || !hardwareIdWellFormed(wire.hardware_id)) {
        state_ = SecretState::Faulted; // present but unusable -- never silently treated as Empty
        return ESP_OK;
    }
    if (std::strcmp(wire.hardware_id, hardware_id_) != 0) {
        // The eFuse value is authority: a record for a different physical
        // unit must never be reinterpreted as this one's secret.
        state_ = SecretState::Faulted;
        return ESP_OK;
    }
    std::memcpy(secret_, wire.secret, sizeof(secret_));
    state_ = SecretState::Valid;
    return ESP_OK;
}

esp_err_t BootstrapIdentityStore::init(const char hardware_id[kHardwareIdBytes + 1])
{
    if (initialized_ || !hardwareIdWellFormed(hardware_id)) { return ESP_ERR_INVALID_ARG; }
    std::strcpy(hardware_id_, hardware_id);
    esp_err_t err = nvs_flash_init_partition("easyled");
    if (err != ESP_OK) { return err; }
    err = nvs_open_from_partition("easyled", kNamespace, NVS_READWRITE, &handle_);
    if (err != ESP_OK) { return err; }
    err = load();
    if (err != ESP_OK) {
        nvs_close(handle_);
        handle_ = 0;
        return err;
    }
    initialized_ = true;
    return ESP_OK;
}

void BootstrapIdentityStore::end()
{
    if (handle_ != 0) {
        nvs_close(handle_);
        handle_ = 0;
    }
    initialized_ = false;
    state_ = SecretState::Empty;
    std::memset(hardware_id_, 0, sizeof(hardware_id_));
    std::memset(secret_, 0, sizeof(secret_));
}

SecretState BootstrapIdentityStore::state() const
{
    return state_;
}

const char *BootstrapIdentityStore::hardwareId() const
{
    return hardware_id_;
}

esp_err_t BootstrapIdentityStore::secretText(char out[kSecretTextBytes + 1]) const
{
    if (state_ != SecretState::Valid) { return ESP_ERR_INVALID_STATE; }
    encodeSecretText(secret_, out);
    return ESP_OK;
}

esp_err_t BootstrapIdentityStore::provision(const uint8_t secret[kSecretBytes])
{
    if (!initialized_) { return ESP_ERR_INVALID_STATE; }
    if (state_ != SecretState::Empty) { return ESP_ERR_INVALID_STATE; } // never a silent overwrite

    WireRecord wire{};
    wire.magic = kRecordMagic;
    wire.schema = kSchema;
    std::strcpy(wire.hardware_id, hardware_id_);
    std::memcpy(wire.secret, secret, kSecretBytes);
    sealWithCrc(wire);

    esp_err_t err;
    bool sdk_write_attempted = false;
    if (fault_for_test_ == StoreTestFault::BeforeWrite) {
        err = ESP_FAIL;
    } else if (fault_for_test_ == StoreTestFault::DuringWrite) {
        err = ESP_FAIL;
    } else {
        sdk_write_attempted = true;
        err = nvs_set_blob(handle_, kRecordKey, &wire, sizeof(wire));
        if (err == ESP_OK) {
            if (fault_for_test_ == StoreTestFault::DuringCommit) {
                // As in pixels_mqtt_credentials: a bare nvs_set_blob() with
                // no following nvs_commit() can still be visible to a
                // same-process handle close+reopen, which is not what a
                // real power cut would leave. Commit real garbage instead,
                // so the record is unambiguously invalid on any subsequent
                // load() regardless of that visibility detail.
                static constexpr uint8_t kFaultGarbage[4] = {0xde, 0xad, 0xbe, 0xef};
                nvs_set_blob(handle_, kRecordKey, kFaultGarbage, sizeof(kFaultGarbage));
                nvs_commit(handle_);
                err = ESP_FAIL;
            } else {
                err = nvs_commit(handle_);
                if (err == ESP_OK) {
                    WireRecord verify{};
                    size_t verify_size = sizeof(verify);
                    err = nvs_get_blob(handle_, kRecordKey, &verify, &verify_size);
                    if (err == ESP_OK) {
                        const bool forced_mismatch = fault_for_test_ == StoreTestFault::DuringReadback;
                        if (forced_mismatch || verify_size != sizeof(wire) ||
                            std::memcmp(&verify, &wire, sizeof(wire)) != 0) {
                            err = ESP_ERR_INVALID_RESPONSE;
                        }
                    }
                }
            }
        }
    }
    fault_for_test_ = StoreTestFault::None;
    if (err != ESP_OK) {
        // Once an SDK write was attempted, flash may contain no record, a
        // committed valid record, or an invalid/partial record. Do not leave
        // RAM at Empty, because that would allow a second provision() call to
        // overwrite an uncertain first result without a reboot/load or erase.
        if (sdk_write_attempted) { state_ = SecretState::Faulted; }
        std::memset(secret_, 0, sizeof(secret_));
        return err;
    }

    std::memcpy(secret_, secret, kSecretBytes);
    state_ = SecretState::Valid;
    return ESP_OK;
}

void BootstrapIdentityStore::setFaultInjectionForTest(StoreTestFault fault)
{
    fault_for_test_ = fault;
}

} // namespace pixels_bootstrap_identity
