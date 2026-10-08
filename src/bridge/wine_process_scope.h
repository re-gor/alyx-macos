#pragma once
#include <stddef.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif
// Current native process only. Copies a lowercase exact Windows executable
// basename from bounded KERN_PROCARGS2 argv0, after up to two exact Wine loaders.
// Never scans arbitrary arguments/environment. False clears out[0] if possible.
// This is kernel launch identity, not a private PEB query or a Windows PID map.
bool wine_bridge_own_pe_name(char* out, size_t capacity);
#ifdef __cplusplus
}
#endif
