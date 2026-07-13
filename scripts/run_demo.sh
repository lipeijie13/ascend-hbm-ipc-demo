#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"${ROOT_DIR}/build"}
DEVICE_ID=${1:-0}
BUFFER_SIZE=${2:-2097152}
SOCKET_PATH=${SOCKET_PATH:-"/tmp/ascend-hbm-ipc-${UID}-$$.sock"}

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}" --parallel

rm -f "${SOCKET_PATH}"
"${BUILD_DIR}/worker" "${SOCKET_PATH}" "${DEVICE_ID}" &
worker_pid=$!

cleanup()
{
    if kill -0 "${worker_pid}" 2>/dev/null; then
        kill "${worker_pid}" 2>/dev/null || true
        wait "${worker_pid}" 2>/dev/null || true
    fi
    rm -f "${SOCKET_PATH}"
}
trap cleanup EXIT INT TERM

"${BUILD_DIR}/client" "${SOCKET_PATH}" "${DEVICE_ID}" "${BUFFER_SIZE}"
wait "${worker_pid}"

trap - EXIT INT TERM
rm -f "${SOCKET_PATH}"

