#include "message_buffer.hpp"
#include <cstdio>
#include <cstring>
#include <climits>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" void app_main()
{
    using Buffer = pixels_network::MessageBuffer<8>;
    using Result = Buffer::Result;
    Buffer buffer;
    int checks = 0, failures = 0;
    auto check = [&](bool value) { ++checks; if (!value) { ++failures; std::printf("FAILED check %d\n", checks); } };

    // Real MQTT fragments do not contain a whole command until the final part.
    check(buffer.append("abc", 3, 0, 8) == Result::Incomplete);
    check(buffer.append("defgh", 5, 3, 8) == Result::Complete);
    check(buffer.size() == 8 && std::memcmp(buffer.data(), "abcdefgh\0", 9) == 0);
    check(buffer.append("x", 1, 8, 8) == Result::Rejected);
    check(buffer.append("x", 1, 0, 9) == Result::Rejected);
    check(buffer.append("x", 1, 1, 2) == Result::Rejected); // orphan continuation

    check(buffer.append("ab", 2, 0, 4) == Result::Incomplete);
    check(buffer.append("c", 1, 3, 4) == Result::Rejected); // gap invalidates whole message
    check(buffer.append("cd", 2, 2, 4) == Result::Rejected);
    check(buffer.append("ab", 2, 0, 4) == Result::Incomplete);
    check(buffer.append("cd", 2, 2, 5) == Result::Rejected); // changed total
    check(buffer.append("ab", 2, 0, 4) == Result::Incomplete);
    buffer.reset(); // disconnect discards a partial command
    check(buffer.append("cd", 2, 2, 4) == Result::Rejected);

    check(buffer.append(nullptr, 1, 0, 1) == Result::Rejected);
    check(buffer.append("a", -1, 0, 1) == Result::Rejected);
    check(buffer.append("a", 1, -1, 1) == Result::Rejected);
    check(buffer.append("a", INT_MAX, INT_MAX, INT_MAX) == Result::Rejected);
    check(buffer.append(nullptr, 0, 0, 1) == Result::Rejected);
    check(buffer.append(nullptr, 0, 0, 0) == Result::Complete);
    check(buffer.size() == 0 && buffer.data()[0] == '\0');
    const char binary[] = {'a', '\0', 'b'};
    check(buffer.append(binary, 3, 0, 3) == Result::Complete);
    check(buffer.size() == 3 && std::memcmp(buffer.data(), binary, 3) == 0);

    // A new complete message replaces an abandoned partial one.
    check(buffer.append("ab", 2, 0, 4) == Result::Incomplete);
    check(buffer.append("ok", 2, 0, 2) == Result::Complete);
    check(buffer.size() == 2 && std::memcmp(buffer.data(), "ok", 2) == 0);

    for (;;) {
        std::printf("NETWORK_VALIDATION checks=%d failures=%d RESULT=%s\n", checks, failures,
                    failures == 0 ? "ALL_PASS" : "FAIL");
        std::fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
