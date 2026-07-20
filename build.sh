#!/usr/bin/env bash

set -Eeuo pipefail

readonly ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

BUILD_DIR=${BUILD_DIR:-"${ROOT_DIR}/build"}
DEVICE_ID=${1:-${DEVICE_ID:-0}}
BUFFER_SIZE=${2:-${BUFFER_SIZE:-2097152}}
XDS_ENABLE=${XDS_ENABLE:-1}
RUN_TESTS=${RUN_TESTS:-1}
XDS_BUILD_MODULE=${XDS_BUILD_MODULE:-1}
XDS_LOAD_MODULE=${XDS_LOAD_MODULE:-0}

usage()
{
    cat <<'EOF'
Usage: ./build.sh [device-id] [buffer-size]

Build, test, and run the HBM IPC demo. XDS mode is enabled by default.

Common environment variables:
  BUILD_DIR=PATH          CMake build directory (default: ./build)
  XDS_ENABLE=0|1          Run normal IPC or XDS SSD-to-HBM mode (default: 1)
  RUN_TESTS=0|1           Run CTest after building (default: 1)
  XDS_BUILD_MODULE=0|1    Build p2p_dev.ko in XDS mode (default: 1)
  XDS_LOAD_MODULE=0|1     Load p2p_dev with sudo when needed (default: 0)
  XDS_FILE=PATH           Existing SSD source file; omitted creates a temporary file
  XDS_FILE_DIR=PATH       Directory for the temporary source file (default: repository root)
  XDS_BLOCK_DEVICE=PATH   Block device backing XDS_FILE; auto-detected if omitted
  XDS_FILE_OFFSET=BYTES   512-byte-aligned source offset (default: 0)
  XDS_VF_ID=ID            Ascend virtual-function ID (default: 0)

Examples:
  ./build.sh
  XDS_LOAD_MODULE=1 ./build.sh 0 2097152
  XDS_FILE=/mnt/nvme/xds.bin XDS_BLOCK_DEVICE=/dev/nvme0n1p2 ./build.sh
  XDS_ENABLE=0 ./build.sh
EOF
}

die()
{
    echo "build.sh: $*" >&2
    exit 1
}

require_boolean()
{
    local name=$1
    local value=$2
    [[ "${value}" == "0" || "${value}" == "1" ]] || die "${name} must be 0 or 1"
}

load_xds_module()
{
    if [[ "${EUID}" -eq 0 ]]; then
        BUILD_DIR="${BUILD_DIR}" bash "${ROOT_DIR}/scripts/xds_module.sh" load
        return
    fi
    command -v sudo >/dev/null 2>&1 || die "sudo is required when XDS_LOAD_MODULE=1"
    sudo env BUILD_DIR="${BUILD_DIR}" bash "${ROOT_DIR}/scripts/xds_module.sh" load
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi
[[ $# -le 2 ]] || { usage >&2; exit 2; }

require_boolean XDS_ENABLE "${XDS_ENABLE}"
require_boolean RUN_TESTS "${RUN_TESTS}"
require_boolean XDS_BUILD_MODULE "${XDS_BUILD_MODULE}"
require_boolean XDS_LOAD_MODULE "${XDS_LOAD_MODULE}"
[[ "${DEVICE_ID}" =~ ^[0-9]+$ ]] || die "device-id must be an unsigned integer"
[[ "${BUFFER_SIZE}" =~ ^[0-9]+$ ]] || die "buffer-size must be an unsigned integer"

cmake_args=(-DCMAKE_BUILD_TYPE=Release)
if [[ "${XDS_ENABLE}" == "1" ]]; then
    cmake_args+=(-DENABLE_XDS=ON)
else
    cmake_args+=(-DENABLE_XDS=OFF)
fi
if [[ "${RUN_TESTS}" == "1" ]]; then
    cmake_args+=(-DBUILD_TESTING=ON)
else
    cmake_args+=(-DBUILD_TESTING=OFF)
fi

echo "[build.sh] configuring: build=${BUILD_DIR}, XDS=${XDS_ENABLE}, device=${DEVICE_ID}, bytes=${BUFFER_SIZE}"
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" "${cmake_args[@]}"
cmake --build "${BUILD_DIR}" --parallel

if [[ "${RUN_TESTS}" == "1" ]]; then
    ctest --test-dir "${BUILD_DIR}" --output-on-failure
fi

if [[ "${XDS_ENABLE}" == "1" ]]; then
    if [[ "${XDS_BUILD_MODULE}" == "1" ]]; then
        cmake --build "${BUILD_DIR}" --target xds_kernel_module --parallel
    fi

    if ! bash "${ROOT_DIR}/scripts/xds_module.sh" status; then
        if [[ "${XDS_LOAD_MODULE}" == "1" ]]; then
            load_xds_module
        else
            die "XDS is not ready; load the compatible module with 'XDS_LOAD_MODULE=1 ./build.sh' or ask an administrator to run 'BUILD_DIR=${BUILD_DIR} bash scripts/xds_module.sh load'"
        fi
    fi
fi

export BUILD_DIR XDS_ENABLE
export SKIP_BUILD=1
exec bash "${ROOT_DIR}/scripts/run_demo.sh" "${DEVICE_ID}" "${BUFFER_SIZE}"
