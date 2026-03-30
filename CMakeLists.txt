cmake_minimum_required(VERSION 3.30.5)
project(cabe)

set(CMAKE_CXX_STANDARD 20)

add_executable(cabe
        main.cpp
        util/util.h
        util/crc32.h
        util/crc32.cpp
        common/error_code.h
        common/structs.h
        memory/index.h
        memory/index.cpp
        storage/free_list.cpp
        storage/free_list.h
        storage/storage.cpp
        storage/storage.h
        engine/engine.cpp
        engine/engine.h
)


include_directories(${CMAKE_SOURCE_DIR})