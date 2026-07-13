#include "ipc_common.h"

#include <vector>

using namespace ascend_hbm_ipc_demo;

namespace {
constexpr uint64_t kBufferId = 1;
constexpr uint64_t kGeneration = 1;
}

int main(int argc, char **argv)
{
    const std::string socketPath = argc > 1 ? argv[1] : "/tmp/ascend-hbm-ipc-demo.sock";
    const int32_t deviceId = argc > 2 ? ParseDeviceId(argv[2]) : 0;
    const size_t bufferSize = argc > 3 ? ParseBufferSize(argv[3]) : kDefaultBufferSize;

    try {
        AclSession acl(deviceId);
        UniqueFd control = ConnectUnixSeqpacket(socketPath);
        const WireMessage hello = ReceiveMessage(control.Get(), MessageType::kHello);
        if (hello.deviceId != deviceId || hello.barePid < 0) {
            throw std::runtime_error("Worker HELLO has an invalid or different Device ID");
        }

        OwnedDeviceBuffer owner(bufferSize);
        std::vector<uint8_t> host(bufferSize);
        for (size_t i = 0; i < host.size(); ++i) {
            host[i] = PatternByte(i);
        }
        PrintHexPreview("[Client] 初始 pattern", host.data(), host.size());
        CheckAcl(aclrtMemcpy(owner.Get(), owner.Size(), host.data(), host.size(), ACL_MEMCPY_HOST_TO_DEVICE),
                 "aclrtMemcpy(H2D initial pattern)");

        std::cout << "[Client] owner HBM allocated: device=" << deviceId << ", owner_ptr=" << owner.Get()
                  << ", size=" << owner.Size() << std::endl;

        // Destruction order is exported -> owner -> ACL session. On the normal
        // path we still close explicitly to make the required ordering clear.
        ExportedIpcMemory exported(owner.Get(), owner.Size(), hello.barePid);
        bool published = false;
        bool workerClosed = false;
        bool exporterClosed = false;
        try {
            WireMessage descriptor = MakeMessage(MessageType::kExportBuffer);
            descriptor.deviceId = deviceId;
            descriptor.barePid = acl.BarePid();
            descriptor.bufferId = kBufferId;
            descriptor.generation = kGeneration;
            descriptor.size = owner.Size();
            descriptor.key = exported.Key();
            SendMessage(control.Get(), descriptor);
            published = true;
            std::cout << "[Client] 65-byte IPC Key exported and sent with PID whitelist enabled" << std::endl;

            WireMessage imported = ReceiveMessage(control.Get(), MessageType::kImported);
            ValidateBufferIdentity(imported, kBufferId, kGeneration);
            std::cout << "[Client] Worker imported the HBM mapping" << std::endl;

            WireMessage modified = ReceiveMessage(control.Get(), MessageType::kModified);
            ValidateBufferIdentity(modified, kBufferId, kGeneration);

            CheckAcl(aclrtMemcpy(host.data(), host.size(), owner.Get(), owner.Size(), ACL_MEMCPY_DEVICE_TO_HOST),
                     "aclrtMemcpy(D2H owner verification)");
            PrintHexPreview("[Client] 从 owner_ptr 读回 Worker 修改后的数据", host.data(), host.size());
            for (size_t i = 0; i < host.size(); ++i) {
                const uint8_t expected = i < kMutationSize ? kMutationValue : PatternByte(i);
                if (host[i] != expected) {
                    throw std::runtime_error("Client verification failed at byte " + std::to_string(i));
                }
            }
            std::cout << "[Client] observed Worker's in-place HBM mutation through owner_ptr" << std::endl;

            WireMessage release = MakeMessage(MessageType::kReleaseBuffer);
            release.deviceId = deviceId;
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

        std::cout << "PASS: two process-local Device VAs accessed the same physical HBM; "
                     "Worker Close -> Client Close -> owner Free completed"
                  << std::endl;
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "[Client] failed: " << error.what() << std::endl;
        return 1;
    }
}
