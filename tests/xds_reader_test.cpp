#include "xds_reader.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include <unistd.h>

using namespace ascend_hbm_ipc_demo;

namespace {

class TempFile {
public:
    TempFile()
    {
        char path[] = "/tmp/xds-reader-test-XXXXXX";
        const int fd = ::mkstemp(path);
        if (fd < 0) {
            throw std::runtime_error("mkstemp failed");
        }
        path_ = path;
        if (::ftruncate(fd, 4096) != 0) {
            (void)::close(fd);
            (void)::unlink(path_.c_str());
            throw std::runtime_error("ftruncate failed");
        }
        (void)::close(fd);
    }

    ~TempFile()
    {
        (void)::unlink(path_.c_str());
    }

    TempFile(const TempFile &) = delete;
    TempFile &operator=(const TempFile &) = delete;

    const std::string &Path() const
    {
        return path_;
    }

private:
    std::string path_;
};

void ExpectRejected(const std::string &name, XdsReadRequest request)
{
    try {
        ValidateXdsReadRequest(request);
    } catch (const std::runtime_error &) {
        return;
    }
    throw std::runtime_error(name + " was unexpectedly accepted");
}

}  // namespace

int main()
{
    try {
        XdsReadRequest request;
        request.filePath = "/does/not/exist";
        request.blockDevice = "/dev/null";
        request.fileOffset = 0;
        request.destinationAddress = 0x200000;
        request.size = 4096;
        request.deviceId = 0;
        request.vfId = 0;
        request.destinationProcessId = 1234;

        auto missingDestinationProcessId = request;
        missingDestinationProcessId.destinationProcessId = 0;
        ExpectRejected("missing destination owner process ID", missingDestinationProcessId);

        auto emptyPath = request;
        emptyPath.filePath.clear();
        ExpectRejected("empty source path", emptyPath);

        auto nullDestination = request;
        nullDestination.destinationAddress = 0;
        ExpectRejected("null destination", nullDestination);

        auto unalignedOffset = request;
        unalignedOffset.fileOffset = 1;
        ExpectRejected("unaligned offset", unalignedOffset);

        auto unalignedAddress = request;
        unalignedAddress.destinationAddress += 1;
        ExpectRejected("unaligned destination", unalignedAddress);

        auto unalignedSize = request;
        unalignedSize.size = 513;
        ExpectRejected("unaligned size", unalignedSize);

        auto missingFile = request;
        ExpectRejected("missing source file", missingFile);

        TempFile file;
        auto nonBlockDevice = request;
        nonBlockDevice.filePath = file.Path();
        ExpectRejected("non-block backing path", nonBlockDevice);

        auto fileRangeOverflow = nonBlockDevice;
        fileRangeOverflow.size = 8192;
        ExpectRejected("file range overflow", fileRangeOverflow);

        std::cout << "PASS: XDS preflight rejects invalid owner PIDs, geometry, file ranges, and block devices"
                  << std::endl;
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << std::endl;
        return 1;
    }
}
