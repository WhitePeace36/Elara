# Installation and packages: the loader, its systemd unit, and CPack settings for
# Debian/Ubuntu (.deb) and Fedora (.rpm) packages. Arch Linux uses the PKGBUILD in
# the repository root, which builds and installs through the same rules.

include(GNUInstallDirs)

# The unit starts the installed binary, wherever the prefix put it.
configure_file(${CMAKE_CURRENT_LIST_DIR}/elara.service.in ${CMAKE_BINARY_DIR}/elara.service @ONLY)
# lib, not CMAKE_INSTALL_LIBDIR (lib64 on Fedora): systemd looks for units in
# <prefix>/lib/systemd/system.
install(FILES ${CMAKE_BINARY_DIR}/elara.service DESTINATION lib/systemd/system)
# What the package scripts of all formats run on install, upgrade and remove
# (the scripts name it by this path: packages install to /usr).
install(PROGRAMS ${CMAKE_CURRENT_LIST_DIR}/elara-hook.sh DESTINATION lib/elara RENAME elara-hook)

set(CPACK_PACKAGE_NAME elara)
set(CPACK_PACKAGE_VERSION ${ELARA_VERSION})
set(CPACK_PACKAGE_VENDOR "Timon Stipkovits")
set(CPACK_PACKAGE_CONTACT "Timon Stipkovits <timon2201@gmail.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "sched_ext CPU scheduler, tuned by nice values, for stable frame times")
set(CPACK_PACKAGE_DESCRIPTION "elara is a sched_ext CPU scheduler: nice values decide how soon and how much a task runs, woken chains keep moving with wake boosts, and tasks stay near their caches. Needs Linux 6.18 or newer with BTF.")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/WhitePeace36/Elara")
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_SOURCE_DIR}/LICENSE)
set(CPACK_STRIP_FILES ON)
# The packages land in the repository root, like the one of the PKGBUILD; what
# CPack needs on the way (_CPack_Packages) stays in the build directory.
set(CPACK_PACKAGE_DIRECTORY ${CMAKE_BINARY_DIR})
set(CPACK_OUTPUT_FILE_PREFIX ${CMAKE_SOURCE_DIR})
# The paths in the package are the configured ones (the unit names the binary).
set(CPACK_PACKAGING_INSTALL_PREFIX ${CMAKE_INSTALL_PREFIX})

# Plain `cpack` builds the package of the system it runs on.
if(EXISTS /etc/debian_version)
  set(CPACK_GENERATOR DEB)
elseif(EXISTS /etc/fedora-release OR EXISTS /etc/redhat-release)
  set(CPACK_GENERATOR RPM)
else()
  set(CPACK_GENERATOR "DEB;RPM")
endif()

# Debian / Ubuntu: the library dependencies (libbpf, libc, libstdc++) come from
# dpkg-shlibdeps.
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_RELEASE 1)
set(CPACK_DEBIAN_PACKAGE_SECTION admin)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    ${CMAKE_CURRENT_LIST_DIR}/deb/postinst
    ${CMAKE_CURRENT_LIST_DIR}/deb/prerm
    ${CMAKE_CURRENT_LIST_DIR}/deb/postrm)
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)

# Fedora: the library dependencies come from rpmbuild (automatic requires).
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_RELEASE 1)
set(CPACK_RPM_PACKAGE_RELEASE_DIST ON)
set(CPACK_RPM_PACKAGE_LICENSE "GPL-2.0-only")
set(CPACK_RPM_PACKAGE_URL ${CPACK_PACKAGE_HOMEPAGE_URL})
set(CPACK_RPM_PACKAGE_DESCRIPTION ${CPACK_PACKAGE_DESCRIPTION})
# The unit names the binary by its path: the package can't be moved elsewhere.
set(CPACK_PACKAGE_RELOCATABLE OFF)
set(CPACK_RPM_PACKAGE_RELOCATABLE OFF)
set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE ${CMAKE_CURRENT_LIST_DIR}/rpm/post.sh)
set(CPACK_RPM_PRE_UNINSTALL_SCRIPT_FILE ${CMAKE_CURRENT_LIST_DIR}/rpm/preun.sh)
set(CPACK_RPM_POST_UNINSTALL_SCRIPT_FILE ${CMAKE_CURRENT_LIST_DIR}/rpm/postun.sh)
# Owned by systemd, not by this package.
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    ${CMAKE_INSTALL_PREFIX}/lib/systemd
    ${CMAKE_INSTALL_PREFIX}/lib/systemd/system)

include(CPack)
