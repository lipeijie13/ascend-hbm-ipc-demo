#include "ipc_common.h"

#include <vector>

using namespace ascend_hbm_ipc_demo;

int main(int argc, char **argv)
{
    const std::string socketPath = argc > 1 ? argv[1] : "/tmp/ascend-hbm-ipc-demo.sock";
    const int32_t deviceId = argc > 2 ? ParseDeviceId(argv[2]) : 0;

    try {
        AclSession acl(deviceId);
        SocketPathGuard socketPathGuard(socketPath);
        UniqueFd listener = ListenUnixSeqpacket(socketPath);
        std::cout << "[Worker] listening on " << socketPath << ", device=" << deviceId << std::endl;
        UniqueFd control = AcceptUnixSeqpacket(listener.Get());

        WireMessage hello = MakeMessage(MessageType::kHello);
        hello.deviceId = deviceId;
        hello.barePid = acl.BarePid();
        SendMessage(control.Get(), hello);

        const WireMessage descriptor = ReceiveMessage(control.Get(), MessageType::kExportBuffer);
        if (descriptor.deviceId != deviceId || descriptor.size < kMutationSize) {
            throw std::runtime_error("invalid or cross-device descriptor; this demo requires the same Device ID");
        }

        // Worker owns only the imported mapping. It must use
        // aclrtIpcMemClose(key), never aclrtFree(imported.Get()).
        ImportedIpcMemory imported(descriptor.key);
        std::cout << "[Worker] imported HBM: imported_ptr=" << imported.Get()
                  << ", size=" << descriptor.size << std::endl;

        std::vector<uint8_t> host(static_cast<size_t>(descriptor.size));
        CheckAcl(aclrtMemcpy(host.data(), host.size(), imported.Get(), host.size(), ACL_MEMCPY_DEVICE_TO_HOST),
                 "aclrtMemcpy(D2H imported verification)");
        PrintHexPreview("[Worker] 从 imported_ptr 读到的初始数据", host.data(), host.size());
        for (size_t i = 0; i < host.size(); ++i) {
            if (host[i] != PatternByte(i)) {
                throw std::runtime_error("Worker verification failed at byte " + std::to_string(i));
            }
        }

        WireMessage importedAck = MakeMessage(MessageType::kImported);
        importedAck.deviceId = deviceId;
        importedAck.bufferId = descriptor.bufferId;
        importedAck.generation = descriptor.generation;
        SendMessage(control.Get(), importedAck);

        std::vector<uint8_t> mutation(kMutationSize, kMutationValue);
        CheckAcl(aclrtMemcpy(imported.Get(), static_cast<size_t>(descriptor.size), mutation.data(), mutation.size(),
                            ACL_MEMCPY_HOST_TO_DEVICE),
                 "aclrtMemcpy(H2D imported mutation)");
        std::fill_n(host.begin(), kMutationSize, kMutationValue);
        PrintHexPreview("[Worker] 写入 imported_ptr 后的数据", host.data(), host.size());

        WireMessage modified = MakeMessage(MessageType::kModified);
        modified.deviceId = deviceId;
        modified.bufferId = descriptor.bufferId;
        modified.generation = descriptor.generation;
        SendMessage(control.Get(), modified);
        std::cout << "[Worker] modified the first " << kMutationSize << " bytes through imported_ptr" << std::endl;

        const WireMessage release = ReceiveMessage(control.Get(), MessageType::kReleaseBuffer);
        ValidateBufferIdentity(release, descriptor.bufferId, descriptor.generation);

        imported.Close();
        WireMessage closed = MakeMessage(MessageType::kImportClosed);
        closed.deviceId = deviceId;
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
