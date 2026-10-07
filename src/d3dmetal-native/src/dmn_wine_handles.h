/* Experimental legacy DXGI handles for Wine: an opaque 32-bit token, resolved
 * by a per-process Unix-socket broker. NT handles are intentionally untouched.
 * The private socket directory must be shared by the participating processes. */
#pragma once
#include <cstddef>
#include <cstdint>

bool dmn_wine_handles_enabled();
bool dmn_wine_is_handle(void* handle);
int32_t dmn_wine_export_handle(void* identity, const void* pod, size_t size,
                               void** handle);
int32_t dmn_wine_import_handle(void* handle, void* pod, size_t capacity,
                               size_t* size, int* fd);
void dmn_wine_evict_handle(void* identity);
