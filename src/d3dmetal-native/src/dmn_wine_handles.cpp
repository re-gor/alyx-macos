#include "dmn_wine_handles.h"
#include "dmn_log.h"
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>

namespace {
constexpr int32_t kFail = int32_t(0x80004005), kInvalid = int32_t(0x80070057);
constexpr size_t kCapacity = 128;
constexpr uint32_t kTag = 0x60000000, kMask = 0xf0000000;
struct Owner { uint32_t version; int32_t pid; uint64_t cookie; };
struct Packet { int32_t hr; uint32_t size; alignas(8) char pod[kCapacity]; };
struct Entry {
    void* identity;
    Packet packet;
    int fd;
};

std::string directory() {
    const char* path = getenv("DMN_WINE_SOCKET_DIR");
    if (!path || !*path) return {};
    struct stat st{};
    if (lstat(path, &st) || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 077)) return {};
    return path;
}
std::string token_path(const std::string& dir, uint32_t token) {
    char leaf[32];
    snprintf(leaf, sizeof(leaf), "/%08x.ref", token);
    return dir + leaf;
}
std::string owner_path(const std::string& dir, const Owner& owner) {
    char leaf[64];
    snprintf(leaf, sizeof(leaf), "/p%d-%016llx.sock", owner.pid,
             (unsigned long long)owner.cookie);
    return dir + leaf;
}
bool address(const std::string& path, sockaddr_un& out) {
    if (path.size() >= sizeof(out.sun_path)) return false;
    out.sun_family = AF_UNIX;
    out.sun_len = sizeof(out);
    memcpy(out.sun_path, path.c_str(), path.size() + 1);
    return true;
}
void socket_options(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags >= 0) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
    timeval timeout{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}
bool read_exact(int fd, void* dst, size_t size) {
    char* p = static_cast<char*>(dst);
    while (size) {
        ssize_t n = read(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n; size -= size_t(n);
    }
    return true;
}
bool write_exact(int fd, const void* src, size_t size) {
    const char* p = static_cast<const char*>(src);
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n; size -= size_t(n);
    }
    return true;
}

class Broker {
public:
    std::mutex mutex;
    std::unordered_map<uint32_t, Entry> entries;
    std::unordered_map<void*, uint32_t> identities;
    std::string dir, path;
    Owner owner{};
    int listener = -1;
    std::thread worker;
    std::atomic<bool> stopped{false};

    bool start() {
        dir = directory();
        if (dir.empty()) return false;
        owner = {1, getpid(), (uint64_t(arc4random()) << 32) | arc4random()};
        path = owner_path(dir, owner);
        sockaddr_un addr{};
        if (!address(path, addr)) return false;
        listener = socket(AF_UNIX, SOCK_STREAM, 0);
        if (listener < 0) return false;
        fcntl(listener, F_SETFD, FD_CLOEXEC);
        if (bind(listener, (sockaddr*)&addr, sizeof(addr)) || listen(listener, 16)) {
            close(listener); listener = -1; unlink(path.c_str()); return false;
        }
        fcntl(listener, F_SETFL, O_NONBLOCK);
        worker = std::thread([this] { serve(); });
        return true;
    }
    void serve() {
        while (!stopped) {
            pollfd ready{listener, POLLIN, 0};
            int pending = poll(&ready, 1, 100);
            if (pending == 0) continue;
            if (pending < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (stopped) break;
            int client = accept(listener, nullptr, nullptr);
            if (client < 0) {
                if (stopped) break;
                if (errno == EINTR || errno == EAGAIN) continue;
                break;
            }
            socket_options(client);
            uint32_t token = 0;
            Packet packet{kInvalid, 0, {}};
            int exported = -1;
            if (read_exact(client, &token, sizeof(token))) {
                std::lock_guard<std::mutex> lock(mutex);
                auto it = entries.find(token);
                if (it != entries.end()) {
                    packet = it->second.packet;
                    exported = fcntl(it->second.fd, F_DUPFD_CLOEXEC, 0);
                    if (exported < 0) packet = {kFail, 0, {}};
                }
            }
            iovec io{&packet, sizeof(packet)};
            alignas(cmsghdr) char ancillary[CMSG_SPACE(sizeof(int))]{};
            msghdr msg{};
            msg.msg_iov = &io; msg.msg_iovlen = 1;
            if (exported >= 0) {
                msg.msg_control = ancillary;
                msg.msg_controllen = sizeof(ancillary);
                cmsghdr* cm = CMSG_FIRSTHDR(&msg);
                cm->cmsg_level = SOL_SOCKET; cm->cmsg_type = SCM_RIGHTS;
                cm->cmsg_len = CMSG_LEN(sizeof(int));
                memcpy(CMSG_DATA(cm), &exported, sizeof(exported));
            }
            ssize_t n = sendmsg(client, &msg, 0);
            if (n > 0 && size_t(n) < sizeof(packet))
                write_exact(client, reinterpret_cast<char*>(&packet) + n,
                            sizeof(packet) - size_t(n));
            if (exported >= 0) close(exported);
            close(client);
        }
    }
    void stop() {
        stopped = true;
        if (worker.joinable()) worker.join();
        if (listener >= 0) close(listener);
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& entry : entries) {
            close(entry.second.fd);
            unlink(token_path(dir, entry.first).c_str());
        }
        entries.clear(); identities.clear();
        unlink(path.c_str());
    }
};
std::atomic<Broker*> broker{nullptr};
std::once_flag broker_once;
void stop_broker() { if (auto* b = broker.load()) b->stop(); }
Broker* get_broker() {
    std::call_once(broker_once, [] {
        auto* candidate = new Broker;
        if (!candidate->start()) { delete candidate; return; }
        broker.store(candidate);
        std::atexit(stop_broker);
    });
    return broker.load();
}
}

bool dmn_wine_handles_enabled() {
    const char* enabled = getenv("DMN_WINE_SHARING");
    return enabled && strcmp(enabled, "1") == 0;
}
bool dmn_wine_is_handle(void* handle) {
    uintptr_t value = reinterpret_cast<uintptr_t>(handle);
    return dmn_wine_handles_enabled() && value <= UINT32_MAX &&
           (uint32_t(value) & kMask) == kTag;
}
int32_t dmn_wine_export_handle(void* identity, const void* pod, size_t size,
                               void** handle) {
    if (!pod || !handle || size < 12 || size > kCapacity) return kInvalid;
    Broker* b = get_broker();
    if (!b) return kFail;
    std::lock_guard<std::mutex> lock(b->mutex);
    auto previous = b->identities.find(identity);
    if (previous != b->identities.end()) {
        *handle = reinterpret_cast<void*>(uintptr_t(previous->second));
        return 0;
    }
    int fd = -1;
    memcpy(&fd, static_cast<const char*>(pod) + 8, sizeof(fd));
    int owned = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (owned < 0) return kFail;
    uint32_t token = 0;
    int record = -1;
    for (int attempt = 0; attempt < 64; ++attempt) {
        token = kTag | (arc4random() & 0x0ffffffcu);
        record = open(token_path(b->dir, token).c_str(),
                      O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (record >= 0) break;
        if (errno != EEXIST) { close(owned); return kFail; }
    }
    if (record < 0) { close(owned); return kFail; }
    bool ok = write_exact(record, &b->owner, sizeof(b->owner));
    close(record);
    if (!ok) {
        unlink(token_path(b->dir, token).c_str()); close(owned); return kFail;
    }
    Entry entry{identity, {0, uint32_t(size), {}}, owned};
    memcpy(entry.packet.pod, pod, size);
    b->entries.emplace(token, entry);
    b->identities.emplace(identity, token);
    *handle = reinterpret_cast<void*>(uintptr_t(token));
    DMN_INFO("wine: exported legacy token 0x%08x pid=%d", token, getpid());
    return 0;
}
int32_t dmn_wine_import_handle(void* handle, void* pod, size_t capacity,
                               size_t* size, int* fd) {
    if (!dmn_wine_is_handle(handle) || !pod || !size || !fd) return kInvalid;
    *fd = -1; *size = 0;
    std::string dir = directory();
    if (dir.empty()) return kFail;
    uint32_t token = uint32_t(reinterpret_cast<uintptr_t>(handle));
    int record = open(token_path(dir, token).c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (record < 0) return kInvalid;
    Owner owner{};
    struct stat st{};
    bool ok = fstat(record, &st) == 0 && S_ISREG(st.st_mode) &&
              st.st_uid == geteuid() && st.st_size == sizeof(owner) &&
              read_exact(record, &owner, sizeof(owner));
    close(record);
    if (!ok || owner.version != 1 || owner.pid <= 0) return kInvalid;
    sockaddr_un addr{};
    if (!address(owner_path(dir, owner), addr)) return kFail;
    int client = socket(AF_UNIX, SOCK_STREAM, 0);
    if (client < 0) return kFail;
    socket_options(client);
    if (connect(client, (sockaddr*)&addr, sizeof(addr)) ||
        !write_exact(client, &token, sizeof(token))) { close(client); return kInvalid; }
    Packet packet{};
    iovec io{&packet, sizeof(packet)};
    alignas(cmsghdr) char ancillary[CMSG_SPACE(sizeof(int))]{};
    msghdr msg{};
    msg.msg_iov = &io; msg.msg_iovlen = 1;
    msg.msg_control = ancillary; msg.msg_controllen = sizeof(ancillary);
    ssize_t got = recvmsg(client, &msg, MSG_WAITALL);
    close(client);
    int received = -1;
    for (cmsghdr* cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm))
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS &&
            cm->cmsg_len == CMSG_LEN(sizeof(int)))
            memcpy(&received, CMSG_DATA(cm), sizeof(received));
    if (got != sizeof(packet) || (msg.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) ||
        packet.hr < 0 || packet.size < 12 || packet.size > capacity ||
        packet.size > kCapacity || received < 0) {
        if (received >= 0) close(received);
        return got == sizeof(packet) && packet.hr < 0 ? packet.hr : kFail;
    }
    fcntl(received, F_SETFD, FD_CLOEXEC);
    memcpy(pod, packet.pod, packet.size);
    memcpy(static_cast<char*>(pod) + 8, &received, sizeof(received));
    *size = packet.size; *fd = received;
    DMN_INFO("wine: imported legacy token 0x%08x pid=%d fd=%d", token, getpid(), received);
    return 0;
}
void dmn_wine_evict_handle(void* identity) {
    Broker* b = broker.load();
    if (!b) return;
    std::lock_guard<std::mutex> lock(b->mutex);
    auto it = b->identities.find(identity);
    if (it == b->identities.end()) return;
    uint32_t token = it->second;
    auto entry = b->entries.find(token);
    if (entry != b->entries.end()) {
        close(entry->second.fd);
        b->entries.erase(entry);
    }
    unlink(token_path(b->dir, token).c_str());
    b->identities.erase(it);
}
