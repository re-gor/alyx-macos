#pragma once
#include <cstdint>

// Only call from an existing Wine thread (GetLoopback/GetCapture callbacks).
// This function uses Wine's private server protocol; it is not HAL-thread safe.
struct DmnSceneSourceSnapshot {
    uint32_t wine_pid;
    int32_t native_pid;
    // Darwin process start epoch in microseconds; guards native PID reuse.
    uint64_t start_time;
    uint64_t generation;
    bool ready;
};

// false means quiet idle: out is initialized, ready=false, native_pid=0.
// No global-audio fallback. The helper only observes an already running VR server.
bool dmn_scene_source_snapshot(DmnSceneSourceSnapshot* out) noexcept;
