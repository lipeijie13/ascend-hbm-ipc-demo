#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"${ROOT_DIR}/build"}
DEVICE_ID=${1:-0}
BUFFER_SIZE=${2:-2097152}
SOCKET_PATH=${SOCKET_PATH:-"/tmp/ascend-hbm-ipc-${UID}-$$.sock"}
XDS_ENABLE=${XDS_ENABLE:-0}
XDS_FILE=${XDS_FILE:-}
XDS_FILE_DIR=${XDS_FILE_DIR:-/tmp}
XDS_BLOCK_DEVICE=${XDS_BLOCK_DEVICE:-}
XDS_FILE_OFFSET=${XDS_FILE_OFFSET:-0}
XDS_VF_ID=${XDS_VF_ID:-0}
SKIP_BUILD=${SKIP_BUILD:-0}

if [[ "${XDS_ENABLE}" != "0" && "${XDS_ENABLE}" != "1" ]] \
    || [[ "${SKIP_BUILD}" != "0" && "${SKIP_BUILD}" != "1" ]]; then
    echo "XDS_ENABLE and SKIP_BUILD must be 0 or 1" >&2
    exit 2
fi
if [[ ! "${BUFFER_SIZE}" =~ ^[0-9]+$ || ! "${XDS_FILE_OFFSET}" =~ ^[0-9]+$ || ! "${XDS_VF_ID}" =~ ^[0-9]+$ ]]; then
    echo "BUFFER_SIZE, XDS_FILE_OFFSET, and XDS_VF_ID must be unsigned integers" >&2
    exit 2
fi

worker_pid=""
generated_xds_file=0

cleanup()
{
    if [[ -n "${worker_pid}" ]] && kill -0 "${worker_pid}" 2>/dev/null; then
        kill "${worker_pid}" 2>/dev/null || true
        wait "${worker_pid}" 2>/dev/null || true
    fi
    rm -f "${SOCKET_PATH}"
    if [[ "${generated_xds_file}" == "1" && -n "${XDS_FILE}" ]]; then
        rm -f "${XDS_FILE}"
    fi
}
trap cleanup EXIT INT TERM

if [[ "${SKIP_BUILD}" == "1" ]]; then
    if [[ ! -x "${BUILD_DIR}/worker" || ! -x "${BUILD_DIR}/client" ]]; then
        echo "SKIP_BUILD=1 requires existing worker and client executables in ${BUILD_DIR}" >&2
        exit 1
    fi
else
    cmake_args=(-DCMAKE_BUILD_TYPE=Release)
    if [[ "${XDS_ENABLE}" == "1" ]]; then
        cmake_args+=(-DENABLE_XDS=ON)
    else
        cmake_args+=(-DENABLE_XDS=OFF)
    fi
    cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" "${cmake_args[@]}"
    cmake --build "${BUILD_DIR}" --parallel
fi

worker_args=("${SOCKET_PATH}" "${DEVICE_ID}")
client_args=("${SOCKET_PATH}" "${DEVICE_ID}" "${BUFFER_SIZE}")

if [[ "${XDS_ENABLE}" == "1" ]]; then
    if (( BUFFER_SIZE == 0 || BUFFER_SIZE > 4294967295 || BUFFER_SIZE % 2097152 != 0
          || XDS_FILE_OFFSET % 512 != 0 || XDS_VF_ID > 65535 )); then
        echo "XDS requires a 2-MiB-multiple BUFFER_SIZE <= UINT_MAX, a 512-byte-aligned offset, and VF ID <= 65535" >&2
        exit 2
    fi
    if [[ ! -c /dev/p2p_device || ! -r /dev/p2p_device || ! -w /dev/p2p_device ]]; then
        echo "XDS is enabled but /dev/p2p_device is unavailable or inaccessible; build the xds_kernel_module target and ask an administrator to load/configure it" >&2
        exit 1
    fi

    required_file_size=$((XDS_FILE_OFFSET + BUFFER_SIZE))
    if [[ -z "${XDS_FILE}" ]]; then
        XDS_FILE=$(mktemp --tmpdir="${XDS_FILE_DIR}" "ascend-hbm-xds-${UID}-XXXXXX.bin")
        generated_xds_file=1
        block_count=$(((required_file_size + 1048575) / 1048576))
        dd if=/dev/urandom of="${XDS_FILE}" bs=1048576 count="${block_count}" conv=fsync status=none
        truncate -s "${required_file_size}" "${XDS_FILE}"
        sync -f "${XDS_FILE}"
    elif [[ ! -f "${XDS_FILE}" ]]; then
        echo "XDS_FILE is not a regular file: ${XDS_FILE}" >&2
        exit 2
    fi
    XDS_FILE=$(readlink -f "${XDS_FILE}")

    actual_file_size=$(stat -c '%s' "${XDS_FILE}")
    if (( actual_file_size < required_file_size )); then
        echo "XDS_FILE is too small: need ${required_file_size} bytes, found ${actual_file_size}" >&2
        exit 2
    fi

    if [[ -z "${XDS_BLOCK_DEVICE}" ]]; then
        XDS_BLOCK_DEVICE=$(findmnt -n -o SOURCE -T "${XDS_FILE}")
    fi
    XDS_BLOCK_DEVICE=$(readlink -f "${XDS_BLOCK_DEVICE}")
    if [[ ! -b "${XDS_BLOCK_DEVICE}" || ! -r "${XDS_BLOCK_DEVICE}" ]]; then
        echo "XDS_BLOCK_DEVICE is not a readable block device: ${XDS_BLOCK_DEVICE}" >&2
        exit 2
    fi

    echo "[run_demo] XDS source=${XDS_FILE}, block_device=${XDS_BLOCK_DEVICE}, offset=${XDS_FILE_OFFSET}, bytes=${BUFFER_SIZE}, device=${DEVICE_ID}, vf=${XDS_VF_ID}"
    worker_args+=(--xds "${XDS_FILE}" "${XDS_BLOCK_DEVICE}" "${XDS_VF_ID}" "${XDS_FILE_OFFSET}")
    client_args+=(--xds "${XDS_FILE}" "${XDS_FILE_OFFSET}")
fi

rm -f "${SOCKET_PATH}"
"${BUILD_DIR}/worker" "${worker_args[@]}" &
worker_pid=$!

"${BUILD_DIR}/client" "${client_args[@]}"
wait "${worker_pid}"
worker_pid=""

trap - EXIT INT TERM
cleanup
