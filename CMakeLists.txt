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
