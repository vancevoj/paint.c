# PcPackaging.cmake - lane I: application icon and version resources,
# the macOS bundle resources, install rules and CPack settings.
#
# Included at the end of src/app/CMakeLists.txt (the paintc target exists).
# docs/PACKAGING.md describes the packages built from these rules:
#   Linux    cmake --install: bin/paintc, the .desktop file, the MIME type
#            of .pdn, AppStream metadata, hicolor icons, licenses; the
#            AppImage (packaging/linux/build-appimage.sh) and the Flatpak
#            manifest (packaging/flatpak) build on top of it.
#   Windows  cmake --install: paintc.exe + licenses (the portable zip); the
#            NSIS installer (packaging/windows/paintc.nsi) registers the
#            file associations.
#   macOS    cmake --install: paint.c.app with the icon and the licenses in
#            Contents/Resources; packaging/macos/make-dmg.sh signs it ad hoc
#            and builds the .dmg.

set(PC_PKG_DIR ${PROJECT_SOURCE_DIR}/packaging)
set(PC_ICON_DIR ${PROJECT_SOURCE_DIR}/assets/icons)
set(PC_LICENSE_FILES
  ${PROJECT_SOURCE_DIR}/LICENSE
  ${PROJECT_SOURCE_DIR}/NOTICE)
file(GLOB PC_THIRD_PARTY_LICENSES ${PC_PKG_DIR}/licenses/*)

# ---- Windows: icon and version resources ---------------------------------------
if(WIN32)
  set(PC_RC_ICON ${PC_ICON_DIR}/paintc.ico)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/platform/paintc_version.rc.in
                 ${CMAKE_CURRENT_BINARY_DIR}/paintc_version.rc @ONLY)
  enable_language(RC)
  target_sources(paintc PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/paintc_version.rc)
  set_property(SOURCE ${CMAKE_CURRENT_BINARY_DIR}/paintc_version.rc APPEND PROPERTY
               OBJECT_DEPENDS ${PC_RC_ICON})
endif()

# ---- macOS: icon and licenses inside the bundle ----------------------------------
if(APPLE)
  set(_icns ${PC_ICON_DIR}/paintc.icns)
  target_sources(paintc PRIVATE ${_icns})
  set_source_files_properties(${_icns} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  set_target_properties(paintc PROPERTIES MACOSX_BUNDLE_ICON_FILE paintc.icns)
  # src/app/CMakeLists.txt copies paintc.app to paint.c.app after linking;
  # this runs after that copy and adds the license texts to the copy.
  set(_res ${CMAKE_BINARY_DIR}/paint.c.app/Contents/Resources/licenses)
  add_custom_command(TARGET paintc POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_res}
    COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_SOURCE_DIR}/LICENSE ${_res}/LICENSE.txt
    COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_SOURCE_DIR}/NOTICE ${_res}/NOTICE.txt
    COMMAND ${CMAKE_COMMAND} -E copy ${PC_THIRD_PARTY_LICENSES} ${_res}
    VERBATIM)
endif()

# ---- install rules ----------------------------------------------------------------------
include(GNUInstallDirs)
if(APPLE)
  install(DIRECTORY ${CMAKE_BINARY_DIR}/paint.c.app DESTINATION . USE_SOURCE_PERMISSIONS
          COMPONENT paintc)
elseif(WIN32)
  install(TARGETS paintc RUNTIME DESTINATION . COMPONENT paintc)
  install(FILES ${PROJECT_SOURCE_DIR}/LICENSE DESTINATION licenses RENAME LICENSE.txt
          COMPONENT paintc)
  install(FILES ${PROJECT_SOURCE_DIR}/NOTICE DESTINATION licenses RENAME NOTICE.txt
          COMPONENT paintc)
  install(FILES ${PC_THIRD_PARTY_LICENSES} DESTINATION licenses COMPONENT paintc)
  install(FILES ${PC_PKG_DIR}/windows/README-portable.txt DESTINATION . RENAME README.txt
          COMPONENT paintc)
else()
  set(_share ${CMAKE_INSTALL_DATADIR})
  install(TARGETS paintc RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT paintc)
  install(FILES ${PC_PKG_DIR}/linux/io.github.vancevoj.paintc.desktop
          DESTINATION ${_share}/applications COMPONENT paintc)
  install(FILES ${PC_PKG_DIR}/linux/io.github.vancevoj.paintc.xml
          DESTINATION ${_share}/mime/packages COMPONENT paintc)
  install(FILES ${PC_PKG_DIR}/linux/io.github.vancevoj.paintc.metainfo.xml
          DESTINATION ${_share}/metainfo COMPONENT paintc)
  foreach(_s 16 24 32 48 64 128 256 512)
    install(FILES ${PC_ICON_DIR}/png/paintc-${_s}.png
            DESTINATION ${_share}/icons/hicolor/${_s}x${_s}/apps
            RENAME io.github.vancevoj.paintc.png COMPONENT paintc)
  endforeach()
  install(FILES ${PC_ICON_DIR}/paintc.svg DESTINATION ${_share}/icons/hicolor/scalable/apps
          RENAME io.github.vancevoj.paintc.svg COMPONENT paintc)
  install(FILES ${PROJECT_SOURCE_DIR}/LICENSE DESTINATION ${_share}/doc/paintc/licenses
          RENAME LICENSE.txt COMPONENT paintc)
  install(FILES ${PROJECT_SOURCE_DIR}/NOTICE DESTINATION ${_share}/doc/paintc/licenses
          RENAME NOTICE.txt COMPONENT paintc)
  install(FILES ${PC_THIRD_PARTY_LICENSES} DESTINATION ${_share}/doc/paintc/licenses
          COMPONENT paintc)
endif()

# ---- CPack: plain archives of the install tree (the installers and images
# are built by the scripts in packaging/, see docs/PACKAGING.md) -------------
set(CPACK_PACKAGE_NAME "paintc")
set(CPACK_PACKAGE_VENDOR "The paint.c authors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "paint.c image editor")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_RESOURCE_FILE_LICENSE ${PROJECT_SOURCE_DIR}/LICENSE)
set(CPACK_PACKAGE_INSTALL_DIRECTORY "paint.c")
set(CPACK_COMPONENTS_ALL paintc)
if(WIN32)
  set(CPACK_GENERATOR ZIP)
  set(CPACK_SYSTEM_NAME windows-x64)
elseif(APPLE)
  set(CPACK_GENERATOR ZIP)
  set(CPACK_SYSTEM_NAME macos)
else()
  set(CPACK_GENERATOR TGZ)
  set(CPACK_SYSTEM_NAME linux-${CMAKE_SYSTEM_PROCESSOR})
endif()
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
include(CPack)
