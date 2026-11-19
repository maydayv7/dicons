cmake_minimum_required(VERSION 3.16)
project(dicons LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(DICONS_DEPS REQUIRED IMPORTED_TARGET
    gtk4-layer-shell-0
    gtk4>=4.12
    gio-unix-2.0>=2.74
)

add_executable(dicons
    src/main.cpp
    src/desktop_directory.cpp
    src/desktop_item.cpp
    src/desktop_sort.cpp
    src/desktop_windows.cpp
    src/icon_grid.cpp
    src/grid_selection.cpp
    src/selection_actions.cpp
    src/context_menu.cpp
    src/create_item.cpp
    src/copy_stage.cpp
    src/templates.cpp
    src/keyboard.cpp
    src/inline_rename.cpp
    src/file_rename.cpp
    src/launch.cpp
    src/notifications.cpp
    src/collision_dialog.cpp
    src/transfer.cpp
    src/copy.cpp
)
target_link_libraries(dicons PRIVATE PkgConfig::DICONS_DEPS)

include(CTest)
if(BUILD_TESTING)
    add_executable(test-copy tests/copy.cpp src/copy.cpp src/file_rename.cpp)
    target_include_directories(test-copy PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(test-copy PRIVATE PkgConfig::DICONS_DEPS)
    add_test(NAME desktop-copy COMMAND test-copy)

    add_executable(test-rename tests/rename.cpp src/file_rename.cpp)
    target_include_directories(test-rename PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(test-rename PRIVATE PkgConfig::DICONS_DEPS)
    add_test(NAME desktop-rename COMMAND test-rename)

    add_executable(test-directory tests/desktop_directory.cpp src/desktop_directory.cpp src/desktop_item.cpp src/desktop_sort.cpp src/notifications.cpp)
    target_include_directories(test-directory PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(test-directory PRIVATE PkgConfig::DICONS_DEPS)
    add_test(NAME desktop-directory COMMAND test-directory)
endif()

install(TARGETS dicons RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES dicons.desktop DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
install(FILES LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/dicons")
