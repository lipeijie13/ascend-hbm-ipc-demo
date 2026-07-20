#include "ipc_common.h"

#include <type_traits>

using namespace ascend_hbm_ipc_demo;

static_assert(kHbmAllocationPolicy == ACL_MEM_MALLOC_HUGE_FIRST,
              "HBM allocation policy must match yuanrong-datasystem");
static_assert(kHbmExportFlags == ACL_RT_IPC_MEM_EXPORT_FLAG_DISABLE_PID_VALIDATION,
              "HBM export must explicitly disable PID validation like yuanrong-datasystem");
static_assert(std::is_constructible_v<ExportedIpcMemory, void *, size_t>,
              "HBM export must not require a Worker PID when the ACL PID whitelist is disabled");
static_assert(!std::is_constructible_v<ExportedIpcMemory, void *, size_t, int32_t>,
              "HBM export must not expose the old PID-whitelist constructor");

int main()
{
    const WireMessage descriptor = MakeMessage(MessageType::kExportBuffer);
    if (descriptor.ownerPid != -1 || descriptor.ownerBase != 0) {
        return 1;
    }
    return 0;
}
