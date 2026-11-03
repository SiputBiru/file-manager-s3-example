cmake_minimum_required(VERSION 3.20)
project(gui-raygui C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(APPLE)
    list(APPEND CMAKE_PREFIX_PATH
        /opt/homebrew/opt/raylib
        /opt/homebrew/opt/curl
        /usr/local/opt/raylib
        /usr/local/opt/curl)
endif()

find_package(raylib CONFIG REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

add_executable(gui-raygui
    src/main.c
    src/ui.c
    src/api.c
    src/image_cache.c
    src/raygui_impl.c
    src/stb_impl.c
    vendor/cJSON.c
    vendor/tinyfiledialogs.c)

target_include_directories(gui-raygui PRIVATE src vendor)

target_link_libraries(gui-raygui PRIVATE
    raylib
    CURL::libcurl
    Threads::Threads
    m)

if(APPLE)
    target_link_libraries(gui-raygui PRIVATE
        "-framework Cocoa"
        "-framework IOKit"
        "-framework CoreVideo")
elseif(UNIX)
    target_link_libraries(gui-raygui PRIVATE dl)
endif()
