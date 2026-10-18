// Deterministic validation for docs/M0-BOOTSTRAP-IDENTITY-PROVISIONING.md:
// MAC-to-hardware_id derivation, the canonical base64url bootstrap-secret
// text contract, and the durable per-device NVS secret store. No real
// hardware eFuse read, broker, backend or secret is used anywhere below.
#include "pixels_bootstrap_identity.hpp"

#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "psa/crypto.h"

namespace {

using namespace pixels_bootstrap_identity;

int checks = 0, failures = 0;
void check(bool value, int line)
{
    ++checks;
    if (!value) { ++failures; std::printf("FAILED check %d (line %d)\n", checks, line); }
}
#define CHECK(expr) check((expr), __LINE__)

esp_err_t sha256(const void *bytes, size_t size, uint8_t digest[32])
{
    static const psa_status_t init_status = psa_crypto_init();
    if (init_status != PSA_SUCCESS) { return ESP_FAIL; }
    size_t out_size = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, static_cast<const uint8_t *>(bytes), size, digest, 32, &out_size) !=
            PSA_SUCCESS ||
        out_size != 32) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool hexEqual(const uint8_t *digest, const char *hex)
{
    char rendered[65]{};
    for (size_t i = 0; i < 32; ++i) { std::snprintf(rendered + i * 2, 3, "%02x", digest[i]); }
    return std::strcmp(rendered, hex) == 0;
}

} // namespace

extern "C" void app_main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // =========================================================================
    // Section 1 -- macToHardwareId(): pure, synthetic-MAC-testable.
    // =========================================================================
    {
        char out[kHardwareIdBytes + 1]{};
        const uint8_t all_zero[6] = {0, 0, 0, 0, 0, 0};
        macToHardwareId(all_zero, out);
        CHECK(std::strcmp(out, "000000000000") == 0);

        const uint8_t all_ff[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
        macToHardwareId(all_ff, out);
        CHECK(std::strcmp(out, "ffffffffffff") == 0);

        // The confirmed M0 physical unit's own eFuse MAC
        // (docs/M0-BOOTSTRAP-IDENTITY-PROVISIONING.md): 14:2b:2f:dc:fa:5c.
        const uint8_t m0_unit[6] = {0x14, 0x2b, 0x2f, 0xdc, 0xfa, 0x5c};
        macToHardwareId(m0_unit, out);
        CHECK(std::strcmp(out, "142b2fdcfa5c") == 0);

        const uint8_t mixed[6] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab};
        macToHardwareId(mixed, out);
        CHECK(std::strcmp(out, "0123456789ab") == 0); // lowercase, no separators, MSB first
    }

    // =========================================================================
    // Section 2 -- the closed public vector: bytes -> text -> SHA-256 ->
    // header, exactly as the backend contract fixes it.
    // =========================================================================
    constexpr uint8_t kVectorBytes[kSecretBytes] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
    };
    constexpr char kVectorText[] = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8";
    constexpr char kVectorSha256[] = "ea866a757e4c38babfa8127cbe9a409d3e1f93a00ff1488ff735fcf917afffd0";
    constexpr char kVectorHeader[] = "Authorization: Bearer AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8";
    static_assert(sizeof(kVectorText) - 1 == kSecretTextBytes, "public vector text must be exactly 43 bytes");
    {
        char encoded[kSecretTextBytes + 1]{};
        encodeSecretText(kVectorBytes, encoded);
        CHECK(std::strcmp(encoded, kVectorText) == 0);

        uint8_t decoded[kSecretBytes]{};
        CHECK(decodeSecretText(kVectorText, kSecretTextBytes, decoded) == ESP_OK);
        CHECK(std::memcmp(decoded, kVectorBytes, kSecretBytes) == 0);

        // The SHA-256 the backend contract names is over the 43 ASCII text
        // bytes transmitted, never the 32 decoded binary bytes.
        uint8_t digest_over_text[32]{};
        CHECK(sha256(kVectorText, kSecretTextBytes, digest_over_text) == ESP_OK);
        CHECK(hexEqual(digest_over_text, kVectorSha256));
        uint8_t digest_over_binary[32]{};
        CHECK(sha256(kVectorBytes, kSecretBytes, digest_over_binary) == ESP_OK);
        CHECK(!hexEqual(digest_over_binary, kVectorSha256)); // must NOT match -- different input entirely

        char header[96]{}; // "Authorization: Bearer " (22) + 43-char secret + NUL
        std::snprintf(header, sizeof(header), "Authorization: Bearer %s", encoded);
        CHECK(std::strcmp(header, kVectorHeader) == 0);

        // No padding, embedded separator or trailing newline anywhere in the
        // canonical text.
        CHECK(std::strchr(encoded, '=') == nullptr && std::strchr(encoded, '\n') == nullptr);
    }

    // =========================================================================
    // Section 3 -- a NUL never enters the hash or the header: encodeSecretText()
    // writes exactly 43 characters into a larger, pre-filled buffer, and
    // every length used downstream is the fixed kSecretTextBytes constant,
    // never strlen().
    // =========================================================================
    {
        char buffer[64];
        std::memset(buffer, 'Z', sizeof(buffer));
        encodeSecretText(kVectorBytes, buffer);
        CHECK(buffer[kSecretTextBytes] == '\0');
        for (size_t i = 0; i < kSecretTextBytes; ++i) { CHECK(buffer[i] != '\0'); }
        for (size_t i = kSecretTextBytes + 1; i < sizeof(buffer); ++i) { CHECK(buffer[i] == 'Z'); } // untouched
        uint8_t digest[32]{};
        CHECK(sha256(buffer, kSecretTextBytes, digest) == ESP_OK); // exact length, not strlen(buffer)
        CHECK(hexEqual(digest, kVectorSha256));
    }

    // =========================================================================
    // Section 4 -- decodeSecretText() rejects anything but the single
    // canonical encoding: wrong length, padding, whitespace, newline,
    // invalid alphabet, and a non-canonical trailing sextet.
    // =========================================================================
    {
        uint8_t decoded[kSecretBytes]{};
        char too_short[kSecretTextBytes]{};
        std::memcpy(too_short, kVectorText, kSecretTextBytes - 1);
        CHECK(decodeSecretText(too_short, kSecretTextBytes - 1, decoded) == ESP_ERR_INVALID_SIZE);

        char too_long[kSecretTextBytes + 2]{};
        std::memcpy(too_long, kVectorText, kSecretTextBytes);
        too_long[kSecretTextBytes] = 'A';
        CHECK(decodeSecretText(too_long, kSecretTextBytes + 1, decoded) == ESP_ERR_INVALID_SIZE);

        char with_padding[kSecretTextBytes + 1]{};
        std::memcpy(with_padding, kVectorText, kSecretTextBytes);
        with_padding[kSecretTextBytes - 1] = '=';
        CHECK(decodeSecretText(with_padding, kSecretTextBytes, decoded) == ESP_ERR_INVALID_ARG);

        char with_space[kSecretTextBytes + 1]{};
        std::memcpy(with_space, kVectorText, kSecretTextBytes);
        with_space[5] = ' ';
        CHECK(decodeSecretText(with_space, kSecretTextBytes, decoded) == ESP_ERR_INVALID_ARG);

        char with_newline[kSecretTextBytes + 1]{};
        std::memcpy(with_newline, kVectorText, kSecretTextBytes);
        with_newline[kSecretTextBytes - 1] = '\n';
        CHECK(decodeSecretText(with_newline, kSecretTextBytes, decoded) == ESP_ERR_INVALID_ARG);

        char with_nul[kSecretTextBytes + 1]{};
        std::memcpy(with_nul, kVectorText, kSecretTextBytes);
        with_nul[10] = '\0';
        CHECK(decodeSecretText(with_nul, kSecretTextBytes, decoded) == ESP_ERR_INVALID_ARG);

        char with_std_base64[kSecretTextBytes + 1]{};
        std::memcpy(with_std_base64, kVectorText, kSecretTextBytes);
        with_std_base64[0] = '+'; // standard base64, not base64url
        CHECK(decodeSecretText(with_std_base64, kSecretTextBytes, decoded) == ESP_ERR_INVALID_ARG);

        // Non-canonical: the last character's two unused low bits must be
        // zero. kVectorText's last character is '8' (value 60 = 0b111100,
        // already canonical); replace it with 'B' (value 1 = 0b000001,
        // bottom two bits nonzero) to force a non-canonical trailing sextet.
        char non_canonical[kSecretTextBytes + 1]{};
        std::memcpy(non_canonical, kVectorText, kSecretTextBytes);
        non_canonical[kSecretTextBytes - 1] = 'B';
        CHECK(decodeSecretText(non_canonical, kSecretTextBytes, decoded) == ESP_ERR_INVALID_ARG);
    }

    // =========================================================================
    // Section 5 -- durable store: empty partition, provision/round-trip
    // across a simulated reboot, refusal to overwrite, fault injection,
    // corruption/truncation, and eFuse incompatibility.
    // =========================================================================
    constexpr char kHardwareIdA[] = "142b2fdcfa5c";
    constexpr char kHardwareIdB[] = "a1b2c3d4e5f6";
    CHECK(nvs_flash_erase_partition("easyled") == ESP_OK);
    {
        BootstrapIdentityStore store;
        CHECK(store.init(kHardwareIdA) == ESP_OK);
        CHECK(store.state() == SecretState::Empty);
        CHECK(std::strcmp(store.hardwareId(), kHardwareIdA) == 0);
        char unused[kSecretTextBytes + 1]{};
        CHECK(store.secretText(unused) == ESP_ERR_INVALID_STATE); // nothing to read yet

        CHECK(store.provision(kVectorBytes) == ESP_OK);
        CHECK(store.state() == SecretState::Valid);
        char text[kSecretTextBytes + 1]{};
        CHECK(store.secretText(text) == ESP_OK);
        CHECK(std::strcmp(text, kVectorText) == 0);

        // Never a silent overwrite, even with different bytes.
        uint8_t other_bytes[kSecretBytes]{};
        std::memset(other_bytes, 0x7f, sizeof(other_bytes));
        CHECK(store.provision(other_bytes) == ESP_ERR_INVALID_STATE);
        CHECK(store.secretText(text) == ESP_OK && std::strcmp(text, kVectorText) == 0); // unchanged

        // Round-trip across a simulated reboot.
        store.end();
        CHECK(store.init(kHardwareIdA) == ESP_OK);
        CHECK(store.state() == SecretState::Valid);
        CHECK(store.secretText(text) == ESP_OK && std::strcmp(text, kVectorText) == 0);
        store.end();
    }

    // Fault injection before an SDK write leaves the store Empty and allows
    // a clean retry. Once an SDK write is attempted, the runtime fails closed
    // as Faulted until reboot/load resolves the durable result.
    for (int fault = static_cast<int>(StoreTestFault::BeforeWrite);
         fault <= static_cast<int>(StoreTestFault::DuringReadback); ++fault) {
        CHECK(nvs_flash_erase_partition("easyled") == ESP_OK);
        BootstrapIdentityStore store;
        CHECK(store.init(kHardwareIdA) == ESP_OK);
        CHECK(store.state() == SecretState::Empty);
        store.setFaultInjectionForTest(static_cast<StoreTestFault>(fault));
        CHECK(store.provision(kVectorBytes) != ESP_OK);
        const auto injected = static_cast<StoreTestFault>(fault);
        if (injected == StoreTestFault::BeforeWrite || injected == StoreTestFault::DuringWrite) {
            CHECK(store.state() == SecretState::Empty);
            CHECK(store.provision(kVectorBytes) == ESP_OK);
            CHECK(store.state() == SecretState::Valid);
        } else {
            CHECK(store.state() == SecretState::Faulted);
            CHECK(store.provision(kVectorBytes) == ESP_ERR_INVALID_STATE); // uncertain result cannot be overwritten
        }
        store.end();

        // Reboot/load is the authority after an uncertain durable write.
        CHECK(store.init(kHardwareIdA) == ESP_OK);
        if (injected == StoreTestFault::DuringCommit) {
            CHECK(store.state() == SecretState::Faulted); // committed synthetic garbage
        } else {
            CHECK(store.state() == SecretState::Valid); // no write, clean retry, or committed valid record
        }
        store.end();
    }

    // Corruption and truncation: raw NVS writes into this component's own
    // namespace/key (duplicated literally here, same convention as
    // validation_mqtt_credentials' own corruption test -- no private
    // helper is shared between targets).
    {
        CHECK(nvs_flash_erase_partition("easyled") == ESP_OK);
        nvs_handle_t handle = 0;
        CHECK(nvs_flash_init_partition("easyled") == ESP_OK);
        CHECK(nvs_open_from_partition("easyled", "easyled_boot", NVS_READWRITE, &handle) == ESP_OK);
        static constexpr uint8_t kGarbage[57] = {0xaa}; // same total size as the real record; wrong content
        CHECK(nvs_set_blob(handle, "identity", kGarbage, sizeof(kGarbage)) == ESP_OK);
        CHECK(nvs_commit(handle) == ESP_OK);
        nvs_close(handle);

        BootstrapIdentityStore store;
        CHECK(store.init(kHardwareIdA) == ESP_OK); // init() itself never fails on a bad record
        CHECK(store.state() == SecretState::Faulted); // never silently reinterpreted as Empty
        store.end();
    }
    {
        CHECK(nvs_flash_erase_partition("easyled") == ESP_OK);
        nvs_handle_t handle = 0;
        CHECK(nvs_flash_init_partition("easyled") == ESP_OK);
        CHECK(nvs_open_from_partition("easyled", "easyled_boot", NVS_READWRITE, &handle) == ESP_OK);
        static constexpr uint8_t kTruncated[10] = {0};
        CHECK(nvs_set_blob(handle, "identity", kTruncated, sizeof(kTruncated)) == ESP_OK);
        CHECK(nvs_commit(handle) == ESP_OK);
        nvs_close(handle);

        BootstrapIdentityStore store;
        CHECK(store.init(kHardwareIdA) == ESP_OK);
        CHECK(store.state() == SecretState::Faulted);
        store.end();
    }

    // eFuse incompatibility: a record provisioned for one hardware_id is
    // Faulted, never Valid, when loaded against a different one.
    {
        CHECK(nvs_flash_erase_partition("easyled") == ESP_OK);
        {
            BootstrapIdentityStore store;
            CHECK(store.init(kHardwareIdA) == ESP_OK);
            CHECK(store.provision(kVectorBytes) == ESP_OK);
            store.end();
        }
        BootstrapIdentityStore store;
        CHECK(store.init(kHardwareIdB) == ESP_OK); // init() itself still succeeds
        CHECK(store.state() == SecretState::Faulted); // eFuse is authority; NVS never silently redefines identity
        CHECK(std::strcmp(store.hardwareId(), kHardwareIdB) == 0); // reports the eFuse value, not the stale record's
        store.end();
    }

    // =========================================================================
    // Section 6 -- cross-tool compatibility: the exact 57 bytes
    // tools/provision_bootstrap_identity.py generated for hardware_id
    // 142b2fdcfa5c and the closed public vector (verified independently via
    // the official nvs_tool.py's hex dump before being transcribed here),
    // written raw into this component's own NVS key, must load as Valid
    // with the expected secret text -- proving the Python tool's record
    // layout and the firmware's are byte-for-byte the same contract, not
    // just two implementations that happen to agree in each one's own tests.
    // =========================================================================
    {
        static constexpr uint8_t kToolGeneratedRecord[57] = {
            0x44, 0x49, 0x53, 0x42, 0x01, 0x00, 0x00, 0x00, 0x31, 0x34, 0x32, 0x62, 0x32, 0x66, 0x64, 0x63,
            0x66, 0x61, 0x35, 0x63, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
            0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a,
            0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x5d, 0x01, 0x4e, 0x06,
        };
        CHECK(nvs_flash_erase_partition("easyled") == ESP_OK);
        nvs_handle_t handle = 0;
        CHECK(nvs_flash_init_partition("easyled") == ESP_OK);
        CHECK(nvs_open_from_partition("easyled", "easyled_boot", NVS_READWRITE, &handle) == ESP_OK);
        CHECK(nvs_set_blob(handle, "identity", kToolGeneratedRecord, sizeof(kToolGeneratedRecord)) == ESP_OK);
        CHECK(nvs_commit(handle) == ESP_OK);
        nvs_close(handle);

        BootstrapIdentityStore store;
        CHECK(store.init("142b2fdcfa5c") == ESP_OK);
        CHECK(store.state() == SecretState::Valid);
        char text[kSecretTextBytes + 1]{};
        CHECK(store.secretText(text) == ESP_OK);
        CHECK(std::strcmp(text, kVectorText) == 0);
        store.end();
    }

    // =========================================================================
    // No secret ever reaches a log line: audited by code inspection (same
    // basis as pixels_mqtt_credentials/pixels_mqtt_session_adapter), not a
    // QEMU check. BootstrapIdentityStore::hardwareId()/state() never expose
    // the secret; only secretText() does, and main.cpp's own diagnostics
    // never call it.
    // =========================================================================

    std::printf("BOOTID_VALIDATION checks=%d failures=%d RESULT=%s\n", checks, failures,
               failures == 0 ? "ALL_PASS" : "FAIL");
    for (;;) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}
