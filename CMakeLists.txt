cmake_minimum_required(VERSION 3.16)
project(dicons LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(DICONS_DEPS REQUIRED IMPORTED_TARGET gtk4-layer-shell-0 gtk4>=4.12 gio-unix-2.0>=2.74)

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

find_program(GLIB_COMPILE_SCHEMAS glib-compile-schemas)
if(NOT GLIB_COMPILE_SCHEMAS)
    message(FATAL_ERROR "glib-compile-schemas is required to build dicons")
endif()
set(dicons_app_id dev.maydayv7.dicons)
set(schema_name "${dicons_app_id}.gschema.xml")
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gschemas.compiled"
    COMMAND "${GLIB_COMPILE_SCHEMAS}" --strict "--targetdir=${CMAKE_CURRENT_BINARY_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/data"
    DEPENDS "data/${schema_name}"
    VERBATIM
)
add_custom_target(dicons-schemas DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/gschemas.compiled")

add_library(
    dicons-core STATIC
    src/preferences.cpp
    src/desktop_directory.cpp
    src/desktop_item.cpp
    src/symlink.cpp
    src/desktop_sort.cpp
    src/desktop_windows.cpp
    src/grid_layout.cpp
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
    src/trash_icon.cpp
    src/trash_file.cpp
    src/trash_actions.cpp
    src/undo.cpp
    src/transfer.cpp
    src/clipboard.cpp
    src/copy.cpp
)
add_dependencies(dicons-core dicons-schemas)
target_include_directories(dicons-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(dicons-core PUBLIC PkgConfig::DICONS_DEPS)

find_program(GLIB_COMPILE_RESOURCES glib-compile-resources)
if(NOT GLIB_COMPILE_RESOURCES)
    message(FATAL_ERROR "glib-compile-resources is required to build dicons")
endif()
add_custom_command(
    OUTPUT dicons-resources.c
    COMMAND "${GLIB_COMPILE_RESOURCES}" --generate-source "--target=${CMAKE_CURRENT_BINARY_DIR}/dicons-resources.c"
            "--sourcedir=${CMAKE_CURRENT_SOURCE_DIR}/data" "${CMAKE_CURRENT_SOURCE_DIR}/data/dicons.gresource.xml"
    DEPENDS data/dicons.gresource.xml data/style.css
    VERBATIM
)
add_executable(dicons src/main.cpp dicons-resources.c)
target_link_libraries(dicons PRIVATE dicons-core)

include(CTest)
if(BUILD_TESTING)
    function(dicons_test name source)
        add_executable(test-${name} tests/${source})
        target_link_libraries(test-${name} PRIVATE dicons-core)
        add_test(NAME desktop-${name} COMMAND test-${name})
    endfunction()
    dicons_test(copy copy.cpp)
    dicons_test(clipboard clipboard.cpp)
    dicons_test(rename rename.cpp)
    dicons_test(folder-drop folder_drop.cpp)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_options(test-folder-drop PRIVATE "LINKER:--wrap=g_file_copy")
    endif()
    dicons_test(launch launch.cpp)
    dicons_test(directory desktop_directory.cpp)
    set_tests_properties(desktop-directory PROPERTIES ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${CMAKE_CURRENT_BINARY_DIR}")
    dicons_test(thumbnails thumbnails.cpp)
    dicons_test(trash trash.cpp)
    dicons_test(undo undo.cpp)
    set_tests_properties(desktop-undo PROPERTIES ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${CMAKE_CURRENT_BINARY_DIR}")
    dicons_test(file-actions file_actions.cpp)
    target_sources(test-file-actions PRIVATE dicons-resources.c)
    set_tests_properties(desktop-file-actions PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${CMAKE_CURRENT_BINARY_DIR}")
endif()

install(TARGETS dicons RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(
    FILES data/dicons.desktop
    DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
    RENAME "${dicons_app_id}.desktop"
)
install(
    FILES data/dicons.svg
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
    RENAME "${dicons_app_id}.svg"
)
install(FILES LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/dicons")

set(unit_bindir "${CMAKE_INSTALL_BINDIR}")
if(NOT IS_ABSOLUTE "${unit_bindir}")
    set(unit_bindir "\${CMAKE_INSTALL_PREFIX}/${unit_bindir}")
endif()
install(
    CODE "
    set(dicons_executable \"${unit_bindir}/dicons\")
    set(session_target graphical-session.target)
    configure_file(\"${CMAKE_CURRENT_SOURCE_DIR}/data/dicons.service.in\"
                   \"${CMAKE_CURRENT_BINARY_DIR}/dicons.service\" @ONLY)
"
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/dicons.service" DESTINATION "${CMAKE_INSTALL_DATADIR}/systemd/user")

set(schema_dir "${CMAKE_INSTALL_DATADIR}/glib-2.0/schemas")
install(FILES "data/${schema_name}" DESTINATION "${schema_dir}")
if(NOT IS_ABSOLUTE "${schema_dir}")
    set(schema_dir "\${CMAKE_INSTALL_PREFIX}/${schema_dir}")
endif()
install(
    CODE "
    execute_process(COMMAND \"${GLIB_COMPILE_SCHEMAS}\" \"\$ENV{DESTDIR}${schema_dir}\"
                    RESULT_VARIABLE status)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR \"Could not compile installed GSettings schemas\")
    endif()
"
)
