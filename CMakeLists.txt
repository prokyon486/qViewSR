cmake_minimum_required(VERSION 3.16)

project(qView VERSION 7.0 LANGUAGES CXX)

# Optional test building
option(BUILD_TESTS "Build tests" OFF)
if(BUILD_TESTS)
    enable_testing()
endif()

# Generate a compile_commands.json file for clang-tidy and other tools
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Set the application type for platform-specific executables
if(WIN32)
    set(EXECUTABLE_TYPE WIN32)
elseif(APPLE)
    set(EXECUTABLE_TYPE MACOSX_BUNDLE)
endif()

# Qt 6 requires C++17; the isolated legacy worker remains C++14.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable Qt's automatic tools
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Find required Qt components
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Network Widgets Svg Concurrent)
set(QT_VERSION_MAJOR 6)
set(QT_VERSION ${Qt6_VERSION})


# Source files
set(PROJECT_SOURCES
    src/main.cpp
    src/mainwindow.cpp
    src/openwith.cpp
    src/qvgraphicsview.cpp
    src/qvoptionsdialog.cpp
    src/qvapplication.cpp
    src/qvaboutdialog.cpp
    src/qvrenamedialog.cpp
    src/qvwelcomedialog.cpp
    src/qvinfodialog.cpp
    src/qvimagecore.cpp
    src/qvshortcutdialog.cpp
    src/actionmanager.cpp
    src/settingsmanager.cpp
    src/shortcutmanager.cpp
    src/sr/color_pipeline.cpp
    src/sr/sr_controller.cpp
    src/sr/gif_export.cpp
)

# Header files
set(PROJECT_HEADERS
    src/mainwindow.h
    src/openwith.h
    src/qvgraphicsview.h
    src/qvoptionsdialog.h
    src/qvapplication.h
    src/qvaboutdialog.h
    src/qvrenamedialog.h
    src/qvwelcomedialog.h
    src/qvinfodialog.h
    src/qvimagecore.h
    src/qvshortcutdialog.h
    src/actionmanager.h
    src/settingsmanager.h
    src/shortcutmanager.h
    src/sr/color_pipeline.h
    src/sr/sr_controller.h
    src/sr/gif_export.h
)

# UI form files
set(PROJECT_FORMS
    src/mainwindow.ui
    src/qvopenwithdialog.ui
    src/qvoptionsdialog.ui
    src/qvaboutdialog.ui
    src/qvwelcomedialog.ui
    src/qvinfodialog.ui
    src/qvshortcutdialog.ui
)

# To disables both manual and automatic checking for updates
option(QV_DISABLE_ONLINE_VERSION_CHECK "Disable online version check" ON)
if(NOT QV_DISABLE_ONLINE_VERSION_CHECK)
    list(APPEND PROJECT_SOURCES src/updatechecker.cpp)
    list(APPEND PROJECT_HEADERS src/updatechecker.h)
endif()

# Platform-specific sources
# Add source files required for each operating system.
if(WIN32)
    # For Windows, add Win32-specific functions and headers.
    option(NO_WIN32 "Build without Win32 API" OFF)
    if(NOT NO_WIN32)
        list(APPEND PROJECT_SOURCES src/qvwin32functions.cpp)
        list(APPEND PROJECT_HEADERS src/qvwin32functions.h)
    endif()
elseif(APPLE)
    # For macOS, add Cocoa-specific functions and headers.
    option(NO_COCOA "Build without Cocoa framework" OFF)
    if(NOT NO_COCOA)
        list(APPEND PROJECT_SOURCES src/qvcocoafunctions.mm)
        list(APPEND PROJECT_HEADERS src/qvcocoafunctions.h)
    endif()
elseif(UNIX AND NOT APPLE)
    # For Linux, add X11-specific functions and headers.
    option(NO_X11 "Build without X11" OFF)
    if(NOT NO_X11)
        list(APPEND PROJECT_SOURCES src/qvlinuxx11functions.cpp)
        list(APPEND PROJECT_HEADERS src/qvlinuxx11functions.h)
    endif()
endif()

# Add the executable and link Qt libraries
set(PROJECT_RESOURCES resources/resources.qrc)
if(WIN32)
    set(ICON_PATH_RAW "${CMAKE_CURRENT_SOURCE_DIR}/dist/win/qView.ico")
    string(REPLACE "\\" "\\\\" ICON_PATH "${ICON_PATH_RAW}")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/dist/win/qView.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/qView.rc"
    )
    list(APPEND PROJECT_RESOURCES "${CMAKE_CURRENT_BINARY_DIR}/qView.rc")
endif()
add_executable(${PROJECT_NAME} ${EXECUTABLE_TYPE} ${PROJECT_SOURCES} ${PROJECT_HEADERS} ${PROJECT_FORMS} ${PROJECT_RESOURCES})

target_link_libraries(${PROJECT_NAME} PRIVATE Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Network Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Svg)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LCMS2 REQUIRED IMPORTED_TARGET lcms2)
find_package(ZLIB REQUIRED)
find_package(GIF 5.1 REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::Concurrent PkgConfig::LCMS2 ZLIB::ZLIB GIF::GIF)
target_include_directories(${PROJECT_NAME} PRIVATE src)

# Set the executable name to lowercase on Linux
if(UNIX AND NOT APPLE)
    set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "qviewsr")
endif()

# Add compile definitions based on options
if(QV_DISABLE_ONLINE_VERSION_CHECK)
    target_compile_definitions(${PROJECT_NAME} PRIVATE QV_DISABLE_ONLINE_VERSION_CHECK)
endif()

# Platform-specific configurations
# Apply settings, link libraries, and define properties for each platform.
if(WIN32)
    # Windows specific: link to Win32 libraries, set icon, and define as a Windows executable.
    if(NOT NO_WIN32)
        target_compile_definitions(${PROJECT_NAME} PRIVATE WIN32_LOADED)
        set(PLATFORM_SPECIFIC_LIBS shell32 user32 ole32 shlwapi gdi32)
        target_link_libraries(${PROJECT_NAME} PRIVATE ${PLATFORM_SPECIFIC_LIBS})
    endif()
elseif(APPLE)
    # macOS specific: link to Cocoa framework and configure the app bundle.
    if(NOT NO_COCOA)
        target_compile_definitions(${PROJECT_NAME} PRIVATE COCOA_LOADED)
        set(PLATFORM_SPECIFIC_LIBS "-framework Cocoa")
        target_link_libraries(${PROJECT_NAME} PRIVATE ${PLATFORM_SPECIFIC_LIBS})
    endif()

    # Set the icon based on the Qt version, as per the Qt documentation.
    if(QT_VERSION_MAJOR LESS 6)
        set(MACOSX_BUNDLE_ICON_FILE "qView_legacy.icns")
    else()
        set(MACOSX_BUNDLE_ICON_FILE "qView.icns")
    endif()

    set(app_icon_macos "${CMAKE_CURRENT_SOURCE_DIR}/dist/mac/${MACOSX_BUNDLE_ICON_FILE}")
    set_source_files_properties(${app_icon_macos} PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )
    target_sources(${PROJECT_NAME} PRIVATE ${app_icon_macos})

    # The Info.plist.in file contains placeholders that will be replaced by
    # CMake. We set MACOSX_ICON_FILENAME here so that configure_file can
    # substitute it in the plist.
    set(MACOSX_ICON_FILENAME ${MACOSX_BUNDLE_ICON_FILE})
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/dist/mac/Info.plist.in"
        "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
        @ONLY
    )

    # Set bundle properties. MACOSX_BUNDLE_INFO_PLIST tells CMake to use our
    # generated Info.plist instead of the default one.
    set_target_properties(${PROJECT_NAME} PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
    )
elseif(UNIX AND NOT APPLE)
    if(NOT NO_X11)
        find_package(X11 REQUIRED)
        target_compile_definitions(${PROJECT_NAME} PRIVATE X11_LOADED)
        set(PLATFORM_SPECIFIC_LIBS ${X11_LIBRARIES})
        if(QT_VERSION_MAJOR EQUAL 5)
            list(APPEND PLATFORM_SPECIFIC_LIBS Qt5::X11Extras)
        endif()
        target_link_libraries(${PROJECT_NAME} PRIVATE ${PLATFORM_SPECIFIC_LIBS})
    endif()
endif()

# Compile definitions
target_compile_definitions(${PROJECT_NAME} PRIVATE
    "VERSION=${PROJECT_VERSION}"
    QT_DEPRECATED_WARNINGS
    QT_NO_FOREACH
)

# Check for nightly builds, which are identified by the NIGHTLY preprocessor definition.
if(DEFINED NIGHTLY)
    message(STATUS "This is nightly ${NIGHTLY}")
    target_compile_definitions(${PROJECT_NAME} PRIVATE "NIGHTLY=\"${NIGHTLY}\"")
endif()

# Bundle translations explicitly. Missing LinguistTools must not silently produce
# an English-only app; lrelease is provided by qt6-l10n-tools on Ubuntu.
find_program(QVIEWSR_LRELEASE NAMES lrelease lrelease-qt6 HINTS /usr/lib/qt6/bin REQUIRED)
find_file(QVIEWSR_QTBASE_JA qtbase_ja.qm HINTS /usr/share/qt6/translations REQUIRED)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/i18n")
file(GLOB TRANSLATIONS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/i18n/qview_*.ts")
set(TRANSLATION_QRC "${CMAKE_CURRENT_BINARY_DIR}/qviewsr_i18n.qrc")
file(WRITE "${TRANSLATION_QRC}" "<RCC><qresource prefix=\"/i18n\">\n")
set(QM_FILES)
foreach(ts IN LISTS TRANSLATIONS)
    get_filename_component(name "${ts}" NAME_WE)
    set(qm "${CMAKE_CURRENT_BINARY_DIR}/i18n/${name}.qm")
    add_custom_command(OUTPUT "${qm}" COMMAND "${QVIEWSR_LRELEASE}" -silent "${ts}" -qm "${qm}" DEPENDS "${ts}" VERBATIM)
    list(APPEND QM_FILES "${qm}")
    file(APPEND "${TRANSLATION_QRC}" "<file alias=\"${name}.qm\">${qm}</file>\n")
endforeach()
file(APPEND "${TRANSLATION_QRC}" "<file alias=\"qtbase_ja.qm\">${QVIEWSR_QTBASE_JA}</file>\n</qresource></RCC>\n")
add_library(qviewsr_i18n OBJECT "${TRANSLATION_QRC}" ${QM_FILES})
target_link_libraries(qviewsr_i18n PRIVATE Qt6::Core)
target_link_libraries(${PROJECT_NAME} PRIVATE qviewsr_i18n)

# Installation rules
include(GNUInstallDirs)
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    BUNDLE DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES dist/linux/com.interversehq.qView.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(DIRECTORY dist/linux/hicolor/ DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/qview)
install(FILES dist/linux/com.interversehq.qView.appdata.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)

# Include tests if requested
if(BUILD_TESTS)
    add_subdirectory(tests)
endif()
