#include "xds_reader.h"

#include <chrono>
#include <climits>
#include <cstring>
#include <limits>
#include <stdexcept>

#include <sys/stat.h>

extern "C" {
#include "file_p2p_api.h"
}

namespace ascend_hbm_ipc_demo {
namespace {

uint64_t ElapsedUs(const std::chrono::steady_clock::time_point &start)
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
}

void Require(bool condition, const std::string &message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string XdsErrorMessage(const char *operation, int error)
{
    std::string message = std::string(operation) + " failed, error=" + std::to_string(error);
    if (error < 0 && error != std::numeric_limits<int>::min()) {
        message += " (";
        message += std::strerror(-error);
        message += ')';
    }
    return message;
}

class XdsDevice {
public:
    XdsDevice() : fd_(new_p2p_fd())
    {
        if (fd_ < 0) {
            throw std::runtime_error(XdsErrorMessage("new_p2p_fd", fd_));
        }
    }

    ~XdsDevice()
    {
        close_p2p_fd(fd_);
    }

    XdsDevice(const XdsDevice &) = delete;
    XdsDevice &operator=(const XdsDevice &) = delete;

    int Get() const
    {
        return fd_;
    }

private:
    int fd_;
};

}  // namespace

void ValidateXdsReadRequest(const XdsReadRequest &request)
{
    Require(!request.filePath.empty(), "XDS source file path is empty");
    Require(!request.blockDevice.empty(), "XDS block device path is empty");
    Require(request.destinationAddress != 0, "XDS destination address is null");
    Require(request.size > 0 && request.size <= std::numeric_limits<unsigned int>::max(),
            "XDS read size is outside the userspace API range");
    Require(request.fileOffset <= std::numeric_limits<unsigned long>::max()
                && request.destinationAddress <= std::numeric_limits<unsigned long>::max(),
            "XDS offset or destination address is outside the userspace API range");
    Require(request.deviceId <= std::numeric_limits<unsigned short>::max()
                && request.vfId <= std::numeric_limits<unsigned short>::max(),
            "XDS device identifier is outside the userspace API range");
    Require(request.fileOffset % kXdsAlignment == 0
                && request.destinationAddress % kXdsAlignment == 0
                && request.size % kXdsAlignment == 0,
            "XDS file offset, HBM address, and size must be 512-byte aligned");
    Require(request.fileOffset <= std::numeric_limits<uint64_t>::max() - request.size,
            "XDS source file range overflows");

    struct stat fileStat = {};
    Require(stat(request.filePath.c_str(), &fileStat) == 0 && S_ISREG(fileStat.st_mode),
            "XDS source path is unavailable or is not a regular file");
    Require(fileStat.st_size >= 0
                && request.fileOffset + request.size <= static_cast<uint64_t>(fileStat.st_size),
            "XDS source range exceeds the file size");

    struct stat blockStat = {};
    Require(stat(request.blockDevice.c_str(), &blockStat) == 0 && S_ISBLK(blockStat.st_mode),
            "XDS block device path is unavailable or is not a block device");
    Require(blockStat.st_rdev == fileStat.st_dev,
            "XDS block device does not back the source file filesystem");
}

XdsReadResult XdsReadFileToHbm(const XdsReadRequest &request)
{
    ValidateXdsReadRequest(request);

    XdsDevice device;
    read_parameter parameter{};
    parameter.file_name = request.filePath.c_str();
    parameter.bdev_name = request.blockDevice.c_str();
    parameter.bdev_offset = static_cast<unsigned long>(request.fileOffset);
    parameter.devid = static_cast<unsigned short>(request.deviceId);
    parameter.vfid = static_cast<unsigned short>(request.vfId);
    parameter.size = static_cast<unsigned int>(request.size);
    parameter.addr = static_cast<unsigned long>(request.destinationAddress);

    XdsReadResult result;
    auto submitStart = std::chrono::steady_clock::now();
    const int submitError = read_file(device.Get(), &parameter);
    result.submitElapsedUs = ElapsedUs(submitStart);
    if (submitError != 0) {
        throw std::runtime_error(XdsErrorMessage("read_file", submitError));
    }

    auto drainStart = std::chrono::steady_clock::now();
    const int drainError = drain_read(device.Get());
    result.drainElapsedUs = ElapsedUs(drainStart);
    if (drainError != 0) {
        throw std::runtime_error(XdsErrorMessage("drain_read", drainError));
    }
    return result;
}

}  // namespace ascend_hbm_ipc_demo
