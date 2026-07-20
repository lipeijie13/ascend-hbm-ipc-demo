#include "ipc_common.h"

#ifdef ASCEND_HBM_IPC_DEMO_ENABLE_XDS
#include "xds_reader.h"
#endif

#include <limits>
#include <vector>

using namespace ascend_hbm_ipc_demo;

namespace {

struct WorkerOptions {
    std::string socketPath = "/tmp/ascend-hbm-ipc-demo.sock";
    int32_t deviceId = 0;
    bool xdsMode = false;
    std::string sourceFile;
    std::string blockDevice;
    uint32_t vfId = 0;
    uint64_t fileOffset = 0;
};

WorkerOptions ParseOptions(int argc, char **argv)
{
    WorkerOptions options;
    if (argc > 1) {
        options.socketPath = argv[1];
    }
    if (argc > 2) {
        options.deviceId = ParseDeviceId(argv[2]);
    }
    if (argc > 3) {
        if (argc != 8 || std::string(argv[3]) != "--xds") {
            throw std::runtime_error(
                "usage: worker [socket] [device] [--xds source-file block-device vf-id file-offset]");
        }
        options.xdsMode = true;
        options.sourceFile = argv[4];
        options.blockDevice = argv[5];
        const uint64_t vfId = ParseUint64(argv[6], "XDS VF ID");
        if (vfId > std::numeric_limits<uint32_t>::max()) {
            throw std::runtime_error("XDS VF ID is outside uint32_t");
        }
        options.vfId = static_cast<uint32_t>(vfId);
        options.fileOffset = ParseUint64(argv[7], "XDS file offset");
    }
    return options;
}

}  // namespace

int main(int argc, char **argv)
{
    try {
        const WorkerOptions options = ParseOptions(argc, argv);
#ifndef ASCEND_HBM_IPC_DEMO_ENABLE_XDS
        if (options.xdsMode) {
            throw std::runtime_error("Worker was built without ENABLE_XDS=ON");
        }
#endif

        AclSession acl(options.deviceId);
        SocketPathGuard socketPathGuard(options.socketPath);
        UniqueFd listener = ListenUnixSeqpacket(options.socketPath);
        std::cout << "[Worker] listening on " << options.socketPath << ", device=" << options.deviceId << std::endl;
        UniqueFd control = AcceptUnixSeqpacket(listener.Get());

        WireMessage hello = MakeMessage(MessageType::kHello);
        hello.deviceId = options.deviceId;
        hello.barePid = acl.BarePid();
        hello.processPid = static_cast<int32_t>(::getpid());
        SendMessage(control.Get(), hello);

        const WireMessage descriptor = ReceiveMessage(control.Get(), MessageType::kExportBuffer);
        if (descriptor.deviceId != options.deviceId || descriptor.size < kMutationSize) {
            throw std::runtime_error("invalid or cross-device descriptor; this demo requires the same Device ID");
        }
        if ((descriptor.flags & ~kWireFlagXdsRead) != 0) {
            throw std::runtime_error("descriptor contains unsupported wire flags");
        }
        const bool descriptorRequestsXds = (descriptor.flags & kWireFlagXdsRead) != 0;
        if (descriptorRequestsXds != options.xdsMode) {
            throw std::runtime_error("Client and Worker disagree on XDS mode");
        }
        if (options.xdsMode && descriptor.processPid <= 0) {
            throw std::runtime_error("Client descriptor contains an invalid process PID for XDS");
        }

        // Worker owns only the imported mapping. It must use
        // aclrtIpcMemClose(key), never aclrtFree(imported.Get()).
        ImportedIpcMemory imported(descriptor.key);
        std::cout << "[Worker] imported HBM: imported_ptr=" << imported.Get()
                  << ", size=" << descriptor.size << std::endl;

        WireMessage importedAck = MakeMessage(MessageType::kImported);
        importedAck.deviceId = options.deviceId;
        importedAck.bufferId = descriptor.bufferId;
        importedAck.generation = descriptor.generation;
        if (options.xdsMode) {
            SendMessage(control.Get(), importedAck);
#ifdef ASCEND_HBM_IPC_DEMO_ENABLE_XDS
            XdsReadRequest request;
            request.filePath = options.sourceFile;
            request.blockDevice = options.blockDevice;
            request.fileOffset = options.fileOffset;
            request.destinationAddress = reinterpret_cast<uintptr_t>(imported.Get());
            request.size = descriptor.size;
            request.deviceId = static_cast<uint32_t>(options.deviceId);
            request.vfId = options.vfId;
            request.clientPid = descriptor.processPid;
            std::cout << "[Worker] submitting XDS read with Client PID: client_pid="
                      << request.clientPid << ", imported_ptr=" << imported.Get() << std::endl;
            const XdsReadResult result = XdsReadFileToHbm(request);
            std::cout << "[Worker] XDS read completed directly into imported_ptr: file=" << options.sourceFile
                      << ", block_device=" << options.blockDevice << ", offset=" << options.fileOffset
                      << ", bytes=" << descriptor.size << ", client_pid=" << descriptor.processPid
                      << ", submit_us=" << result.submitElapsedUs
                      << ", drain_us=" << result.drainElapsedUs << std::endl;
#endif
        } else {
            std::vector<uint8_t> host(static_cast<size_t>(descriptor.size));
            CheckAcl(aclrtMemcpy(host.data(), host.size(), imported.Get(), host.size(), ACL_MEMCPY_DEVICE_TO_HOST),
                     "aclrtMemcpy(D2H imported verification)");
            PrintHexPreview("[Worker] 从 imported_ptr 读到的初始数据", host.data(), host.size());
            for (size_t i = 0; i < host.size(); ++i) {
                if (host[i] != PatternByte(i)) {
                    throw std::runtime_error("Worker verification failed at byte " + std::to_string(i));
                }
            }
            SendMessage(control.Get(), importedAck);

            std::vector<uint8_t> mutation(kMutationSize, kMutationValue);
            CheckAcl(aclrtMemcpy(imported.Get(), static_cast<size_t>(descriptor.size), mutation.data(), mutation.size(),
                                ACL_MEMCPY_HOST_TO_DEVICE),
                     "aclrtMemcpy(H2D imported mutation)");
            std::fill_n(host.begin(), kMutationSize, kMutationValue);
            PrintHexPreview("[Worker] 写入 imported_ptr 后的数据", host.data(), host.size());
            std::cout << "[Worker] modified the first " << kMutationSize << " bytes through imported_ptr" << std::endl;
        }

        WireMessage modified = MakeMessage(MessageType::kModified);
        modified.deviceId = options.deviceId;
        modified.bufferId = descriptor.bufferId;
        modified.generation = descriptor.generation;
        SendMessage(control.Get(), modified);

        const WireMessage release = ReceiveMessage(control.Get(), MessageType::kReleaseBuffer);
        ValidateBufferIdentity(release, descriptor.bufferId, descriptor.generation);

        imported.Close();
        WireMessage closed = MakeMessage(MessageType::kImportClosed);
        closed.deviceId = options.deviceId;
        closed.bufferId = descriptor.bufferId;
        closed.generation = descriptor.generation;
        SendMessage(control.Get(), closed);

        std::cout << "[Worker] imported mapping closed; Client may now Close and Free the owner HBM" << std::endl;
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "[Worker] failed: " << error.what() << std::endl;
        return 1;
    }
}
