cmake_minimum_required(VERSION 3.16)
project(dicons LANGUAGES C)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(DICONS_DEPS REQUIRED IMPORTED_TARGET
    gtk+-3.0
    gio-unix-2.0
    gtk-layer-shell-0
)

add_executable(dicons dicons.c)
target_compile_features(dicons PRIVATE c_std_99)
target_link_libraries(dicons PRIVATE PkgConfig::DICONS_DEPS)

install(TARGETS dicons RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/dicons")
