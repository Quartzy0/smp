cmake_minimum_required(VERSION 3.16.0)

project(smp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)

include_directories(/usr/include/dbus-1.0)
include_directories(/usr/lib/x86_64-linux-gnu/dbus-1.0/include)
include_directories(/usr/lib/dbus-1.0/include)
include_directories(include)

option(NO_PIPEWIRE "Use PipeWire as backend" OFF)

file(GLOB smp_SRC CONFIGURE_DEPENDS "src/*.h" "src/*.c")

add_executable(smp ${smp_SRC})
target_compile_options(smp PRIVATE "-march=native")

find_package(PipeWire QUIET)
if (NOT NO_PIPEWIRE AND PipeWire_FOUND)
    message("PipeWire found, using PipeWire backend")
    target_link_libraries(smp ${PipeWire_LIBRARIES})
    target_include_directories(smp SYSTEM PRIVATE ${PipeWire_INCLUDE_DIRS} ${PKG_Spa_INCLUDE_DIRS})
    target_compile_definitions(smp PUBLIC PIPEWIRE_BACKEND)
else()
    if (PipeWire_FOUND)
        message(WARNING "Using portaudio backend despite PipeWire being found")
    else()
        message("PipeWire wasn't found. Falling back to portaudio backend")
    endif ()
    target_link_libraries(smp portaudio)
endif ()

set(CJSON_OVERRIDE_BUILD_SHARED_LIBS OFF)
set(CJSON_BUILD_SHARED_LIBS OFF)
set(BUILD_SHARED_LIBS OFF)
add_subdirectory(lib/cjson EXCLUDE_FROM_ALL)

set(DBUS_UTIL_BUILD_SHARED OFF)
add_subdirectory(lib/dbus-util)

include(cmake/FileEmbed.cmake)
FileEmbedSetup()
FileEmbedAdd(${CMAKE_SOURCE_DIR}/src/introspection.xml)

target_link_libraries(smp file_embed)
target_link_libraries(smp dbus_util)
target_link_libraries(smp cjson)
target_link_libraries(smp dbus-1)
target_link_libraries(smp pthread)
target_link_libraries(smp m)
target_link_libraries(smp event)
target_link_libraries(smp vorbis)
target_link_libraries(smp ogg)
