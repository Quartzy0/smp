cmake_minimum_required(VERSION 3.16.0)

project(smp C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_BUILD_TYPE Debug)

find_package(CURL REQUIRED)
include_directories(/usr/include/dbus-1.0)
include_directories(/usr/lib/x86_64-linux-gnu/dbus-1.0/include)
include_directories(include)

add_executable(smp src/main.c src/spotify.c src/util.c src/spotify.h src/util.h src/audio.c src/audio.h src/dbus.c src/dbus.h src/config.c src/config.h)

add_subdirectory(lib/cjson EXCLUDE_FROM_ALL)

target_link_libraries(smp cjson)
target_link_libraries(smp portaudio)
target_link_libraries(smp sndfile)
target_link_libraries(smp CURL::libcurl)
target_link_libraries(smp dbus-1)
target_link_libraries(smp pthread)
target_link_libraries(smp confuse)
