cmake_minimum_required(VERSION 3.27)
project(cabe)

set(CMAKE_CXX_STANDARD 20)

add_executable(cabe main.cpp
        ioutil/IOManager.h
        ioutil/stdio/STDIO.h
        ioutil/stdio/STDIO.cpp
        memory/Index.h
        memory/rbtree/HashIndex.cpp
        memory/rbtree/HashIndex.h
        db/Cabe.cpp
        db/Cabe.h
)
