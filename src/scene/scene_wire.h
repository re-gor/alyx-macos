#pragma once
#include <stdint.h>

// Fixed-size private PIPE record; never an arbitrary PID control file.
struct DmnSceneWire {
    uint32_t magic, version, size, publisher_pid;
    uint64_t publisher_creation;
    uint32_t server_pid, scene_state;
    uint64_t server_creation, sequence;
    uint32_t scene_pid, flags;
    uint64_t scene_creation;
    uint64_t observed_time; // Windows UTC FILETIME; freshness, not PID identity.
    char executable[256];
    char app_key[128];
};
static_assert(sizeof(DmnSceneWire) == 456);
static_assert(__builtin_offsetof(DmnSceneWire, scene_creation) == 56);
constexpr uint32_t DmnSceneWireMagic = 0x314e4353; // SCN1
constexpr uint32_t DmnSceneWireVersion = 2;
constexpr uint32_t DmnSceneWireVRReady = 1;
constexpr uint32_t DmnSceneWireSceneValid = 2;
constexpr uint32_t DmnSceneWireSystem = 4;
