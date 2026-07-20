#ifndef ASCEND_HBM_IPC_DEMO_XDS_READER_H_
#define ASCEND_HBM_IPC_DEMO_XDS_READER_H_

#include <cstdint>
#include <string>

namespace ascend_hbm_ipc_demo {

constexpr uint64_t kXdsAlignment = 512;

struct XdsReadRequest {
    std::string filePath;
    std::string blockDevice;
    uint64_t fileOffset = 0;
    uintptr_t destinationAddress = 0;
    uint64_t size = 0;
    uint32_t deviceId = 0;
    uint32_t vfId = 0;
    // PID of the process whose SVM address space owns destinationAddress.
    int32_t destinationProcessId = -1;
};

struct XdsReadResult {
    uint64_t submitElapsedUs = 0;
    uint64_t drainElapsedUs = 0;
};

void ValidateXdsReadRequest(const XdsReadRequest &request);
XdsReadResult XdsReadFileToHbm(const XdsReadRequest &request);

}  // namespace ascend_hbm_ipc_demo

#endif  // ASCEND_HBM_IPC_DEMO_XDS_READER_H_
