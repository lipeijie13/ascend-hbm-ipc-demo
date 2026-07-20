#ifndef ASCEND_HBM_IPC_DEMO_IPC_COMMON_H_
#define ASCEND_HBM_IPC_DEMO_IPC_COMMON_H_

#include <acl/acl_rt.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace ascend_hbm_ipc_demo {

constexpr uint32_t kProtocolMagic = 0x41495043U;  // "AIPC"
constexpr uint32_t kProtocolVersion = 3;
constexpr uint32_t kWireFlagXdsRead = 1U << 0;
constexpr size_t kIpcKeyLength = 65;
constexpr size_t kHugePageSize = 2UL * 1024UL * 1024UL;
constexpr size_t kDefaultBufferSize = kHugePageSize;
constexpr size_t kMutationSize = 64;
constexpr uint8_t kMutationValue = 0xA5;
constexpr uint8_t kXdsPoisonValue = 0x3C;
constexpr int kSocketTimeoutSeconds = 30;

enum class MessageType : uint32_t {
    kHello = 1,
    kExportBuffer = 2,
    kImported = 3,
    kModified = 4,
    kReleaseBuffer = 5,
    kImportClosed = 6,
};

// This is a same-host, same-binary demo wire format. A production protocol
// should use protobuf or another versioned serializer instead of sending a C++
// struct directly. No process-local pointer is sent across the socket.
struct WireMessage {
    uint32_t magic;
    uint32_t version;
    uint32_t type;
    uint32_t flags;
    int32_t deviceId;
    int32_t barePid;
    int32_t processPid;
    uint64_t bufferId;
    uint64_t generation;
    uint64_t size;
    std::array<char, kIpcKeyLength> key;
};

static_assert(std::is_trivially_copyable_v<WireMessage>);

inline WireMessage MakeMessage(MessageType type)
{
    WireMessage message;
    // The demo sends the whole fixed-size object over a local socket. Clear
    // tail padding as well as fields so no stack bytes are exposed.
    std::memset(&message, 0, sizeof(message));
    message.magic = kProtocolMagic;
    message.version = kProtocolVersion;
    message.type = static_cast<uint32_t>(type);
    message.deviceId = -1;
    message.barePid = -1;
    message.processPid = -1;
    return message;
}

inline std::string AclErrorMessage(const char *operation, aclError error)
{
    std::string message = std::string(operation) + " failed, aclError=" + std::to_string(error);
    const char *recent = aclGetRecentErrMsg();
    if (recent != nullptr && recent[0] != '\0') {
        message += ", recent=";
        message += recent;
    }
    return message;
}

inline void CheckAcl(aclError error, const char *operation)
{
    if (error != ACL_SUCCESS) {
        throw std::runtime_error(AclErrorMessage(operation, error));
    }
}

inline void CheckSyscall(int result, const char *operation)
{
    if (result < 0) {
        throw std::runtime_error(std::string(operation) + " failed: " + std::strerror(errno));
    }
}

class UniqueFd {
public:
    explicit UniqueFd(int fd = -1) : fd_(fd)
    {
    }

    ~UniqueFd()
    {
        Reset();
    }

    UniqueFd(const UniqueFd &) = delete;
    UniqueFd &operator=(const UniqueFd &) = delete;

    UniqueFd(UniqueFd &&other) noexcept : fd_(other.fd_)
    {
        other.fd_ = -1;
    }

    UniqueFd &operator=(UniqueFd &&other) noexcept
    {
        if (this != &other) {
            Reset();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    int Get() const
    {
        return fd_;
    }

    void Reset(int fd = -1)
    {
        if (fd_ >= 0) {
            (void)::close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_;
};

class SocketPathGuard {
public:
    explicit SocketPathGuard(std::string path) : path_(std::move(path))
    {
        (void)::unlink(path_.c_str());
    }

    ~SocketPathGuard()
    {
        (void)::unlink(path_.c_str());
    }

    SocketPathGuard(const SocketPathGuard &) = delete;
    SocketPathGuard &operator=(const SocketPathGuard &) = delete;

private:
    std::string path_;
};

inline sockaddr_un MakeUnixAddress(const std::string &path)
{
    if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path)) {
        throw std::runtime_error("Unix socket path is empty or too long");
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    return address;
}

inline void SetSocketTimeouts(int fd)
{
    timeval timeout{};
    timeout.tv_sec = kSocketTimeoutSeconds;
    CheckSyscall(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)),
                 "setsockopt(SO_RCVTIMEO)");
    CheckSyscall(::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)),
                 "setsockopt(SO_SNDTIMEO)");
}

inline void VerifyPeerUid(int fd)
{
    ucred credentials{};
    socklen_t length = sizeof(credentials);
    CheckSyscall(::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length),
                 "getsockopt(SO_PEERCRED)");
    if (length != sizeof(credentials) || credentials.uid != ::geteuid()) {
        throw std::runtime_error("control socket peer UID is not trusted");
    }
}

inline UniqueFd ListenUnixSeqpacket(const std::string &path)
{
    UniqueFd fd(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
    CheckSyscall(fd.Get(), "socket");
    sockaddr_un address = MakeUnixAddress(path);
    CheckSyscall(::bind(fd.Get(), reinterpret_cast<const sockaddr *>(&address), sizeof(address)), "bind");
    CheckSyscall(::chmod(path.c_str(), S_IRUSR | S_IWUSR), "chmod(socket)");
    CheckSyscall(::listen(fd.Get(), 1), "listen");
    return fd;
}

inline UniqueFd AcceptUnixSeqpacket(int listenFd)
{
    int fd;
    do {
        fd = ::accept4(listenFd, nullptr, nullptr, SOCK_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    CheckSyscall(fd, "accept4");
    UniqueFd accepted(fd);
    SetSocketTimeouts(accepted.Get());
    VerifyPeerUid(accepted.Get());
    return accepted;
}

inline UniqueFd ConnectUnixSeqpacket(const std::string &path)
{
    sockaddr_un address = MakeUnixAddress(path);
    for (int attempt = 0; attempt < 200; ++attempt) {
        UniqueFd fd(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
        CheckSyscall(fd.Get(), "socket");
        if (::connect(fd.Get(), reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0) {
            SetSocketTimeouts(fd.Get());
            VerifyPeerUid(fd.Get());
            return fd;
        }
        if (errno != ENOENT && errno != ECONNREFUSED) {
            CheckSyscall(-1, "connect");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    throw std::runtime_error("connect timed out waiting for Worker");
}

inline void SendMessage(int fd, const WireMessage &message)
{
    ssize_t sent;
    do {
        sent = ::send(fd, &message, sizeof(message), MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    if (sent < 0) {
        CheckSyscall(-1, "send");
    }
    if (sent != static_cast<ssize_t>(sizeof(message))) {
        throw std::runtime_error("short SOCK_SEQPACKET send");
    }
}

inline WireMessage ReceiveMessage(int fd, MessageType expected)
{
    WireMessage message{};
    ssize_t received;
    do {
        received = ::recv(fd, &message, sizeof(message), 0);
    } while (received < 0 && errno == EINTR);
    if (received < 0) {
        CheckSyscall(-1, "recv");
    }
    if (received == 0) {
        throw std::runtime_error("peer closed the control socket");
    }
    if (received != static_cast<ssize_t>(sizeof(message))) {
        throw std::runtime_error("invalid control message length");
    }
    if (message.magic != kProtocolMagic || message.version != kProtocolVersion) {
        throw std::runtime_error("invalid control protocol magic/version");
    }
    if (message.type != static_cast<uint32_t>(expected)) {
        throw std::runtime_error("unexpected control message type");
    }
    return message;
}

inline void ValidateBufferIdentity(const WireMessage &message, uint64_t bufferId, uint64_t generation)
{
    if (message.bufferId != bufferId || message.generation != generation) {
        throw std::runtime_error("buffer id/generation mismatch");
    }
}

class AclSession {
public:
    explicit AclSession(int32_t deviceId) : deviceId_(deviceId)
    {
        CheckAcl(aclInit(nullptr), "aclInit");
        initialized_ = true;
        try {
            CheckAcl(aclrtSetDevice(deviceId_), "aclrtSetDevice");
            deviceSet_ = true;
        } catch (...) {
            (void)aclFinalize();
            initialized_ = false;
            throw;
        }
    }

    ~AclSession()
    {
        if (deviceSet_) {
            const aclError error = aclrtResetDevice(deviceId_);
            if (error != ACL_SUCCESS) {
                std::cerr << AclErrorMessage("aclrtResetDevice", error) << std::endl;
            }
        }
        if (initialized_) {
            const aclError error = aclFinalize();
            if (error != ACL_SUCCESS) {
                std::cerr << AclErrorMessage("aclFinalize", error) << std::endl;
            }
        }
    }

    AclSession(const AclSession &) = delete;
    AclSession &operator=(const AclSession &) = delete;

    int32_t BarePid() const
    {
        int32_t pid = -1;
        CheckAcl(aclrtDeviceGetBareTgid(&pid), "aclrtDeviceGetBareTgid");
        return pid;
    }

private:
    int32_t deviceId_;
    bool initialized_ = false;
    bool deviceSet_ = false;
};

class OwnedDeviceBuffer {
public:
    explicit OwnedDeviceBuffer(size_t size) : size_(size)
    {
        CheckAcl(aclrtMalloc(&ptr_, size_, ACL_MEM_MALLOC_HUGE_ONLY), "aclrtMalloc");
    }

    ~OwnedDeviceBuffer()
    {
        FreeNoThrow();
    }

    OwnedDeviceBuffer(const OwnedDeviceBuffer &) = delete;
    OwnedDeviceBuffer &operator=(const OwnedDeviceBuffer &) = delete;

    void *Get() const
    {
        return ptr_;
    }

    size_t Size() const
    {
        return size_;
    }

    void Free()
    {
        if (ptr_ != nullptr) {
            CheckAcl(aclrtFree(ptr_), "aclrtFree(owner)");
            ptr_ = nullptr;
            size_ = 0;
        }
    }

    // Used only when the control channel fails after the Key was published.
    // Avoid an explicit exporter-first Free; process teardown reclaims it.
    void AbandonToProcessExit()
    {
        ptr_ = nullptr;
        size_ = 0;
    }

private:
    void FreeNoThrow()
    {
        if (ptr_ != nullptr) {
            const aclError error = aclrtFree(ptr_);
            if (error != ACL_SUCCESS) {
                std::cerr << AclErrorMessage("aclrtFree(owner)", error) << std::endl;
            }
            ptr_ = nullptr;
            size_ = 0;
        }
    }

    void *ptr_ = nullptr;
    size_t size_ = 0;
};

class ExportedIpcMemory {
public:
    ExportedIpcMemory(void *devicePtr, size_t size, int32_t workerBarePid)
    {
        // Keep the default PID whitelist behavior. The Worker Bare TGID is
        // explicitly authorized below before the Key leaves this process.
        CheckAcl(aclrtIpcMemGetExportKey(devicePtr, size, key_.data(), key_.size(),
                                         ACL_RT_IPC_MEM_EXPORT_FLAG_DEFAULT),
                 "aclrtIpcMemGetExportKey");
        open_ = true;
        try {
            // 【可选】设置worker进程到白名单，不调用时默认不启动白名单校验。
            CheckAcl(aclrtIpcMemSetImportPid(key_.data(), &workerBarePid, 1),
                     "aclrtIpcMemSetImportPid");
        } catch (...) {
            (void)aclrtIpcMemClose(key_.data());
            open_ = false;
            throw;
        }
    }

    ~ExportedIpcMemory()
    {
        CloseNoThrow();
    }

    ExportedIpcMemory(const ExportedIpcMemory &) = delete;
    ExportedIpcMemory &operator=(const ExportedIpcMemory &) = delete;

    const std::array<char, kIpcKeyLength> &Key() const
    {
        return key_;
    }

    void Close()
    {
        if (open_) {
            CheckAcl(aclrtIpcMemClose(key_.data()), "aclrtIpcMemClose(client/exporter)");
            open_ = false;
        }
    }

    void AbandonToProcessExit()
    {
        open_ = false;
    }

private:
    void CloseNoThrow()
    {
        if (open_) {
            const aclError error = aclrtIpcMemClose(key_.data());
            if (error != ACL_SUCCESS) {
                std::cerr << AclErrorMessage("aclrtIpcMemClose(client/exporter)", error) << std::endl;
            }
            open_ = false;
        }
    }

    std::array<char, kIpcKeyLength> key_{};
    bool open_ = false;
};

class ImportedIpcMemory {
public:
    explicit ImportedIpcMemory(const std::array<char, kIpcKeyLength> &key) : key_(key)
    {
        CheckAcl(aclrtIpcMemImportByKey(&ptr_, key_.data(), ACL_RT_IPC_MEM_IMPORT_FLAG_DEFAULT),
                 "aclrtIpcMemImportByKey");
        open_ = true;
    }

    ~ImportedIpcMemory()
    {
        CloseNoThrow();
    }

    ImportedIpcMemory(const ImportedIpcMemory &) = delete;
    ImportedIpcMemory &operator=(const ImportedIpcMemory &) = delete;

    void *Get() const
    {
        return ptr_;
    }

    void Close()
    {
        if (open_) {
            CheckAcl(aclrtIpcMemClose(key_.data()), "aclrtIpcMemClose(worker/importer)");
            open_ = false;
            ptr_ = nullptr;
        }
    }

private:
    void CloseNoThrow()
    {
        if (open_) {
            const aclError error = aclrtIpcMemClose(key_.data());
            if (error != ACL_SUCCESS) {
                std::cerr << AclErrorMessage("aclrtIpcMemClose(worker/importer)", error) << std::endl;
            }
            open_ = false;
            ptr_ = nullptr;
        }
    }

    std::array<char, kIpcKeyLength> key_{};
    void *ptr_ = nullptr;
    bool open_ = false;
};

inline uint8_t PatternByte(size_t index)
{
    return static_cast<uint8_t>((index * 131U + 17U) % 251U);
}

inline void PrintHexPreview(const std::string &label, const uint8_t *data, size_t size,
                            size_t maxBytes = kMutationSize)
{
    const size_t bytes = std::min(size, maxBytes);
    std::ostringstream output;
    output << label << "（打印前 " << bytes << "/" << size << " 字节）";
    for (size_t i = 0; i < bytes; ++i) {
        if (i % 16 == 0) {
            output << "\n  [" << std::setw(4) << std::setfill('0') << i << "] ";
        }
        output << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(data[i]) << ' ';
    }
    std::cout << output.str() << std::dec << std::endl;
}

inline int32_t ParseDeviceId(const char *value)
{
    size_t consumed = 0;
    const int result = std::stoi(value, &consumed);
    if (value[consumed] != '\0' || result < 0) {
        throw std::runtime_error("invalid device id");
    }
    return result;
}

inline size_t ParseBufferSize(const char *value)
{
    size_t consumed = 0;
    const unsigned long long result = std::stoull(value, &consumed);
    if (value[consumed] != '\0' || result < kMutationSize || result % kHugePageSize != 0) {
        throw std::runtime_error("buffer size must be a positive multiple of 2 MiB");
    }
    return static_cast<size_t>(result);
}

inline uint64_t ParseUint64(const char *value, const char *name)
{
    const std::string text(value == nullptr ? "" : value);
    if (text.empty()
        || std::any_of(text.begin(), text.end(), [](char character) { return character < '0' || character > '9'; })) {
        throw std::runtime_error(std::string("invalid ") + name);
    }
    size_t consumed = 0;
    const unsigned long long result = std::stoull(text, &consumed);
    if (consumed != text.size()) {
        throw std::runtime_error(std::string("invalid ") + name);
    }
    return static_cast<uint64_t>(result);
}

}  // namespace ascend_hbm_ipc_demo

#endif  // ASCEND_HBM_IPC_DEMO_IPC_COMMON_H_
