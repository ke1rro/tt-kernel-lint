cmake_minimum_required(VERSION 4.0)

project(tt-kernel-lint LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

add_library(
  tt_kernel_lint_frontend STATIC
  tt_kernel_lint/src/ttkl/capture.cpp tt_kernel_lint/src/ttkl/sfpi_profile.cpp
  tt_kernel_lint/src/ttkl/translator.cpp)
target_include_directories(tt_kernel_lint_frontend
                           PUBLIC tt_kernel_lint/include)
target_include_directories(tt_kernel_lint_frontend SYSTEM
                           PUBLIC ${LLVM_INCLUDE_DIRS})
target_link_libraries(tt_kernel_lint_frontend PUBLIC LLVMSupport)

add_library(tt_kernel_lint_analysis STATIC
            tt_kernel_lint/src/ttkl/check/init_operation_pairing.cpp)
target_include_directories(tt_kernel_lint_analysis
                           PUBLIC tt_kernel_lint/include)
target_include_directories(tt_kernel_lint_analysis SYSTEM
                           PUBLIC ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
target_link_libraries(tt_kernel_lint_analysis PUBLIC tt_kernel_lint_frontend
                                                     clang-cpp LLVM)

add_executable(tt-init-check tt_kernel_lint/tools/tt-init-check/main.cpp)
target_link_libraries(tt-init-check PRIVATE tt_kernel_lint_analysis clang-cpp
                                            LLVM)
target_compile_options(
  tt-init-check
  PRIVATE $<$<AND:$<CONFIG:Debug>,$<CXX_COMPILER_ID:Clang>>:-fstandalone-debug>)

if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(tt_kernel_lint_frontend PRIVATE -fno-rtti)
  target_compile_options(tt_kernel_lint_analysis PRIVATE -fno-rtti)
  target_compile_options(tt-init-check PRIVATE -fno-rtti)
endif()

option(BUILD_KERNELS
       "Build the Metalium host example and its runtime dependencies" OFF)

if(BUILD_KERNELS)
  set(WITH_PYTHON_BINDINGS OFF)
  set(ENABLE_DISTRIBUTED OFF)
  add_subdirectory(tt-metal EXCLUDE_FROM_ALL)
  add_executable(example_kernel example_kernel/main.cpp)
  target_link_libraries(example_kernel PRIVATE TT::Metalium)
  target_compile_definitions(
    example_kernel
    PRIVATE
      EXAMPLE_KERNEL_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/example_kernel")
endif()
