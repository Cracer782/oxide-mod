 cmake_minimum_required(VERSION 3.18.1)
project(modmenu LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(
    modmenu
    SHARED
    main.cpp
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/imgui_impl_opengl3.cpp
    imgui/imgui_impl_android.cpp
)

target_include_directories(modmenu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/imgui)

find_library(log-lib log)
find_library(gles-lib GLESv3)
find_library(android-lib android)

target_link_libraries(
    modmenu
    ${log-lib}
    ${gles-lib}
    ${android-lib}
    EGL
    GLESv3
)
