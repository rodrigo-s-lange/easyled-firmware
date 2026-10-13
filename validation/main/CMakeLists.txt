idf_component_register(SRCS "main.cpp"
    REQUIRES pixels_network pixels_firmware_contract pixels_payload_adapter pixels_command_executor
             pixels_durable_storage pixels_control_runtime nvs_flash
             esp_pixels esp_pixels_fx esp_pixels_composer adapter_esp_pixels freertos
    EMBED_TXTFILES
        "../fixtures/apply-config-command.provisional.json"
        "../fixtures/config-payload-bench-gpio16-50.provisional.json"
        "../fixtures/sample-config-payload.canonical.json"
        "../fixtures/activate-preset-command.provisional.json"
        "../fixtures/preset-payload.provisional.json"
        "../fixtures/event-command-received.provisional.json"
        "../fixtures/event-command-accepted.provisional.json"
        "../fixtures/event-command-applied.provisional.json"
        "../fixtures/event-checksum-mismatch-rejected.provisional.json"
        "../fixtures/event-command-failed.provisional.json"
        "../fixtures/state-config-active.provisional.json"
        "../fixtures/event-capability-reported.provisional.json"
        "../fixtures/p1-config-download/sample-config-payload.json"
        "../fixtures/p1-config-download/bench-31x12-config-v1.json"
        "../fixtures/p1-config-download/bench-31x12-config-v2.json"
        "../fixtures/p1-config-download/two-outputs-holes-config.json"
        "../fixtures/p1-config-download/invalid-ledmap-schema-version-2.json"
        "../fixtures/p1-config-download/invalid-ledmap-map-length-mismatch.json"
        "../fixtures/p1-config-download/invalid-ledmap-duplicate-index.json"
        "../fixtures/p1-config-download/invalid-ledmap-index-out-of-range.json"
        "../fixtures/p1-config-download/invalid-segment-color-short-hex.json")
