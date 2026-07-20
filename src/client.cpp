#include "ipc_common.h"

#include <fcntl.h>

#include <limits>
#include <vector>

using namespace ascend_hbm_ipc_demo;

namespace {
constexpr uint64_t kBufferId = 1;
constexpr uint64_t kGeneration = 1;

struct ClientOptions {
    std::string socketPath = "/tmp/ascend-hbm-ipc-demo.sock";
    int32_t deviceId = 0;
    size_t bufferSize = kDefaultBufferSize;
    bool xdsMode = false;
    std::string sourceFile;
    uint64_t fileOffset = 0;
};

ClientOptions ParseOptions(int argc, char **argv)
{
    ClientOptions options;
    if (argc > 1) {
        options.socketPath = argv[1];
    }
    if (argc > 2) {
        options.deviceId = ParseDeviceId(argv[2]);
    }
    if (argc > 3) {
        options.bufferSize = ParseBufferSize(argv[3]);
    }
    if (argc > 4) {
        if (argc != 7 || std::string(argv[4]) != "--xds") {
            throw std::runtime_error(
                "usage: client [socket] [device] [buffer-size] [--xds source-file file-offset]");
        }
        options.xdsMode = true;
        options.sourceFile = argv[5];
        options.fileOffset = ParseUint64(argv[6], "XDS file offset");
    }
    return options;
}

std::vector<uint8_t> ReadFileRange(const std::string &path, uint64_t offset, size_t size)
{
    if (offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max())
        || size > static_cast<size_t>(std::numeric_limits<off_t>::max())
        || offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) - size) {
        throw std::runtime_error("verification file range is outside off_t");
    }

    UniqueFd file(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    CheckSyscall(file.Get(), "open(XDS verification file)");
    std::vector<uint8_t> data(size);
    size_t completed = 0;
    while (completed < data.size()) {
        ssize_t received;
        do {
            received = ::pread(file.Get(), data.data() + completed, data.size() - completed,
                               static_cast<off_t>(offset + completed));
        } while (received < 0 && errno == EINTR);
        if (received < 0) {
            CheckSyscall(-1, "pread(XDS verification file)");
        }
        if (received == 0) {
            throw std::runtime_error("XDS verification file is shorter than the requested range");
        }
        completed += static_cast<size_t>(received);
    }
    return data;
}
}

int main(int argc, char **argv)
{
    try {
        const ClientOptions options = ParseOptions(argc, argv);
        AclSession acl(options.deviceId);
        UniqueFd control = ConnectUnixSeqpacket(options.socketPath);
        const WireMessage hello = ReceiveMessage(control.Get(), MessageType::kHello);
        if (hello.deviceId != options.deviceId) {
            throw std::runtime_error("Worker HELLO has an invalid or different Device ID");
        }

        OwnedDeviceBuffer owner(options.bufferSize);
        std::vector<uint8_t> host(options.bufferSize);
        if (options.xdsMode) {
            std::fill(host.begin(), host.end(), kXdsPoisonValue);
            PrintHexPreview("[Client] XDS 前的 HBM poison pattern", host.data(), host.size());
        } else {
            for (size_t i = 0; i < host.size(); ++i) {
                host[i] = PatternByte(i);
            }
            PrintHexPreview("[Client] 初始 pattern", host.data(), host.size());
        }
        CheckAcl(aclrtMemcpy(owner.Get(), owner.Size(), host.data(), host.size(), ACL_MEMCPY_HOST_TO_DEVICE),
                 "aclrtMemcpy(H2D initial pattern)");

        std::cout << "[Client] owner HBM allocated: device=" << options.deviceId << ", owner_ptr=" << owner.Get()
                  << ", size=" << owner.Size() << std::endl;

        // Destruction order is exported -> owner -> ACL session. On the normal
        // path we still close explicitly to make the required ordering clear.
        ExportedIpcMemory exported(owner.Get(), owner.Size());
        bool published = false;
        bool workerClosed = false;
        bool exporterClosed = false;
        try {
            WireMessage descriptor = MakeMessage(MessageType::kExportBuffer);
            descriptor.flags = options.xdsMode ? kWireFlagXdsRead : 0;
            descriptor.deviceId = options.deviceId;
            descriptor.ownerPid = static_cast<int32_t>(::getpid());
            descriptor.ownerBase = reinterpret_cast<uintptr_t>(owner.Get());
            descriptor.bufferId = kBufferId;
            descriptor.generation = kGeneration;
            descriptor.size = owner.Size();
            descriptor.key = exported.Key();
            SendMessage(control.Get(), descriptor);
            published = true;
            std::cout << "[Client] 65-byte IPC Key exported without PID whitelist: owner_pid="
                      << descriptor.ownerPid << ", owner_ptr=" << owner.Get() << std::endl;

            WireMessage imported = ReceiveMessage(control.Get(), MessageType::kImported);
            ValidateBufferIdentity(imported, kBufferId, kGeneration);
            std::cout << "[Client] Worker imported the HBM mapping" << std::endl;

            WireMessage modified = ReceiveMessage(control.Get(), MessageType::kModified);
            ValidateBufferIdentity(modified, kBufferId, kGeneration);

            CheckAcl(aclrtMemcpy(host.data(), host.size(), owner.Get(), owner.Size(), ACL_MEMCPY_DEVICE_TO_HOST),
                     "aclrtMemcpy(D2H owner verification)");
            if (options.xdsMode) {
                const std::vector<uint8_t> expected =
                    ReadFileRange(options.sourceFile, options.fileOffset, options.bufferSize);
                if (std::all_of(expected.begin(), expected.end(),
                                [](uint8_t value) { return value == kXdsPoisonValue; })) {
                    throw std::runtime_error(
                        "XDS source range is identical to the initial HBM poison; the result would be inconclusive");
                }
                PrintHexPreview("[Client] XDS 后从 owner_ptr 读回的数据", host.data(), host.size());
                for (size_t i = 0; i < host.size(); ++i) {
                    if (host[i] != expected[i]) {
                        throw std::runtime_error("XDS verification failed at byte " + std::to_string(i));
                    }
                }
                std::cout << "[Client] owner HBM matches the SSD source range after Worker XDS read" << std::endl;
            } else {
                PrintHexPreview("[Client] 从 owner_ptr 读回 Worker 修改后的数据", host.data(), host.size());
                for (size_t i = 0; i < host.size(); ++i) {
                    const uint8_t expected = i < kMutationSize ? kMutationValue : PatternByte(i);
                    if (host[i] != expected) {
                        throw std::runtime_error("Client verification failed at byte " + std::to_string(i));
                    }
                }
                std::cout << "[Client] observed Worker's in-place HBM mutation through owner_ptr" << std::endl;
            }

            WireMessage release = MakeMessage(MessageType::kReleaseBuffer);
            release.deviceId = options.deviceId;
            release.bufferId = kBufferId;
            release.generation = kGeneration;
            SendMessage(control.Get(), release);

            WireMessage closed = ReceiveMessage(control.Get(), MessageType::kImportClosed);
            ValidateBufferIdentity(closed, kBufferId, kGeneration);
            workerClosed = true;

            // Mandatory order: Worker/importer Close -> ACK -> Client/exporter
            // Close -> owner aclrtFree.
            exported.Close();
            exporterClosed = true;
            owner.Free();
        } catch (...) {
            if (published && (!workerClosed || !exporterClosed)) {
                // The importer-close order is unknown. Do not perform an
                // explicit exporter-first Close/Free, or Free after exporter
                // Close failed. Terminate and let Runtime teardown reclaim
                // owner resources.
                exported.AbandonToProcessExit();
                owner.AbandonToProcessExit();
            }
            throw;
        }

        if (options.xdsMode) {
            std::cout << "PASS: XDS wrote the SSD file range through Worker's imported HBM VA; "
                         "Client owner VA observed identical bytes; importer Close -> exporter Close -> owner Free"
                      << std::endl;
        } else {
            std::cout << "PASS: two process-local Device VAs accessed the same physical HBM; "
                         "Worker Close -> Client Close -> owner Free completed"
                      << std::endl;
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "[Client] failed: " << error.what() << std::endl;
        return 1;
    }
}
