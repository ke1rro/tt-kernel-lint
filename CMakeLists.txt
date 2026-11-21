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
  tt_kernel_lint/src/ttkl/capture.cpp
  tt_kernel_lint/src/ttkl/frontend.cpp
  tt_kernel_lint/src/ttkl/jit_capture.cpp
  tt_kernel_lint/src/ttkl/sfpi_profile.cpp
  tt_kernel_lint/src/ttkl/sfpi_toolchain.cpp
  tt_kernel_lint/src/ttkl/translator.cpp
  tt_kernel_lint/src/ttkl/wormhole_profile.cpp)
target_include_directories(tt_kernel_lint_frontend
                           PUBLIC tt_kernel_lint/include)
target_include_directories(tt_kernel_lint_frontend SYSTEM
                           PUBLIC ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
target_link_libraries(
  tt_kernel_lint_frontend
  PUBLIC clangTooling
         clangFrontend
         clangDriver
         clangSerialization
         clangAST
         clangBasic
         LLVMSupport)

if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(tt_kernel_lint_frontend PUBLIC -fno-rtti)
endif()

add_executable(tt-kernel-lint tt_kernel_lint/src/main.cpp)
target_link_libraries(tt-kernel-lint PRIVATE tt_kernel_lint_frontend)

option(TTKL_BUILD_TESTS "Build the tt-kernel-lint GoogleTest suite" OFF)

if(TTKL_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
  add_executable(
    tt_kernel_lint_tests
    tests/cpp/jit_capture_test.cpp tests/cpp/translator_test.cpp
    tests/cpp/wormhole_integration_test.cpp)
  target_link_libraries(
    tt_kernel_lint_tests PRIVATE tt_kernel_lint_frontend clangASTMatchers
                                 GTest::gtest_main)
  target_compile_definitions(
    tt_kernel_lint_tests PRIVATE TTKL_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  add_test(NAME tt_kernel_lint_tests COMMAND tt_kernel_lint_tests)
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
