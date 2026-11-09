cmake_minimum_required(VERSION 4.0)

project(tt-kernel-lint LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LLVM REQUIRED CONFIG)

add_library(tt_kernel_lint_frontend STATIC
            tt_kernel_lint/src/ttkl/translator.cpp)
target_include_directories(tt_kernel_lint_frontend
                           PUBLIC tt_kernel_lint/include)
target_include_directories(tt_kernel_lint_frontend SYSTEM
                           PUBLIC ${LLVM_INCLUDE_DIRS})
target_link_libraries(tt_kernel_lint_frontend PUBLIC LLVMSupport)

if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(tt_kernel_lint_frontend PRIVATE -fno-rtti)
endif()

include(CTest)
if(BUILD_TESTING)
  find_package(GTest REQUIRED)
  include(GoogleTest)
  add_executable(tt_kernel_lint_tests tests/cpp/translator_test.cpp)
  target_link_libraries(tt_kernel_lint_tests PRIVATE tt_kernel_lint_frontend
                                                     GTest::gtest_main)
  gtest_discover_tests(tt_kernel_lint_tests)
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
