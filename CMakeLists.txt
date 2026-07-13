cmake_minimum_required(VERSION 3.16)
project(ascend_hbm_ipc_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(DEFINED ENV{ASCEND_HOME_PATH})
  set(ASCEND_ROOT "$ENV{ASCEND_HOME_PATH}")
elseif(DEFINED ENV{ASCEND_TOOLKIT_HOME})
  set(ASCEND_ROOT "$ENV{ASCEND_TOOLKIT_HOME}")
else()
  set(ASCEND_ROOT "/usr/local/Ascend/ascend-toolkit/latest")
endif()

find_path(ASCEND_ACL_INCLUDE_DIR acl/acl_rt.h
  HINTS
    "${ASCEND_ROOT}/include"
    "${ASCEND_ROOT}/x86_64-linux/include"
    "/usr/local/Ascend/cann/include"
  NO_DEFAULT_PATH)

find_library(ASCENDCL_LIBRARY ascendcl
  HINTS
    "${ASCEND_ROOT}/lib64"
    "${ASCEND_ROOT}/x86_64-linux/lib64"
    "/usr/local/Ascend/cann/lib64"
  NO_DEFAULT_PATH)

if(NOT ASCEND_ACL_INCLUDE_DIR OR NOT ASCENDCL_LIBRARY)
  message(FATAL_ERROR
    "AscendCL headers/library not found. Source the CANN set_env.sh first, "
    "or set ASCEND_HOME_PATH. Current ASCEND_ROOT=${ASCEND_ROOT}")
endif()

add_executable(client src/client.cpp)
add_executable(worker src/worker.cpp)

foreach(target client worker)
  target_include_directories(${target} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${ASCEND_ACL_INCLUDE_DIR}")
  target_link_libraries(${target} PRIVATE "${ASCENDCL_LIBRARY}")
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  set_target_properties(${target} PROPERTIES
    BUILD_RPATH "${ASCEND_ROOT}/lib64;${ASCEND_ROOT}/x86_64-linux/lib64")
endforeach()
