#include "ipc_common.h"

#include <type_traits>

using namespace ascend_hbm_ipc_demo;

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
