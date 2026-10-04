cmake_minimum_required(VERSION 3.24)

project(RemoteTouchscreen VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "RemoteTouchHost currently supports Windows only")
endif()

include(CTest)
add_subdirectory(host)
