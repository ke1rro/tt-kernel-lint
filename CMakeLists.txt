cmake_minimum_required(VERSION 4.0)

set(PROJECT_NAME tt-kernel-lint)
project(${PROJECT_NAME} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

add_executable(${PROJECT_NAME} main.cpp)

target_include_directories(${PROJECT_NAME} PRIVATE ${LLVM_INCLUDE_DIRS}
                                                   ${CLANG_INCLUDE_DIRS})

target_link_libraries(
  ${PROJECT_NAME} PRIVATE libclang clangTooling clangAST clangASTMatchers
                          clangFrontend clangBasic)

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
