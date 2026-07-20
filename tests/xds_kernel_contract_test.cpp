#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

std::string ReadFile(const char *path)
{
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error(std::string("cannot open ") + path);
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

std::string FunctionBody(const std::string &source, const std::string &beginMarker,
                         const std::string &endMarker)
{
    const size_t begin = source.find(beginMarker);
    const size_t end = source.find(endMarker, begin);
    if (begin == std::string::npos || end == std::string::npos) {
        throw std::runtime_error("cannot locate kernel function boundary");
    }
    return source.substr(begin, end - begin);
}

void RequireContains(const std::string &source, const std::string &expected,
                     const std::string &message)
{
    if (source.find(expected) == std::string::npos) {
        throw std::runtime_error(message);
    }
}

void RequireNotContains(const std::string &source, const std::string &unexpected,
                        const std::string &message)
{
    if (source.find(unexpected) != std::string::npos) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main(int argc, char **argv)
{
    try {
        if (argc != 2) {
            throw std::runtime_error("usage: xds_kernel_contract_test p2p_dev.c");
        }
        const std::string source = ReadFile(argv[1]);
        const std::string ioContext = FunctionBody(
            source, "struct p2p_io_context {", "struct p2p_batch {");
        const std::string getPaList = FunctionBody(
            source, "static int get_pa_list(", "static int get_pa_list_batch(");
        const std::string freeIoContext = FunctionBody(
            source, "static void free_io_ctx(", "static struct p2p_io_context *new_io_ctx(");
        const std::string readFile = FunctionBody(
            source, "static int p2p_read_file(", "static int p2p_drain_read(");

        RequireNotContains(getPaList, "devmm_put_mem_pa_list",
                           "get_pa_list releases the HBM PA pin before asynchronous NVMe I/O");
        RequireContains(ioContext, "struct devmm_svm_process_id pa_process;",
                        "I/O context does not retain the DEVMM process identity");
        RequireContains(ioContext, "u64 pa_addr;",
                        "I/O context does not retain the pinned HBM address");
        RequireContains(ioContext, "u64 pa_bytes;",
                        "I/O context does not retain the pinned HBM size");
        RequireContains(ioContext, "bool pa_pinned;",
                        "I/O context does not track the HBM PA pin lifetime");
        RequireContains(freeIoContext, "if (io_ctx->pa_pinned)",
                        "I/O context teardown does not guard PA pin release");
        RequireContains(freeIoContext, "devmm_put_mem_pa_list(&io_ctx->pa_process",
                        "I/O context teardown does not release the HBM PA pin");
        RequireContains(readFile, "int err = 0;",
                        "p2p_read_file success return value is uninitialized");

        std::cout << "PASS: single-read HBM PA remains pinned until I/O context teardown" << std::endl;
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << std::endl;
        return 1;
    }
}
