#pragma once

#include <cstdlib>
#include <cstring>

inline bool crimson_env_flag_enabled(const char* name) {
    const char* env = std::getenv(name);
    if (!env || env[0] == '\0') {
        return false;
    }
    return std::strcmp(env, "0") != 0 &&
           std::strcmp(env, "false") != 0 &&
           std::strcmp(env, "FALSE") != 0 &&
           std::strcmp(env, "off") != 0 &&
           std::strcmp(env, "OFF") != 0;
}

inline bool crimson_seek_debug_logs_enabled() {
    static const bool enabled = []() {
        return crimson_env_flag_enabled("CRIMSON_SEEK_DEBUG_LOGS");
    }();
    return enabled;
}

inline bool crimson_stimulus_debug_logs_enabled() {
    static const bool enabled = []() {
        if (crimson_env_flag_enabled("CRIMSON_STIMULUS_DEBUG_LOGS")) {
            return true;
        }
        return crimson_seek_debug_logs_enabled();
    }();
    return enabled;
}

inline bool crimson_clipped_startup_trace_enabled() {
    static const bool enabled = []() {
        return crimson_env_flag_enabled("CRIMSON_CLIPPED_STARTUP_TRACE");
    }();
    return enabled;
}
