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
        "../fixtures/event-capability-reported.provisional.json")
