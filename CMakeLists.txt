cmake_minimum_required(VERSION 3.16)
project(TaskbarPopup LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(TaskbarPopup WIN32
    src/main.cpp
    src/hook.cpp
    src/fullscreen.cpp
    src/taskbar_anim.cpp
    src/desktop_icons.cpp
    src/windows.cpp
    src/popup.cpp
    src/glass.cpp
    src/levels.cpp
    src/settings_dialog.cpp
    src/pinned.cpp
    src/util.cpp
    src/elevation.cpp
    src/apps.cpp
    src/app_menu.cpp
    src/thumbnail.cpp
    src/update.cpp
    src/float_windows.cpp
    src/hotkeys.cpp
    src/tray.cpp
    src/lang.cpp
    src/radios.cpp
    src/attention.cpp
    src/indicators.cpp
    src/media.cpp
    src/app.rc
)

target_compile_definitions(TaskbarPopup PRIVATE
    UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)

if(NOT DEFINED TP_VERSION AND DEFINED ENV{TP_VERSION})
    set(TP_VERSION "$ENV{TP_VERSION}")
endif()
if(DEFINED TP_VERSION)
    string(REGEX REPLACE "^[vV]" "" TP_VER_CLEAN "${TP_VERSION}")
    string(REPLACE "." ";" TP_VER_LIST "${TP_VER_CLEAN}")
    list(LENGTH TP_VER_LIST TP_VER_COUNT)
    if(TP_VER_COUNT EQUAL 3)
        list(GET TP_VER_LIST 0 TP_VER_MAJOR)
        list(GET TP_VER_LIST 1 TP_VER_MINOR)
        list(GET TP_VER_LIST 2 TP_VER_PATCH)
        target_compile_definitions(TaskbarPopup PRIVATE
            TP_VERSION_MAJOR=${TP_VER_MAJOR}
            TP_VERSION_MINOR=${TP_VER_MINOR}
            TP_VERSION_PATCH=${TP_VER_PATCH})
        set_property(SOURCE src/app.rc APPEND PROPERTY COMPILE_DEFINITIONS
            TP_VERSION_MAJOR=${TP_VER_MAJOR}
            TP_VERSION_MINOR=${TP_VER_MINOR}
            TP_VERSION_PATCH=${TP_VER_PATCH})
    endif()
endif()

target_link_libraries(TaskbarPopup PRIVATE
    user32 gdi32 gdiplus shell32 comctl32 dwmapi shcore advapi32 ole32 uuid
    oleaut32 dxva2 winhttp iphlpapi imm32 comdlg32)

if(MSVC)
    target_compile_options(TaskbarPopup PRIVATE /utf-8 /W4)
    # 清单由 app.rc 嵌入，关掉链接器自动生成的那份
    target_link_options(TaskbarPopup PRIVATE /MANIFEST:NO)
    set_property(TARGET TaskbarPopup PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(TaskbarPopup PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
    target_link_options(TaskbarPopup PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif()
