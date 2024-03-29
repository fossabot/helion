# stop if cmake version below 3.5
cmake_minimum_required(VERSION 3.5 FATAL_ERROR)
project(helion C CXX ASM)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set (CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)


SET(CMAKE_ASM_FLAGS "${CFLAGS} -x assembler-with-cpp")

set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DBUILD_MODE=Debug -fno-omit-frame-pointer")
set(CMAKE_C_FLAGS_DEBUG "-g -O0 -DBUILD_MODE=Debug -fno-omit-frame-pointer")

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -flto -DBUILD_MODE=Release")
set(CMAKE_C_FLAGS_RELEASE   "-O3 -flto -DBUILD_MODE=Release")



add_definitions(-DPOINTER_MASK=0x0007FFFFFFFFFFFF)


add_definitions(-DUSE_GC=true)

# build dependencies before setting up project deets
add_subdirectory(dep/libuv)



# set(BUILD_SHARED_LIBS ON)

link_directories(/usr/local/lib)
include_directories(/usr/local/include)
include_directories(dep/bdwgc/include)
include_directories(dep/libuv/include)


set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib")

add_definitions(-DBUILD_DIR="${BUILD_DIR}")

message(STATUS "Build Mode: ${CMAKE_BUILD_TYPE}")

add_compile_options(-frtti)

include_directories(${CMAKE_SOURCE_DIR}/include)

include(${CMAKE_SOURCE_DIR}/src/helion/CMakeLists.txt)

target_compile_options(helion PRIVATE -Wall -Wno-unused-function -Wno-sign-compare -Wextra -Wno-unused-parameter -Wno-strict-aliasing)


install(TARGETS helion DESTINATION bin CONFIGURATIONS Release)
# install(TARGETS helion-lib DESTINATION lib CONFIGURATIONS Release)
