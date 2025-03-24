cmake_minimum_required(VERSION 3.13)

project(InputAnalyzer LANGUAGES C CXX)

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)
find_package(nlohmann_json REQUIRED)

include_directories(SYSTEM 
    ${LLVM_INCLUDE_DIRS}
    ${Clang_INCLUDE_DIRS}
    ${nlohmann_json_INCLUDE_DIRS}
)

add_definitions(${LLVM_DEFINITIONS})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(InputAnalyzer
    main.cpp
)


target_link_libraries(InputAnalyzer PRIVATE
    clangTooling
    clangBasic
    clangAST
    nlohmann_json::nlohmann_json
)