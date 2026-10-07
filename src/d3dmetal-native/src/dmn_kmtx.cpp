/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Cross-process keyed mutex implementation. See dmn_kmtx.h.
 */

#include <sched.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <new>
#include <unordered_map>

#include <dxgi1_2.h>

#include "dmn_hook.h"
#include "dmn_kmtx.h"
#include "dmn_log.h"
#include "dmn_share.h"

namespace {

constexpr uint32_t kKmtxMagic = 0x324d4b44u; /* 'DKM2': owning-device layout */

/* The shared state page. Fields are only touched under `lock` (a cross-process
 * spinlock word taken with atomic exchange); waiting is poll+backoff. */
struct KmtxShared {
    uint32_t magic;
    volatile uint32_t lock;     /* 0 free, 1 held */
    volatile uint64_t key;      /* key it was last released with */
    volatile uint32_t acquired; /* 0 released, 1 acquired, 2 releasing */
    volatile uint32_t owner_pid;
    volatile uint64_t owner_device; /* canonical IUnknown identity, local to PID */
};
static_assert(sizeof(KmtxShared) == 32, "keyed-mutex shared layout drift");

void kmtx_lock(KmtxShared* s) {
    while (__atomic_exchange_n(&s->lock, 1u, __ATOMIC_ACQUIRE) != 0)
        sched_yield();
}

void kmtx_unlock(KmtxShared* s) {
    __atomic_store_n(&s->lock, 0u, __ATOMIC_RELEASE);
}

uint64_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* == The vended COM object ================================================ */
/* IDXGIKeyedMutex : IDXGIDeviceSubObject : IDXGIObject : IUnknown. */
struct VendedKmtx {
    void** vtbl;
    std::atomic<ULONG> refs;
    KmtxShared* state;   /* inside `map` */
    void* map;
    size_t map_size;
    // Own the device/query, never the texture or a persistent external context
    // reference. A bound context can retain this texture; keeping its external
    // reference here could add a mutex -> context -> texture cycle.
    ID3D11Device* device = nullptr;
    uint64_t device_identity = 0;
    ID3D11Query* gpu_done = nullptr;
    HRESULT gpu_setup = E_FAIL;
    std::mutex release_mutex; // serialize reuse of this object's event query
};

HRESULT STDMETHODCALLTYPE km_QueryInterface(VendedKmtx* self, REFIID riid, void** ppv) {
    if (!ppv)
        return E_POINTER;
    if (dmn_iid_eq(riid, __uuidof(IUnknown)) ||
        dmn_iid_eq(riid, __uuidof(IDXGIObject)) ||
        dmn_iid_eq(riid, __uuidof(IDXGIDeviceSubObject)) ||
        dmn_iid_eq(riid, __uuidof(IDXGIKeyedMutex))) {
        self->refs.fetch_add(1, std::memory_order_relaxed);
        *ppv = self;
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}
ULONG STDMETHODCALLTYPE km_AddRef(VendedKmtx* self) {
    return self->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}
ULONG STDMETHODCALLTYPE km_Release(VendedKmtx* self) {
    /* The registry holds one reference until dmn_kmtx_unregister (texture
     * destruction); destruction happens when the app's refs also drain. */
    ULONG r = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (r == 0) {
        if (self->gpu_done) self->gpu_done->Release();
        if (self->device) self->device->Release();
        dmn_share_unmap(self->map, self->map_size);
        delete self;
    }
    return r;
}
HRESULT STDMETHODCALLTYPE km_SetPrivateData(VendedKmtx*, REFGUID, UINT, const void*) { return S_OK; }
HRESULT STDMETHODCALLTYPE km_SetPrivateDataInterface(VendedKmtx*, REFGUID, const IUnknown*) { return S_OK; }
HRESULT STDMETHODCALLTYPE km_GetPrivateData(VendedKmtx*, REFGUID, UINT*, void*) { return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE km_GetParent(VendedKmtx*, REFIID, void** ppv) {
    if (ppv) *ppv = nullptr;
    return E_NOTIMPL;
}
HRESULT STDMETHODCALLTYPE km_GetDevice(VendedKmtx*, REFIID, void** ppv) {
    if (ppv) *ppv = nullptr;
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE km_AcquireSync(VendedKmtx* self, UINT64 key, DWORD ms) {
    static std::atomic<unsigned> trace_count{0};
    bool trace = trace_count.fetch_add(1) < 16;
    KmtxShared* s = self->state;
    const bool infinite = (ms == 0xffffffffu);
    const uint64_t deadline = infinite ? 0 : now_ms() + ms;
    uint32_t spins = 0;
    for (;;) {
        kmtx_lock(s);
        if (trace && spins == 0)
            DMN_INFO("kmtx: acquire pid=%d want=%llu key=%llu held=%u timeout=%u",
                     getpid(), (unsigned long long)key,
                     (unsigned long long)s->key, s->acquired, ms);
        if (!s->acquired && s->key == key) {
            s->acquired = 1;
            s->owner_pid = static_cast<uint32_t>(getpid());
            s->owner_device = self->device_identity;
            kmtx_unlock(s);
            if (trace) DMN_INFO("kmtx: acquired pid=%d key=%llu", getpid(), (unsigned long long)key);
            return S_OK;
        }
        kmtx_unlock(s);
        if (!infinite && now_ms() >= deadline) {
            if (trace) DMN_INFO("kmtx: timed out pid=%d want=%llu", getpid(), (unsigned long long)key);
            return (HRESULT)WAIT_TIMEOUT;
        }
        if (spins < 128) {
            sched_yield();
            spins++;
        } else {
            struct timespec ns = {0, 200 * 1000}; /* 0.2 ms */
            nanosleep(&ns, nullptr);
        }
    }
}

HRESULT STDMETHODCALLTYPE km_ReleaseSync(VendedKmtx* self, UINT64 key) {
    static std::atomic<unsigned> trace_count{0};
    std::lock_guard<std::mutex> release_guard(self->release_mutex);
    KmtxShared* s = self->state;
    kmtx_lock(s);
    if (trace_count.fetch_add(1) < 16)
        DMN_INFO("kmtx: release pid=%d key=%llu held=%u", getpid(),
                 (unsigned long long)key, s->acquired);
    if (!s->acquired) {
        kmtx_unlock(s);
        return DXGI_ERROR_INVALID_CALL;
    }
    const uint32_t pid = static_cast<uint32_t>(getpid());
    if (s->acquired != 1 || s->owner_pid != pid ||
        s->owner_device != self->device_identity) {
        static std::atomic<unsigned> rejected_count{0};
        if (rejected_count.fetch_add(1) < 16)
            DMN_INFO("kmtx: rejected release pid=%u device=%llx owner_pid=%u "
                     "owner_device=%llx held=%u", pid,
                     (unsigned long long)self->device_identity, s->owner_pid,
                     (unsigned long long)s->owner_device, s->acquired);
        kmtx_unlock(s);
        return E_FAIL;
    }
    // Claim the release under the shared lock. Another mutex interface for the
    // same device must not publish a second release during GPU completion.
    s->acquired = 2;
    kmtx_unlock(s);
    struct ReleaseClaim {
        KmtxShared* state;
        uint32_t pid;
        uint64_t device;
        bool published = false;
        ~ReleaseClaim() {
            if (published) return;
            // Setup/query/timeout errors retain the acquisition and allow a
            // later retry by the same device; never expose incomplete writes.
            kmtx_lock(state);
            if (state->acquired == 2 && state->owner_pid == pid &&
                state->owner_device == device)
                state->acquired = 1;
            kmtx_unlock(state);
        }
    } claim{s, pid, self->device_identity};

    // Flush submits the owning process's writes, but does not complete them.
    // Publishing a CPU key immediately after Flush lets the peer's GPU copy
    // race those writes. Keep the shared mutex acquired until an EVENT query
    // after the preceding commands reports completion. Do not hold the shared
    // spinlock during this bounded wait; a peer can still observe a timeout.
    if (FAILED(self->gpu_setup)) return self->gpu_setup;
    ID3D11DeviceContext* context = nullptr;
    self->device->GetImmediateContext(&context);
    if (!context) return E_FAIL;
    struct ContextRelease {
        ID3D11DeviceContext* value;
        ~ContextRelease() { value->Release(); }
    } context_ref{context};
    context->End(self->gpu_done);
    context->Flush();
    const uint64_t deadline = now_ms() + 5000;
    for (;;) {
        BOOL complete = FALSE;
        HRESULT hr = context->GetData(self->gpu_done, &complete,
                                      sizeof(complete), 0);
        if (FAILED(hr)) {
            DMN_ERROR("kmtx: GPU completion query failed 0x%08x; key not released",
                      (unsigned)hr);
            return hr;
        }
        if (hr == S_OK && complete) break;
        HRESULT removed = self->device->GetDeviceRemovedReason();
        if (FAILED(removed)) return removed;
        if (now_ms() >= deadline) {
            DMN_ERROR("kmtx: GPU completion timed out; key not released");
            return static_cast<HRESULT>(0x80070102u); // HRESULT_FROM_WIN32(WAIT_TIMEOUT)
        }
        struct timespec ns = {0, 200 * 1000};
        nanosleep(&ns, nullptr);
    }

    kmtx_lock(s);
    if (s->acquired != 2 || s->owner_pid != pid ||
        s->owner_device != self->device_identity) {
        kmtx_unlock(s);
        DMN_ERROR("kmtx: release ownership changed during GPU completion");
        return E_FAIL;
    }
    s->key = key;
    s->owner_pid = 0;
    s->owner_device = 0;
    s->acquired = 0;
    claim.published = true;
    kmtx_unlock(s);
    return S_OK;
}

void* g_km_vtbl[10] = {
    (void*)km_QueryInterface, (void*)km_AddRef, (void*)km_Release,
    (void*)km_SetPrivateData, (void*)km_SetPrivateDataInterface,
    (void*)km_GetPrivateData, (void*)km_GetParent, (void*)km_GetDevice,
    (void*)km_AcquireSync, (void*)km_ReleaseSync,
};

std::mutex g_km_mtx;
std::unordered_map<void*, VendedKmtx*> g_km_reg; /* texture identity -> mutex */

} // namespace

bool dmn_kmtx_register(void* texture_identity, int fd, uint64_t offset, bool init,
                       ID3D11Device* device) {
    if (!texture_identity || fd < 0 || !device)
        return false;
    // Different interfaces of one D3D device share its COM identity. Device
    // pointers from other processes need the PID as a separate discriminator.
    IUnknown* identity = nullptr;
    HRESULT identity_hr = device->QueryInterface(__uuidof(IUnknown),
                                                (void**)&identity);
    if (FAILED(identity_hr) || !identity) {
        if (identity) identity->Release();
        DMN_ERROR("kmtx: device identity unavailable 0x%08x", (unsigned)identity_hr);
        return false;
    }
    const uint64_t device_identity = reinterpret_cast<uintptr_t>(identity);
    identity->Release(); // device remains alive through the registered reference
    {
        std::lock_guard<std::mutex> lk(g_km_mtx);
        if (g_km_reg.count(texture_identity))
            return true;
    }
    size_t map_size = (size_t)offset + dmn_share_page_align(sizeof(KmtxShared));
    void* map = dmn_share_map_fd(fd, map_size);
    if (!map) {
        DMN_ERROR("kmtx: map(fd=%d, %zu) failed", fd, map_size);
        return false;
    }
    auto* s = reinterpret_cast<KmtxShared*>(static_cast<char*>(map) + offset);
    if (init) {
        s->lock = 0;
        s->key = 0;
        s->acquired = 0;
        s->owner_pid = 0;
        s->owner_device = 0;
        __atomic_store_n(&s->magic, kKmtxMagic, __ATOMIC_RELEASE);
    } else if (__atomic_load_n(&s->magic, __ATOMIC_ACQUIRE) != kKmtxMagic) {
        DMN_ERROR("kmtx: state page missing magic (producer without keyed "
                  "mutex, or layout drift)");
        dmn_share_unmap(map, map_size);
        return false;
    }
    auto* km = new (std::nothrow) VendedKmtx();
    if (!km) {
        dmn_share_unmap(map, map_size);
        return false;
    }
    km->vtbl = g_km_vtbl;
    km->refs.store(1, std::memory_order_relaxed); /* the registry's reference */
    km->state = s;
    km->map = map;
    km->map_size = map_size;
    km->device = device;
    km->device_identity = device_identity;
    device->AddRef();
    D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
    km->gpu_setup = device->CreateQuery(&desc, &km->gpu_done);
    if (FAILED(km->gpu_setup))
        DMN_ERROR("kmtx: GPU completion setup failed 0x%08x; ReleaseSync will fail",
                  (unsigned)km->gpu_setup);
    {
        std::lock_guard<std::mutex> lk(g_km_mtx);
        g_km_reg[texture_identity] = km;
    }
    DMN_INFO("kmtx: %s keyed mutex for texture %p (fd=%d off=%llu)",
             init ? "created" : "opened", texture_identity, fd,
             (unsigned long long)offset);
    return true;
}

IDXGIKeyedMutex* dmn_kmtx_lookup(void* texture_identity) {
    std::lock_guard<std::mutex> lk(g_km_mtx);
    auto it = g_km_reg.find(texture_identity);
    if (it == g_km_reg.end())
        return nullptr;
    km_AddRef(it->second);
    return reinterpret_cast<IDXGIKeyedMutex*>(it->second);
}

void dmn_kmtx_unregister(void* texture_identity) {
    VendedKmtx* km = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_km_mtx);
        auto it = g_km_reg.find(texture_identity);
        if (it == g_km_reg.end())
            return;
        km = it->second;
        g_km_reg.erase(it);
    }
    km_Release(km); /* the registry's reference from dmn_kmtx_register */
    DMN_INFO("kmtx: unregistered keyed mutex for texture %p", texture_identity);
}
