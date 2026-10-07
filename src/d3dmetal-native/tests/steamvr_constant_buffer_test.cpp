/* Exact SteamVR frame-info shape: DEFAULT, CONSTANT_BUFFER, SHARED, 400 bytes.
 * A fresh peer imports once, then GPU-reads every byte after each producer
 * update. Socket acknowledgements deliberately serialize the two processes;
 * this tests sharing, not SteamVR's GPU fence protocol. */
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <time.h>
#include <unistd.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include "d3dmetal_native.h"
#include "common/com.h"
#include "common/ipc.h"
#define T_TAG "STEAMVR400"
#include "common/check.h"

namespace {
constexpr unsigned kWords = 100, kTicks = 32;
using Frame = std::array<uint32_t, kWords>;
bool wine_handles() {
    const char* mode = getenv("DMN_WINE_SHARING");
    return mode && strcmp(mode, "1") == 0;
}

Frame frame(unsigned tick) {
    Frame data{};
    for (unsigned i = 0; i < data.size(); ++i)
        data[i] = 0x46100000u ^ (tick * 0x10001u) ^ (i * 0x9e3779b9u);
    return data;
}

uint64_t milliseconds() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return uint64_t(t.tv_sec) * 1000 + uint64_t(t.tv_nsec) / 1000000;
}

int device(Com<ID3D11Device>& dev, Com<ID3D11DeviceContext>& ctx) {
    EXPECT(dmn_init(nullptr) == DMN_SUCCESS, "dmn_init failed");
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0, actual;
    CK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                        &level, 1, D3D11_SDK_VERSION, &dev, &actual, &ctx),
       "D3D11CreateDevice");
    return 0;
}

int producer(int sock) {
    Com<ID3D11Device> dev;
    Com<ID3D11DeviceContext> ctx;
    EXPECT(device(dev, ctx) == 0, "producer device failed");
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = sizeof(Frame);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    Com<ID3D11Buffer> buffer;
    CK_OK(dev->CreateBuffer(&desc, nullptr, &buffer), "CreateBuffer(400)");
    Com<IDXGIResource> resource;
    CK(buffer->QueryInterface(__uuidof(IDXGIResource), (void**)&resource), "QI");
    HANDLE handle = nullptr;
    CK_OK(resource->GetSharedHandle(&handle), "GetSharedHandle");
    EXPECT(handle != nullptr, "empty shared handle");
    if (wine_handles()) {
        uintptr_t value = reinterpret_cast<uintptr_t>(handle);
        EXPECT(value <= UINT32_MAX, "legacy handle cannot survive SteamVR truncation");
        HANDLE again = nullptr;
        CK(resource->GetSharedHandle(&again), "Repeated GetSharedHandle");
        EXPECT(again == handle, "legacy handle identity changed");
        uint32_t token = uint32_t(value);
        EXPECT(write(sock, &token, sizeof(token)) == sizeof(token), "send token failed");
    } else {
        dmn_shared_buffer_handle wire{};
        memcpy(&wire, handle, sizeof(wire));
        EXPECT(wire.magic == DMN_SHARED_BUFFER_MAGIC && wire.size >= sizeof(Frame),
               "bad shared buffer metadata");
        EXPECT(send_with_fd(sock, &wire, sizeof(wire), wire.fd), "send fd failed");
    }
    char ack{};
    EXPECT(read(sock, &ack, 1) == 1 && ack == 'R', "peer import failed");
    D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
    Com<ID3D11Query> done;
    CK(dev->CreateQuery(&qd, &done), "CreateQuery");
    for (unsigned tick = 1; tick <= kTicks; ++tick) {
        Frame data = frame(tick);
        ctx->UpdateSubresource(buffer.ptr(), 0, nullptr, data.data(), 0, 0);
        ctx->End(done.ptr());
        ctx->Flush();
        BOOL complete = FALSE;
        uint64_t deadline = milliseconds() + 5000;
        while (!complete) {
            HRESULT hr = ctx->GetData(done.ptr(), &complete, sizeof(complete), 0);
            CK(hr, "GetData");
            EXPECT(milliseconds() < deadline, "GPU completion timeout");
            if (!complete) usleep(1000);
        }
        EXPECT(write(sock, &tick, sizeof(tick)) == sizeof(tick), "send tick failed");
        EXPECT(read(sock, &ack, 1) == 1 && ack == 'A', "peer data check failed");
    }
    printf("STEAMVR400: producer pid=%d sent %u updates\n", getpid(), kTicks);
    return 0;
}

int consumer(int sock) {
    dmn_shared_buffer_handle wire{};
    int fd = -1;
    HANDLE handle = &wire;
    if (wine_handles()) {
        uint32_t token = 0;
        EXPECT(read(sock, &token, sizeof(token)) == sizeof(token), "receive token failed");
        handle = reinterpret_cast<HANDLE>(uintptr_t(token));
    } else {
        EXPECT(recv_with_fd(sock, &wire, sizeof(wire), &fd), "receive fd failed");
        wire.fd = fd;
    }
    Com<ID3D11Device> dev;
    Com<ID3D11DeviceContext> ctx;
    EXPECT(device(dev, ctx) == 0, "consumer device failed");
    Com<ID3D11Buffer> imported;
    if (wine_handles()) {
        Com<ID3D11Buffer> absent;
        HRESULT hr = dev->OpenSharedResource((HANDLE)uintptr_t(0x6ffffffd),
                                             __uuidof(ID3D11Buffer), (void**)&absent);
        EXPECT(FAILED(hr) && !absent, "unknown token was accepted");
    }
    CK_OK(dev->OpenSharedResource(handle, __uuidof(ID3D11Buffer),
                                 (void**)&imported), "OpenSharedResource");
    if (fd >= 0) close(fd); // The import retains its own backing reference.
    D3D11_BUFFER_DESC shape{};
    imported->GetDesc(&shape);
    EXPECT(shape.ByteWidth == sizeof(Frame), "import changed logical buffer size");
    D3D11_BUFFER_DESC staging_desc{};
    staging_desc.ByteWidth = sizeof(Frame);
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Com<ID3D11Buffer> staging;
    CK(dev->CreateBuffer(&staging_desc, nullptr, &staging), "Create staging");
    char ack = 'R';
    EXPECT(write(sock, &ack, 1) == 1, "send ready failed");
    for (unsigned wanted = 1; wanted <= kTicks; ++wanted) {
        unsigned tick = 0;
        EXPECT(read(sock, &tick, sizeof(tick)) == sizeof(tick) && tick == wanted,
               "receive tick failed");
        ctx->CopyResource(staging.ptr(), imported.ptr());
        D3D11_MAPPED_SUBRESOURCE map{};
        CK(ctx->Map(staging.ptr(), 0, D3D11_MAP_READ, 0, &map), "Map staging");
        Frame expected = frame(tick);
        bool equal = memcmp(map.pData, expected.data(), sizeof(expected)) == 0;
        ctx->Unmap(staging.ptr(), 0);
        EXPECT(equal, "peer GPU readback differs from producer data");
        ack = 'A';
        EXPECT(write(sock, &ack, 1) == 1, "send acknowledgement failed");
    }
    printf("STEAMVR400: consumer pid=%d verified all 400 bytes in %u updates\n",
           getpid(), kTicks);
    return 0;
}
}

int main(int argc, char** argv) {
    if (argc == 3 && strcmp(argv[1], "--consumer") == 0)
        return consumer(atoi(argv[2]));
    if (wine_handles() && !getenv("DMN_WINE_SOCKET_DIR")) {
        char dir[] = "/private/tmp/mvr-test-XXXXXX";
        EXPECT(mkdtemp(dir) != nullptr, "temporary directory failed");
        setenv("DMN_WINE_SOCKET_DIR", dir, 1);
        std::atexit([] { rmdir(getenv("DMN_WINE_SOCKET_DIR")); });
    }
    int rc = run_exec_pair(T_TAG, argv[0], "--consumer", producer);
    if (!rc) T_PASS();
    return rc;
}
