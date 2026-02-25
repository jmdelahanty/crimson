#pragma once

#include <cstdlib>
#include <cstring>

inline bool crimson_seek_debug_logs_enabled() {
    static const bool enabled = []() {
        const char* env = std::getenv("CRIMSON_SEEK_DEBUG_LOGS");
        if (!env || env[0] == '\0') {
            return false;
        }
        return std::strcmp(env, "0") != 0 &&
               std::strcmp(env, "false") != 0 &&
               std::strcmp(env, "FALSE") != 0 &&
               std::strcmp(env, "off") != 0 &&
               std::strcmp(env, "OFF") != 0;
    }();
    return enabled;
}
