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

if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
    add_compile_options(-Wall -Wextra -Wpedantic -Wformat=2 -Wno-missing-field-initializers)
endif()

option(DICONS_SANITIZERS "Instrument dicons with ASAN and UBSAN" OFF)
if(DICONS_SANITIZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
        message(FATAL_ERROR "DICONS_SANITIZERS requires GCC or Clang")
    endif()
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

add_library(dicons-core STATIC
    src/desktop_directory.cpp
    src/desktop_item.cpp
    src/symlink.cpp
    src/desktop_sort.cpp
    src/desktop_windows.cpp
    src/icon_grid.cpp
    src/item_icon.cpp
    src/thumbnail_cache.cpp
    src/thumbnailer.cpp
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
target_include_directories(dicons-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(dicons-core PUBLIC PkgConfig::DICONS_DEPS)

add_executable(dicons src/main.cpp)
target_link_libraries(dicons PRIVATE dicons-core)

include(CTest)
if(BUILD_TESTING)
    function(dicons_test name source)
        add_executable(test-${name} tests/${source})
        target_link_libraries(test-${name} PRIVATE dicons-core)
        add_test(NAME desktop-${name} COMMAND test-${name})
    endfunction()
    dicons_test(copy copy.cpp)
    dicons_test(rename rename.cpp)
    dicons_test(directory desktop_directory.cpp)
    dicons_test(thumbnails thumbnails.cpp)
endif()

install(TARGETS dicons RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES dicons.desktop DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
install(FILES LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/dicons")
