#!/usr/bin/env bash
# Load the experimental XDS p2p driver on a Worker host.
#
# This is intentionally a node-administration tool, not a Worker startup hook:
# loading a kernel module changes host-wide state and requires CAP_SYS_MODULE.
# Run it once on every XDS Worker node before starting the Worker service.

set -Eeuo pipefail

readonly MODULE_NAME="p2p_dev"
readonly DEVICE_PATH="/dev/p2p_device"
readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
MODULE_PATH="${XDS_MODULE:-${BUILD_DIR}/xds-kernel/p2p_dev/p2p_dev.ko}"

usage() {
    cat <<'EOF'
Usage: xds_module.sh [load|status|unload] [--module PATH]

Load, inspect, or safely unload the experimental XDS p2p kernel module.

Commands:
  load       Validate prerequisites and load p2p_dev. This is the default.
  status     Report whether p2p_dev and /dev/p2p_device are ready.
  unload     Remove p2p_dev. The kernel refuses the operation while it is in use.

Options:
  --module PATH  Kernel module path. Defaults to XDS_MODULE or
                 build/xds-kernel/p2p_dev/p2p_dev.ko.
  -h, --help     Show this help.

Run this on the Worker host before the Worker starts. It must run as root and
does not change /dev/p2p_device permissions; configure device access with the
deployment's udev/container policy.
EOF
}

die() {
    echo "xds module: $*" >&2
    exit 1
}

is_loaded() {
    awk -v module="${MODULE_NAME}" '$1 == module { found = 1 } END { exit !found }' /proc/modules
}

require_root() {
    [[ "${EUID}" -eq 0 ]] || die "root privileges are required to change kernel-module state"
}

check_module_compatibility() {
    [[ -f "${MODULE_PATH}" ]] || die "module not found: ${MODULE_PATH}"

    command -v modinfo >/dev/null 2>&1 || die "modinfo is required to validate ${MODULE_PATH}"
    local vermagic
    vermagic="$(modinfo -F vermagic "${MODULE_PATH}")"
    [[ -n "${vermagic}" ]] || die "cannot read module vermagic: ${MODULE_PATH}"
    [[ "${vermagic%% *}" == "$(uname -r)" ]] || die \
        "module kernel release (${vermagic%% *}) does not match running kernel ($(uname -r))"
}

check_ascend_driver() {
    local symbol
    for symbol in devmm_get_mem_pa_list devmm_put_mem_pa_list devmm_get_mem_page_size; do
        grep -qw "${symbol}" /proc/kallsyms || die "Ascend driver symbol is unavailable: ${symbol}"
    done
}

get_nvme_tracepoint_address() {
    local address
    address="$(awk '/__tracepoint_nvme_setup_cmd/ { print $1; exit }' /proc/kallsyms)"
    [[ -n "${address}" && "${address}" != "0000000000000000" ]] || \
        die "nvme_setup_cmd tracepoint is unavailable"
    printf '%s\n' "${address}"
}

status() {
    if ! is_loaded; then
        echo "XDS module is not loaded: ${MODULE_NAME}"
        return 1
    fi
    if [[ ! -c "${DEVICE_PATH}" ]]; then
        echo "XDS module is loaded but device node is missing: ${DEVICE_PATH}" >&2
        return 1
    fi
    echo "XDS module is ready: ${DEVICE_PATH}"
}

load() {
    require_root
    [[ "$(uname -s)" == "Linux" ]] || die "Linux is required"

    if is_loaded; then
        status
        return
    fi

    check_module_compatibility
    check_ascend_driver
    command -v insmod >/dev/null 2>&1 || die "insmod is required"

    local tracepoint_address
    tracepoint_address="$(get_nvme_tracepoint_address)"
    insmod "${MODULE_PATH}" "tp_nvme_setup_cmd_addr=0x${tracepoint_address}"
    status
}

unload() {
    require_root
    if ! is_loaded; then
        echo "XDS module is not loaded: ${MODULE_NAME}"
        return
    fi
    command -v rmmod >/dev/null 2>&1 || die "rmmod is required"
    rmmod "${MODULE_NAME}"
    echo "XDS module unloaded: ${MODULE_NAME}"
}

main() {
    local command="load"
    if [[ $# -gt 0 && "${1}" != --* && "${1}" != "-h" ]]; then
        command="$1"
        shift
    fi

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --module)
                [[ $# -ge 2 ]] || die "--module requires a path"
                MODULE_PATH="$2"
                shift 2
                ;;
            -h|--help)
                usage
                return
                ;;
            *)
                die "unknown argument: $1"
                ;;
        esac
    done

    case "${command}" in
        load) load ;;
        status) status ;;
        unload) unload ;;
        *) usage >&2; die "unknown command: ${command}" ;;
    esac
}

main "$@"
