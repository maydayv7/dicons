set(CPACK_PACKAGE_NAME "dicons")
set(CPACK_PACKAGE_VERSION "${DICONS_PACKAGE_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Desktop icons for Wayland compositors with layer-shell support")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/maydayv7/dicons")
set(CPACK_PACKAGE_CONTACT "maydayv7 <maydayv7@gmail.com>")
set(CPACK_SET_DESTDIR ON)
set(CPACK_STRIP_FILES ON)

set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_RELEASE "1")
set(CPACK_DEBIAN_PACKAGE_SECTION "x11")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "xdg-terminal-exec")

set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-only")
set(CPACK_RPM_USER_FILELIST "%license /usr/share/licenses/dicons/LICENSE")
set(CPACK_RPM_PACKAGE_RELEASE_DIST ON)
set(CPACK_RPM_PACKAGE_RECOMMENDS "xdg-terminal-exec")
set(CPACK_RPM_PACKAGE_RELOCATABLE OFF)
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    /usr/lib/systemd
    /usr/lib/systemd/user
    /usr/share/applications
    /usr/share/glib-2.0
    /usr/share/glib-2.0/schemas
    /usr/share/icons
    /usr/share/icons/hicolor
    /usr/share/icons/hicolor/scalable
    /usr/share/icons/hicolor/scalable/apps
    /usr/share/licenses)
include(CPack)
