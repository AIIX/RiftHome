# SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.25)
project(RiftHome VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui DBus Test)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WLROOTS REQUIRED IMPORTED_TARGET wlroots-0.20)
pkg_check_modules(WAYLAND_SERVER REQUIRED IMPORTED_TARGET wayland-server)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)
pkg_check_modules(PIXMAN REQUIRED IMPORTED_TARGET pixman-1)
# Enum headers (<wayland-protocols/*-enum.h>) used by wlroots and by the colour model.
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED IMPORTED_TARGET wayland-protocols>=1.49)
pkg_check_modules(OPENXR REQUIRED IMPORTED_TARGET openxr)
pkg_check_modules(VULKAN REQUIRED IMPORTED_TARGET vulkan)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_get_variable(WAYLAND_SCANNER wayland-scanner wayland_scanner)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)

# Client-side protocol code for the test tools.
function(rift_client_protocol name xml)
    set(out ${CMAKE_BINARY_DIR}/protocols)
    file(MAKE_DIRECTORY ${out})
    add_custom_command(
        OUTPUT ${out}/${name}-client-protocol.h ${out}/${name}-protocol.c
        COMMAND ${WAYLAND_SCANNER} client-header ${xml} ${out}/${name}-client-protocol.h
        COMMAND ${WAYLAND_SCANNER} private-code ${xml} ${out}/${name}-protocol.c
        DEPENDS ${xml})
endfunction()
# Server-side code for the protocols Rift Home implements itself (color-management-v1).
set(COLOR_MANAGEMENT_XML ${WAYLAND_PROTOCOLS_DIR}/staging/color-management/color-management-v1.xml)
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/protocols/color-management-v1-server-protocol.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/protocols
    COMMAND ${WAYLAND_SCANNER} server-header ${COLOR_MANAGEMENT_XML} ${CMAKE_BINARY_DIR}/protocols/color-management-v1-server-protocol.h
    DEPENDS ${COLOR_MANAGEMENT_XML}
    VERBATIM)
rift_client_protocol(xdg-shell ${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml)
rift_client_protocol(color-management-v1 ${WAYLAND_PROTOCOLS_DIR}/staging/color-management/color-management-v1.xml)
# linux-dmabuf, Rift Home's own (P010 HDR video, a modifier for each plane): server header, and the code from the rule above.
set(LINUX_DMABUF_XML ${WAYLAND_PROTOCOLS_DIR}/stable/linux-dmabuf/linux-dmabuf-v1.xml)
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/protocols/linux-dmabuf-v1-server-protocol.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/protocols
    COMMAND ${WAYLAND_SCANNER} server-header ${LINUX_DMABUF_XML} ${CMAKE_BINARY_DIR}/protocols/linux-dmabuf-v1-server-protocol.h
    DEPENDS ${LINUX_DMABUF_XML}
    VERBATIM)
rift_client_protocol(linux-dmabuf-v1 ${LINUX_DMABUF_XML})
rift_client_protocol(pointer-constraints-unstable-v1 ${WAYLAND_PROTOCOLS_DIR}/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml)
rift_client_protocol(relative-pointer-unstable-v1 ${WAYLAND_PROTOCOLS_DIR}/unstable/relative-pointer/relative-pointer-unstable-v1.xml)

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# Colour model: no wlroots, no OpenXR; tested on its own.
add_library(rift_color STATIC
    src/color/ColorDescription.cpp
    src/color/Primaries.cpp
    src/graphics/PixelFormat.cpp
    src/graphics/RadianceHdr.cpp
)
target_include_directories(rift_color PUBLIC src)
target_link_libraries(rift_color PUBLIC Qt6::Core PkgConfig::WAYLAND_PROTOCOLS PkgConfig::LIBDRM)

# Windows in the room: pure geometry and placement rules, tested on their own.
add_library(rift_spatial STATIC src/spatial/SpatialLayout.cpp src/spatial/WindowFrame.cpp src/spatial/VirtualKeyboard.cpp src/spatial/Controls.cpp)
target_include_directories(rift_spatial PUBLIC src)
target_link_libraries(rift_spatial PUBLIC Qt6::Core)

# The handoff between the Wayland and XR threads: plain C++, no wlroots, no OpenXR.
add_library(rift_bridge STATIC src/xr/LayerBridge.cpp)
target_link_libraries(rift_bridge PUBLIC rift_color Qt6::Core Qt6::Gui)

# The window pass (src/xr/shaders/window.comp) as SPIR-V words included by XrCompositor.cpp: one for 8-bit
# swapchains, one for 16-bit float ones.
find_program(GLSLC glslc REQUIRED)
set(SHADER_OUT ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT})
set(WINDOW_SHADER ${CMAKE_SOURCE_DIR}/src/xr/shaders/window.comp)
add_custom_command(
    OUTPUT ${SHADER_OUT}/window_rgba8.spv.h ${SHADER_OUT}/window_rgba16f.spv.h
    COMMAND ${GLSLC} -fshader-stage=comp -mfmt=c -O -o ${SHADER_OUT}/window_rgba8.spv.h ${WINDOW_SHADER}
    COMMAND ${GLSLC} -fshader-stage=comp -mfmt=c -O -DOUTPUT_FLOAT -o ${SHADER_OUT}/window_rgba16f.spv.h ${WINDOW_SHADER}
    DEPENDS ${WINDOW_SHADER}
    COMMENT "Compiling the window pass shader")
# Sharp windows near and far: the area-filtered smaller copies and the enlarged ones, 8-bit and float.
set(RESAMPLE_SHADER ${CMAKE_SOURCE_DIR}/src/xr/shaders/resample.comp)
add_custom_command(
    OUTPUT ${SHADER_OUT}/resample_rgba8.spv.h ${SHADER_OUT}/resample_rgba16f.spv.h
    COMMAND ${GLSLC} -fshader-stage=comp -mfmt=c -O -o ${SHADER_OUT}/resample_rgba8.spv.h ${RESAMPLE_SHADER}
    COMMAND ${GLSLC} -fshader-stage=comp -mfmt=c -O -DOUTPUT_FLOAT -o ${SHADER_OUT}/resample_rgba16f.spv.h ${RESAMPLE_SHADER}
    DEPENDS ${RESAMPLE_SHADER}
    COMMENT "Compiling the resample shader")
# The controllers in the room (Rift Home's own models): a vertex and a fragment shader.
add_custom_command(
    OUTPUT ${SHADER_OUT}/controller_vert.spv.h ${SHADER_OUT}/controller_frag.spv.h
    COMMAND ${GLSLC} -fshader-stage=vert -mfmt=c -O -o ${SHADER_OUT}/controller_vert.spv.h ${CMAKE_SOURCE_DIR}/src/xr/shaders/controller.vert
    COMMAND ${GLSLC} -fshader-stage=frag -mfmt=c -O -o ${SHADER_OUT}/controller_frag.spv.h ${CMAKE_SOURCE_DIR}/src/xr/shaders/controller.frag
    DEPENDS ${CMAKE_SOURCE_DIR}/src/xr/shaders/controller.vert ${CMAKE_SOURCE_DIR}/src/xr/shaders/controller.frag
    VERBATIM)
add_custom_target(rift_shaders DEPENDS ${SHADER_OUT}/window_rgba8.spv.h ${SHADER_OUT}/window_rgba16f.spv.h
                                       ${SHADER_OUT}/resample_rgba8.spv.h ${SHADER_OUT}/resample_rgba16f.spv.h
                                       ${SHADER_OUT}/controller_vert.spv.h ${SHADER_OUT}/controller_frag.spv.h)

# The OpenXR session on RiftLinux's runtime.
add_library(rift_xr STATIC src/xr/XrCompositor.cpp src/xr/XrInput.cpp src/xr/FrameArt.cpp src/xr/VirtualGamepad.cpp src/xr/RuntimeApps.cpp
            src/xr/ControllerModel.cpp src/xr/ControllerRenderer.cpp src/xr/ViewShot.cpp)
add_dependencies(rift_xr rift_shaders)
target_include_directories(rift_xr PRIVATE third_party/openxr ${SHADER_OUT})
# OpenXR and Vulkan structures start as {TYPE}, the rest zeroed on purpose.
target_compile_options(rift_xr PRIVATE -Wno-missing-field-initializers)
target_link_libraries(rift_xr PUBLIC rift_bridge rift_spatial Qt6::Gui PkgConfig::OPENXR PkgConfig::VULKAN PkgConfig::LIBDRM ${CMAKE_DL_LIBS})

# Wayland server on wlroots.
add_library(rift_wayland STATIC
    src/wayland/Server.cpp
    src/wayland/Window.cpp
    src/wayland/Output.cpp
    src/wayland/Input.cpp
    src/wayland/SurfaceProbe.cpp
    src/wayland/SurfaceExport.cpp
    src/wayland/StatusPage.cpp
    src/wayland/ColorManagement.cpp
    src/wayland/LinuxDmabuf.cpp
    ${CMAKE_BINARY_DIR}/protocols/color-management-v1-server-protocol.h
    ${CMAKE_BINARY_DIR}/protocols/color-management-v1-protocol.c
    ${CMAKE_BINARY_DIR}/protocols/linux-dmabuf-v1-server-protocol.h
    ${CMAKE_BINARY_DIR}/protocols/linux-dmabuf-v1-protocol.c
)
target_include_directories(rift_wayland PUBLIC src)
target_include_directories(rift_wayland PRIVATE ${CMAKE_BINARY_DIR}/protocols)
target_compile_definitions(rift_wayland PUBLIC WLR_USE_UNSTABLE)
target_link_libraries(rift_wayland PUBLIC
    rift_color
    rift_bridge
    Qt6::Core
    PkgConfig::WLROOTS
    PkgConfig::WAYLAND_SERVER
    PkgConfig::XKBCOMMON
    PkgConfig::PIXMAN
)

# The GPU's path (which GPU, video decoding, Firefox's preferences): Rift Home's own choice on any machine.
add_library(rift_gpu STATIC src/app/GpuProfile.cpp src/app/Command.cpp src/app/FirefoxPrefs.cpp src/app/Chromium.cpp)
target_include_directories(rift_gpu PUBLIC src)
target_link_libraries(rift_gpu PUBLIC Qt6::Core PkgConfig::LIBDRM PkgConfig::VULKAN)
target_compile_options(rift_gpu PRIVATE -Wno-missing-field-initializers) # Vulkan's structs: sType only, the rest zero

add_executable(rift-home
    src/app/main.cpp
    src/app/AppLauncher.cpp
    src/app/AppCatalog.cpp
    src/app/NotificationServer.cpp
    src/app/ControlsService.cpp
    src/app/HeadsetService.cpp
    src/app/BusForwarder.cpp
)
target_link_libraries(rift-home PRIVATE rift_wayland rift_xr rift_gpu Qt6::DBus)
target_compile_definitions(rift-home PRIVATE PROJECT_VERSION_STRING="${PROJECT_VERSION}")

# Settings: a Qt Quick app shown in Rift Home as a window; system settings through nmcli, pactl and
# timedatectl, Rift Home's own through settings.json.
find_package(Qt6 6.11 COMPONENTS Quick QuickControls2 Network DBus)
# Settings' own password prompt: polkit's agent library (part of polkit).
pkg_check_modules(POLKIT_AGENT IMPORTED_TARGET polkit-agent-1)
if(Qt6Quick_FOUND AND Qt6QuickControls2_FOUND)
    file(GLOB SETTINGS_QML RELATIVE ${CMAKE_SOURCE_DIR} CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/settings/qml/*.qml)
    qt_add_executable(rift-home-settings src/settings/main.cpp src/settings/Backends.cpp src/settings/Authentication.cpp src/graphics/RadianceHdr.cpp)
    qt_add_resources(rift-home-settings settings_qml PREFIX "/qml" BASE src/settings/qml FILES ${SETTINGS_QML})
    target_include_directories(rift-home-settings PRIVATE src)
    target_link_libraries(rift-home-settings PRIVATE rift_spatial Qt6::Quick Qt6::QuickControls2 Qt6::Gui Qt6::Network Qt6::DBus)
    if(POLKIT_AGENT_FOUND)
        target_compile_definitions(rift-home-settings PRIVATE RIFT_HOME_POLKIT)
        target_link_libraries(rift-home-settings PRIVATE PkgConfig::POLKIT_AGENT)
    endif()
    target_compile_definitions(rift-home-settings PRIVATE RIFT_HOME_VERSION="${PROJECT_VERSION}")
endif()

enable_testing()
add_executable(tst_layerplan tests/color/tst_layerplan.cpp)
target_link_libraries(tst_layerplan PRIVATE rift_color Qt6::Test)
add_test(NAME tst_layerplan COMMAND tst_layerplan)
add_executable(tst_spatial tests/spatial/tst_spatial.cpp)
target_link_libraries(tst_spatial PRIVATE rift_spatial Qt6::Test)
add_test(NAME tst_spatial COMMAND tst_spatial)
add_executable(tst_hdr tests/graphics/tst_hdr.cpp)
target_link_libraries(tst_hdr PRIVATE rift_color Qt6::Test)
add_test(NAME tst_hdr COMMAND tst_hdr)
add_executable(tst_parse tests/settings/tst_parse.cpp)
target_include_directories(tst_parse PRIVATE src)
target_link_libraries(tst_parse PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_parse COMMAND tst_parse)
add_executable(tst_gpuprofile tests/app/tst_gpuprofile.cpp)
target_link_libraries(tst_gpuprofile PRIVATE rift_gpu Qt6::Test)
add_test(NAME tst_gpuprofile COMMAND tst_gpuprofile)
add_executable(tst_firefoxprefs tests/app/tst_firefoxprefs.cpp)
target_link_libraries(tst_firefoxprefs PRIVATE rift_gpu Qt6::Test)
add_test(NAME tst_firefoxprefs COMMAND tst_firefoxprefs)
add_executable(tst_chromium tests/app/tst_chromium.cpp)
target_link_libraries(tst_chromium PRIVATE rift_gpu Qt6::Test)
add_test(NAME tst_chromium COMMAND tst_chromium)
add_executable(tst_notifications tests/app/tst_notifications.cpp)
target_include_directories(tst_notifications PRIVATE src)
target_link_libraries(tst_notifications PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_notifications COMMAND tst_notifications)

# Rift Home's own controller models, as Rift Home fits them into the grip.
add_executable(tst_controllermodel tests/xr/tst_controllermodel.cpp src/xr/ControllerModel.cpp)
target_include_directories(tst_controllermodel PRIVATE src)
target_compile_definitions(tst_controllermodel PRIVATE RIFT_HOME_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(tst_controllermodel PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_controllermodel COMMAND tst_controllermodel)
# The screenshot's renderer: the layers' geometry ray-cast from the head.
add_executable(tst_viewshot tests/xr/tst_viewshot.cpp src/xr/ViewShot.cpp)
target_include_directories(tst_viewshot PRIVATE src)
target_link_libraries(tst_viewshot PRIVATE Qt6::Gui Qt6::Test)
add_test(NAME tst_viewshot COMMAND tst_viewshot)
# The headset's own pictures drawn by Rift Home (the lens test).
add_executable(tst_frameart tests/xr/tst_frameart.cpp src/xr/FrameArt.cpp)
target_include_directories(tst_frameart PRIVATE src)
target_link_libraries(tst_frameart PRIVATE rift_spatial Qt6::Gui Qt6::Test)
add_test(NAME tst_frameart COMMAND tst_frameart)
add_executable(tst_keyboard tests/spatial/tst_keyboard.cpp)
target_link_libraries(tst_keyboard PRIVATE rift_spatial Qt6::Test)
add_test(NAME tst_keyboard COMMAND tst_keyboard)

# Test tools for comparing captures: the pattern as a Wayland window, and RiftLinux's own test application.
add_executable(rift-hdr-pattern
    tools/hdr-pattern/hdr_pattern_client.cpp
    ${CMAKE_BINARY_DIR}/protocols/xdg-shell-protocol.c
    ${CMAKE_BINARY_DIR}/protocols/color-management-v1-protocol.c)
target_include_directories(rift-hdr-pattern PRIVATE ${CMAKE_BINARY_DIR}/protocols)
target_compile_options(rift-hdr-pattern PRIVATE -Wno-missing-field-initializers)
target_link_libraries(rift-hdr-pattern PRIVATE PkgConfig::WAYLAND_CLIENT m)

# A window locking the pointer as a game's mouse look, for headless checks.
add_executable(rift-pointer-lock-test
    tools/pointer-lock/pointer_lock_client.cpp
    ${CMAKE_BINARY_DIR}/protocols/xdg-shell-protocol.c
    ${CMAKE_BINARY_DIR}/protocols/pointer-constraints-unstable-v1-protocol.c
    ${CMAKE_BINARY_DIR}/protocols/relative-pointer-unstable-v1-protocol.c)
target_include_directories(rift-pointer-lock-test PRIVATE ${CMAKE_BINARY_DIR}/protocols)
target_compile_options(rift-pointer-lock-test PRIVATE -Wno-missing-field-initializers)
target_link_libraries(rift-pointer-lock-test PRIVATE PkgConfig::WAYLAND_CLIENT)

# An X11 window reporting its X screen, its size and its clicks (Rift Home's X11 screen), for headless checks.
pkg_check_modules(XCB IMPORTED_TARGET xcb)
if(XCB_FOUND)
    add_executable(rift-x11-pointer-test tools/x11-pointer/x11_pointer_client.cpp)
    target_link_libraries(rift-x11-pointer-test PRIVATE PkgConfig::XCB)
endif()

# The VR apps connected to RiftLinux's runtime, and switching between them (through libmonado), for checks and by hand.
# Checks a compositor's color-management-v1 against the protocol.
add_executable(rift-color-check tools/color-check/color_check_client.cpp ${CMAKE_BINARY_DIR}/protocols/color-management-v1-client-protocol.h
    ${CMAKE_BINARY_DIR}/protocols/color-management-v1-protocol.c)
target_include_directories(rift-color-check PRIVATE ${CMAKE_BINARY_DIR}/protocols)
target_compile_options(rift-color-check PRIVATE -Wno-missing-field-initializers)
target_link_libraries(rift-color-check PRIVATE PkgConfig::WAYLAND_CLIENT)
add_executable(rift-vr-apps tools/vr-apps/vr_apps.cpp src/xr/RuntimeApps.cpp)
target_include_directories(rift-vr-apps PRIVATE src)
target_link_libraries(rift-vr-apps PRIVATE Qt6::Core ${CMAKE_DL_LIBS})

# Rift Home's virtual gamepad on its own: made, checked, removed.
add_executable(rift-gamepad-test tools/gamepad-test/gamepad_test.cpp src/xr/VirtualGamepad.cpp)
target_include_directories(rift-gamepad-test PRIVATE src)
target_link_libraries(rift-gamepad-test PRIVATE Qt6::Core)

add_executable(riftlinux-openxr-hdr-test third_party/riftlinux/riftlinux_openxr_hdr_test.cpp)
target_include_directories(riftlinux-openxr-hdr-test PRIVATE third_party/openxr)
target_compile_options(riftlinux-openxr-hdr-test PRIVATE -w)
target_link_libraries(riftlinux-openxr-hdr-test PRIVATE PkgConfig::OPENXR ${CMAKE_DL_LIBS} m)

# Installing (packages, or cmake --install): the programs and their start commands in bin, the desktop's menu entry,
# the login screen's session, the icon, the controller models and rooms in share/rift-home, the Firefox profile
# check at desktop login, the user documentation. The desktop entries are the work tree's templates pointed at the
# installed commands.
include(GNUInstallDirs)
install(TARGETS rift-home RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(TARGET rift-home-settings)
    install(TARGETS rift-home-settings RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(PROGRAMS scripts/rift-home-run.sh DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME rift-home-run)
install(PROGRAMS scripts/rift-home-start.sh DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME rift-home-start)
install(PROGRAMS scripts/rift-home-session-start DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME rift-home-session)
install(PROGRAMS scripts/rift-home-vt.sh DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME rift-home-console)
foreach(entry org.rifthome.RiftHome rift-home-session)
    file(READ ${CMAKE_SOURCE_DIR}/data/${entry}.desktop.in text)
    string(REPLACE "@ROOT@/scripts/rift-home-start.sh" "rift-home-start" text "${text}")
    string(REPLACE "@ROOT@/scripts/rift-home-session-start" "rift-home-session" text "${text}")
    string(REPLACE "@ROOT@/data/rift-home.svg" "rift-home" text "${text}")
    file(WRITE ${CMAKE_BINARY_DIR}/installed/${entry}.desktop "${text}")
endforeach()
install(FILES ${CMAKE_BINARY_DIR}/installed/org.rifthome.RiftHome.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${CMAKE_BINARY_DIR}/installed/rift-home-session.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/wayland-sessions
        RENAME rift-home.desktop)
install(FILES data/rift-home.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES data/rift-home-firefox-restore.desktop DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/xdg/autostart)
install(FILES data/controllers/ControllerLeft.glb data/controllers/ControllerRight.glb
        DESTINATION ${CMAKE_INSTALL_DATADIR}/rift-home/controllers)
install(DIRECTORY data/rooms/ DESTINATION ${CMAKE_INSTALL_DATADIR}/rift-home/rooms OPTIONAL
        PATTERN "*.license" EXCLUDE)
install(FILES README.md NOTICE DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/rift-home)
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/rift-home/docs FILES_MATCHING PATTERN "*.md" PATTERN "*.png"
        PATTERN plans EXCLUDE PATTERN reports EXCLUDE PATTERN releases EXCLUDE PATTERN addon-page.md EXCLUDE)

# API documentation with Doxygen, not part of the normal build: cmake --build build --target doc (build/doc/html).
find_package(Doxygen)
if(DOXYGEN_FOUND)
    configure_file(docs/Doxyfile.in ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile @ONLY)
    add_custom_target(doc
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/doc
        COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "API documentation (Doxygen)"
        VERBATIM)
    add_dependencies(doc rift_shaders)
endif()
