cmake_minimum_required(VERSION 3.30.5)
project(cabe)

set(CMAKE_CXX_STANDARD 20)

add_executable(cabe main.cpp
        util/crc32.h
        util/crc32.cpp
        memory/index.h
        memory/index.cpp
        localstore/std_io.cpp
        localstore/std_io.h
        db/cabe.cpp
        db/cabe.h
        util/util.h
)


include_directories(${CMAKE_SOURCE_DIR})