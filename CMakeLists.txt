cmake_minimum_required(VERSION 3.18.1)
project(iptade LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -s -fvisibility=hidden -fvisibility-inlines-hidden -flto")
set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "-Wl,--exclude-libs,ALL -Wl,-s -flto")

add_library(dobby STATIC IMPORTED)
set_target_properties(dobby PROPERTIES IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/dobby/${ANDROID_ABI}/libdobby.a)

add_library(
    iptade
    SHARED
    main.cpp
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/imgui_impl_android.cpp
    imgui/imgui_impl_vulkan.cpp
)

target_include_directories(iptade PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/dobby
)

find_library(log-lib log)
find_library(android-lib android)
find_library(dl-lib dl)
find_library(vulkan-lib vulkan)

target_link_libraries(
    iptade
    ${log-lib}
    ${android-lib}
    ${dl-lib}
    ${vulkan-lib}
    dobby
)
