cmake_minimum_required(VERSION 3.16)
project(hs80tray)

set(CMAKE_CXX_STANDARD 17)
find_package(Qt6 COMPONENTS Widgets REQUIRED)
find_package(KF6StatusNotifierItem REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(HIDAPI REQUIRED hidapi-hidraw)
set(CMAKE_AUTOMOC ON)
add_executable(hs80tray main.cpp
    README.md
    tray.png)
target_include_directories(hs80tray PRIVATE ${HIDAPI_INCLUDE_DIRS})
target_link_libraries(hs80tray Qt6::DBus KF6::StatusNotifierItem Qt6::Widgets ${HIDAPI_LIBRARIES})

