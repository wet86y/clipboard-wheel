cmake_minimum_required(VERSION 3.25)

project(SuperMiddleKeyNative VERSION 2.0.0 LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "Super Middle Key native is a Windows-only application.")
endif()

if(NOT MSVC)
    message(FATAL_ERROR "The migration build currently requires MSVC.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_library(super_middle_key_core STATIC
    core/Animation.cpp
    core/ClipboardHistory.cpp
    core/ExtendedWheel.cpp
    core/PastePolicy.cpp
    core/Settings.cpp
    core/WheelInteraction.cpp
    core/WheelLayout.cpp
    core/WheelVisualGeometry.cpp
)
target_include_directories(super_middle_key_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(super_middle_key_core PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(super_middle_key_core PRIVATE /W4 /permissive- /utf-8 /EHsc)

add_library(super_middle_key_windows STATIC
    platform/windows/AutoStart.cpp
    platform/windows/ClipboardService.cpp
    platform/windows/CrashHandler.cpp
    platform/windows/DiagnosticLog.cpp
    platform/windows/ElevationService.cpp
    platform/windows/ExtendedActionExecutor.cpp
    platform/windows/HotkeyCaptureHook.cpp
    platform/windows/HotkeyCodec.cpp
    platform/windows/ImageClipboardPayload.cpp
    platform/windows/MouseHook.cpp
    platform/windows/SettingsStore.cpp
    platform/windows/ShortcutIconResolver.cpp
    platform/windows/ShortcutDropTarget.cpp
    platform/windows/ShortcutDropHelper.cpp
    platform/windows/SingleInstance.cpp
    platform/windows/StartupTrace.cpp
    platform/windows/TrayIcon.cpp
    platform/windows/WindowsClipboardHistory.cpp
)
target_include_directories(super_middle_key_windows PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(super_middle_key_windows PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_definitions(super_middle_key_windows PUBLIC $<$<CONFIG:RelWithDebInfo>:SMK_DIAGNOSTICS=1>)
target_compile_options(super_middle_key_windows PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(super_middle_key_windows
    PUBLIC super_middle_key_core
    PRIVATE advapi32 bcrypt d2d1 dbghelp dwrite dwmapi gdi32 gdiplus ole32 oleaut32 runtimeobject shell32 shlwapi
        uiautomationcore user32 userenv windowscodecs wtsapi32
)

add_library(super_middle_key_ui STATIC
    ui/SettingsVisualLayout.cpp
    ui/SettingsWindow.cpp
    ui/WheelWindow.cpp
)
target_include_directories(super_middle_key_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(super_middle_key_ui PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_definitions(super_middle_key_ui PRIVATE $<$<CONFIG:RelWithDebInfo>:SMK_DIAGNOSTICS=1>)
target_compile_options(super_middle_key_ui PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(super_middle_key_ui PUBLIC super_middle_key_core PRIVATE super_middle_key_windows comctl32 d2d1 dwrite dwmapi shell32 user32 uxtheme windowscodecs winmm)

add_executable(super_middle_key_native WIN32
    app/AppHost.cpp
    app/main.cpp
    app/resources.rc
)
set_target_properties(super_middle_key_native PROPERTIES OUTPUT_NAME "超级中键")
set_target_properties(super_middle_key_native PROPERTIES PDB_NAME "超级中键")
target_compile_definitions(super_middle_key_native PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_definitions(super_middle_key_native PRIVATE $<$<CONFIG:RelWithDebInfo>:SMK_DIAGNOSTICS=1>)
target_link_options(super_middle_key_native PRIVATE $<$<CONFIG:RelWithDebInfo>:/DEBUG:FULL>)
target_compile_options(super_middle_key_native PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_options(super_middle_key_native PRIVATE /MANIFEST:NO)
target_link_libraries(super_middle_key_native
    PRIVATE super_middle_key_core super_middle_key_windows super_middle_key_ui ole32 shell32 user32
)

include(CTest)
if(BUILD_TESTING)
    add_executable(super_middle_key_native_tests
        tests/CoreTests.cpp
    )
    target_compile_definitions(super_middle_key_native_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(super_middle_key_native_tests PRIVATE /W4 /permissive- /utf-8 /EHsc)
    target_link_libraries(super_middle_key_native_tests PRIVATE super_middle_key_core)
    add_test(NAME native_core COMMAND super_middle_key_native_tests)

    add_executable(super_middle_key_native_windows_tests
        tests/WindowsClipboardTests.cpp
    )
    target_compile_definitions(super_middle_key_native_windows_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(super_middle_key_native_windows_tests PRIVATE /W4 /permissive- /utf-8 /EHsc)
    target_link_libraries(super_middle_key_native_windows_tests PRIVATE super_middle_key_windows ole32 user32)
    add_test(NAME native_windows_clipboard COMMAND super_middle_key_native_windows_tests)

    add_executable(super_middle_key_native_wheel_tests
        tests/WheelWindowTests.cpp
    )
    target_compile_definitions(super_middle_key_native_wheel_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(super_middle_key_native_wheel_tests PRIVATE /W4 /permissive- /utf-8 /EHsc)
    target_link_libraries(super_middle_key_native_wheel_tests PRIVATE super_middle_key_ui ole32 user32)
    add_test(NAME native_wheel_window COMMAND super_middle_key_native_wheel_tests)

    add_executable(super_middle_key_native_diagnostic_tests
        tests/DiagnosticLogTests.cpp
    )
    target_compile_definitions(super_middle_key_native_diagnostic_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(super_middle_key_native_diagnostic_tests PRIVATE /W4 /permissive- /utf-8 /EHsc)
    target_link_libraries(super_middle_key_native_diagnostic_tests PRIVATE super_middle_key_windows)
    add_test(NAME native_diagnostic_log COMMAND super_middle_key_native_diagnostic_tests)

    add_executable(super_middle_key_native_settings_tests
        tests/SettingsWindowTests.cpp
    )
    target_compile_definitions(super_middle_key_native_settings_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(super_middle_key_native_settings_tests PRIVATE /W4 /permissive- /utf-8 /EHsc)
    target_link_libraries(super_middle_key_native_settings_tests PRIVATE super_middle_key_ui ole32 user32)
    add_test(NAME native_settings_window COMMAND super_middle_key_native_settings_tests)
endif()
