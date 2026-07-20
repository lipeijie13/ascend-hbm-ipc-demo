cmake_minimum_required(VERSION 3.16)

add_library(xds_file_p2p STATIC
  file_p2p/file_p2p_api.c)

target_include_directories(xds_file_p2p PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/file_p2p")
target_compile_features(xds_file_p2p PRIVATE c_std_11)
target_compile_options(xds_file_p2p PRIVATE -Wall -Wextra)
set_target_properties(xds_file_p2p PROPERTIES POSITION_INDEPENDENT_CODE ON)

execute_process(
  COMMAND uname -r
  OUTPUT_VARIABLE XDS_KERNEL_RELEASE
  OUTPUT_STRIP_TRAILING_WHITESPACE)
set(XDS_KERNEL_BUILD_DIR
  "/lib/modules/${XDS_KERNEL_RELEASE}/build"
  CACHE PATH "Linux kernel build directory used for the XDS module")

find_program(XDS_KERNEL_MAKE NAMES gmake make)
if(XDS_KERNEL_MAKE AND EXISTS "${XDS_KERNEL_BUILD_DIR}/Makefile")
  set(XDS_KERNEL_MODULE_DIR "${CMAKE_BINARY_DIR}/xds-kernel/p2p_dev")
  file(MAKE_DIRECTORY "${XDS_KERNEL_MODULE_DIR}")
  configure_file(p2p_dev.c "${XDS_KERNEL_MODULE_DIR}/p2p_dev.c" COPYONLY)
  configure_file(p2p_dev_uapi.h "${XDS_KERNEL_MODULE_DIR}/p2p_dev_uapi.h" COPYONLY)
  configure_file(p2p_mem_query.h "${XDS_KERNEL_MODULE_DIR}/p2p_mem_query.h" COPYONLY)
  file(WRITE "${XDS_KERNEL_MODULE_DIR}/Makefile" "obj-m += p2p_dev.o\n")

  # The target is deliberately not part of ALL. Building or loading an
  # experimental kernel module is an explicit administrator action.
  add_custom_target(xds_kernel_module
    COMMAND "${XDS_KERNEL_MAKE}" -C "${XDS_KERNEL_BUILD_DIR}"
            "M=${XDS_KERNEL_MODULE_DIR}" KBUILD_MODPOST_WARN=1 modules
    BYPRODUCTS "${XDS_KERNEL_MODULE_DIR}/p2p_dev.ko"
    COMMENT "Building the experimental XDS p2p_dev kernel module"
    VERBATIM)
else()
  message(STATUS
    "XDS kernel-module target disabled: matching kernel headers or make are unavailable")
endif()
