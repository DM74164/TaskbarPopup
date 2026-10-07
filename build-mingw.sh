#!/bin/sh
# 在 Linux 上用 MinGW-w64 交叉编译（需要 g++-mingw-w64-x86-64-posix）
set -e
cd "$(dirname "$0")"
x86_64-w64-mingw32-windres src/app.rc -O coff -o build-app.res
x86_64-w64-mingw32-g++ -std=c++17 -O2 -s -Wall -Wextra -Wno-missing-field-initializers \
    -DUNICODE -D_UNICODE -DNOMINMAX -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 \
    src/*.cpp build-app.res -o TaskbarPopup.exe \
    -mwindows -municode -static -static-libgcc -static-libstdc++ \
    -luser32 -lgdi32 -lgdiplus -lshell32 -lcomctl32 -ldwmapi -lshcore -ladvapi32 -lole32 -luuid -loleaut32 -ldxva2
rm -f build-app.res
