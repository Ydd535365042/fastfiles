cmake_minimum_required(VERSION 3.20)
project(FastFiles VERSION 1.0.0.10 LANGUAGES CXX)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# x64 or x86, from the toolchain the build runs in (build.ps1 -Arch picks the VS developer shell): the Rust target, the
# manifests' processorArchitecture and the setup's name follow it
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(FF_ARCH x86)
    set(FF_MANIFEST_ARCH x86)
    set(FF_RUST_TARGET i686-pc-windows-msvc)
else()
    set(FF_ARCH x64)
    set(FF_MANIFEST_ARCH amd64)
    set(FF_RUST_TARGET x86_64-pc-windows-msvc)
endif()
# The checked-in manifests say amd64; the x86 build gets copies that say x86 (a wrong architecture fails to start)
foreach(m src/app.manifest setup/setup.manifest)
    file(READ ${CMAKE_SOURCE_DIR}/${m} FF_MANIFEST_TEXT)
    string(REPLACE "processorArchitecture=\"amd64\"" "processorArchitecture=\"${FF_MANIFEST_ARCH}\"" FF_MANIFEST_TEXT "${FF_MANIFEST_TEXT}")
    get_filename_component(FF_MANIFEST_NAME ${m} NAME)
    file(CONFIGURE OUTPUT ${CMAKE_BINARY_DIR}/generated/${FF_MANIFEST_NAME} CONTENT "${FF_MANIFEST_TEXT}" @ONLY)
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS src/app.manifest setup/setup.manifest)

add_executable(FastFiles WIN32
    src/main.cpp
    src/MainWindow.cpp
    src/RoundFrame.cpp
    src/MainWindowLayout.cpp
    src/MainWindowStatus.cpp
    src/MainWindowTray.cpp
    src/FolderSize.cpp
    src/FileSearch.cpp
    src/TabSearch.cpp
    src/Operations.cpp
    src/FileOperations.cpp
    src/StatusCenter.cpp
    src/MainWindowFileOps.cpp
    src/MainWindowOperations.cpp
    src/MainWindowTags.cpp
    src/Tags.cpp
    src/TabMenu.cpp
    src/MainWindowCommands.cpp
src/MainWindowToolbar.cpp src/Toolbar.cpp src/ToolbarDialog.cpp src/Log.cpp
src/ContentDialog.cpp src/NewItem.cpp src/MainWindowNewItem.cpp src/SevenZip.cpp src/MediaPreview.cpp src/Lang.cpp src/ColorPickerFlyout.cpp
    src/MainWindowRecycle.cpp
    src/Breadcrumb.cpp src/Theme.cpp src/DWriteText.cpp src/Controls.cpp src/TabStrip.cpp
    src/TabCommands.cpp src/TabView.cpp src/MainWindowView.cpp
    src/MainWindowTabs.cpp
    src/MainWindowTabDrag.cpp
    src/ColumnView.cpp src/TabColumns.cpp
    src/FileList.cpp src/FileListModel.cpp src/FileListPaint.cpp src/FileListInput.cpp src/FileListDrop.cpp src/FileListGroup.cpp src/FileListAccessible.cpp src/FileListLayout.cpp src/FileListThumbs.cpp src/FileItems.cpp src/TabList.cpp
    src/MainWindowInput.cpp
    src/MainWindowPanes.cpp
    src/MainWindowHome.cpp
    src/MainWindowKeys.cpp
    src/MainWindowSettings.cpp
    src/SettingsView.cpp
    src/CommandPalette.cpp
    src/Settings.cpp
    src/DarkMode.cpp
    src/HomeData.cpp
    src/HomeView.cpp
    src/HomeInput.cpp
    src/Tab.cpp
    src/Sidebar.cpp
    src/SidebarModel.cpp
    src/SidebarDrop.cpp
    src/InfoPane.cpp
    src/InfoPaneData.cpp
    src/Sandbox.cpp
    src/SmoothScroll.cpp
    src/ScrollOverlay.cpp
    src/Backdrop.cpp
    src/Format.cpp
    src/Drives.cpp
    src/Compat.cpp
    src/SearchSuggest.cpp
    src/Session.cpp
    src/Update.cpp
    src/Share.cpp
    ${CMAKE_BINARY_DIR}/generated/app.manifest
    src/FastFiles.rc
)
set_source_files_properties(src/FastFiles.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_SOURCE_DIR}/res/lang.txt)

configure_file(src/version.h.in ${CMAKE_BINARY_DIR}/generated/version.h @ONLY)
target_include_directories(FastFiles PRIVATE ${CMAKE_BINARY_DIR}/generated)
target_compile_features(FastFiles PRIVATE cxx_std_20)
target_compile_definitions(FastFiles PRIVATE UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00 "FASTFILES_VERSION=L\"${PROJECT_VERSION}\"")
target_compile_options(FastFiles PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(FastFiles PRIVATE comctl32 shell32 shlwapi ole32 oleaut32 uuid uxtheme gdiplus propsys msimg32 dwmapi comdlg32 rstrtmgr oleacc dwrite winhttp version runtimeobject)

# Rust core: built by cargo as a static lib (release profile, static CRT to match /MT) and linked in
set(CORE_DIR ${CMAKE_SOURCE_DIR}/core)
set(CARGO_TARGET_DIR ${CMAKE_BINARY_DIR}/cargo)
set(CORE_LIB ${CARGO_TARGET_DIR}/${FF_RUST_TARGET}/release/fastfiles_core.lib)
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS ${CORE_DIR}/src/*.rs)
add_custom_command(
    OUTPUT ${CORE_LIB}
    COMMAND cargo build --release --lib --target ${FF_RUST_TARGET} --manifest-path ${CORE_DIR}/Cargo.toml --target-dir ${CARGO_TARGET_DIR}
    DEPENDS ${CORE_SOURCES} ${CORE_DIR}/Cargo.toml
    WORKING_DIRECTORY ${CORE_DIR}
    COMMENT "Building Rust core"
    VERBATIM)
add_custom_target(fastfiles_core_build DEPENDS ${CORE_LIB})
add_dependencies(FastFiles fastfiles_core_build)
target_include_directories(FastFiles PRIVATE ${CORE_DIR}/include)
target_link_libraries(FastFiles PRIVATE ${CORE_LIB} ntdll userenv ws2_32 dbghelp)

# FastFiles' own license (MIT) and the third-party notices travel with the program (MIT requires the notice).
# No built-in compress / extract (removed at the user's request: the shell's own menu items - WinRAR, 7-Zip... -
# cover it), so 7z.dll is no longer shipped next to the exe or in the installer payload.
set(FF_LICENSE ${CMAKE_SOURCE_DIR}/LICENSE)
set(FF_NOTICES ${CMAKE_SOURCE_DIR}/THIRD-PARTY-NOTICES.txt)
add_custom_command(TARGET FastFiles POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FF_LICENSE} $<TARGET_FILE_DIR:FastFiles>/LICENSE.txt
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FF_NOTICES} $<TARGET_FILE_DIR:FastFiles>/THIRD-PARTY-NOTICES.txt
    VERBATIM)

# Installer: our own Setup.exe (self-drawn wizard, Windows Compression API payload). ffpack compresses FastFiles.exe,
# Setup.rc embeds the result; Uninstall.exe is Setup.exe with the payload removed at install time.
add_executable(ffpack setup/ffpack.cpp)
target_compile_features(ffpack PRIVATE cxx_std_20)
target_compile_definitions(ffpack PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(ffpack PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(ffpack PRIVATE cabinet)

set(FF_PAYLOAD ${CMAKE_BINARY_DIR}/payload.bin)
add_custom_command(
    OUTPUT ${FF_PAYLOAD}
    COMMAND ffpack ${FF_PAYLOAD} $<TARGET_FILE:FastFiles> ${FF_LICENSE}=LICENSE.txt ${FF_NOTICES}=THIRD-PARTY-NOTICES.txt
    DEPENDS ffpack FastFiles ${FF_LICENSE} ${FF_NOTICES}
    COMMENT "Packing FastFiles.exe"
    VERBATIM)
add_custom_target(ff_payload DEPENDS ${FF_PAYLOAD})
file(WRITE ${CMAKE_BINARY_DIR}/generated/payload_path.h "#define FF_PAYLOAD_PATH \"${FF_PAYLOAD}\"\n")

add_executable(FastFilesSetup WIN32
    setup/Main.cpp setup/Install.cpp setup/System.cpp setup/Draw.cpp setup/Ui.cpp setup/UiPaint.cpp src/RoundFrame.cpp
    src/DWriteText.cpp setup/Setup.rc ${CMAKE_BINARY_DIR}/generated/setup.manifest)
add_dependencies(FastFilesSetup ff_payload)
set_source_files_properties(setup/Setup.rc PROPERTIES OBJECT_DEPENDS ${FF_PAYLOAD})
target_include_directories(FastFilesSetup PRIVATE ${CMAKE_BINARY_DIR}/generated ${CMAKE_SOURCE_DIR}/src)
target_compile_features(FastFilesSetup PRIVATE cxx_std_20)
target_compile_definitions(FastFilesSetup PRIVATE UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)
target_compile_options(FastFilesSetup PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_options(FastFilesSetup PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
target_link_libraries(FastFilesSetup PRIVATE cabinet rstrtmgr dwmapi gdiplus msimg32 shell32 shlwapi ole32 oleaut32 uuid version dwrite)
set_target_properties(FastFilesSetup PROPERTIES OUTPUT_NAME "FastFiles_${PROJECT_VERSION}_${FF_ARCH}_Setup")