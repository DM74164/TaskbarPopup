#!/bin/sh
# 在 Linux 上用 MinGW-w64 交叉编译（需要 g++-mingw-w64-x86-64-posix）
set -e
cd "$(dirname "$0")"
# 版本号：设了 TP_VERSION（形如 1.2.3）就用它，否则用 src/version.h 里的默认值
VERDEFS=
if [ -n "${TP_VERSION:-}" ]; then
    case "$TP_VERSION" in
        *[!0-9.]*|.*|*.|*..*) echo "TP_VERSION 应该形如 1.2.3：$TP_VERSION" >&2; exit 1 ;;
    esac
    old_ifs=$IFS; IFS=.; set -- $TP_VERSION; IFS=$old_ifs
    if [ $# -ne 3 ]; then echo "TP_VERSION 应该形如 1.2.3：$TP_VERSION" >&2; exit 1; fi
    VERDEFS="-DTP_VERSION_MAJOR=$1 -DTP_VERSION_MINOR=$2 -DTP_VERSION_PATCH=$3"
fi
x86_64-w64-mingw32-windres $VERDEFS src/app.rc -O coff -o build-app.res
x86_64-w64-mingw32-g++ -std=c++17 -O2 -s -Wall -Wextra -Wno-missing-field-initializers \
    -DUNICODE -D_UNICODE -DNOMINMAX -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 \
    src/*.cpp build-app.res -o TaskbarPopup.exe \
    -mwindows -municode -static -static-libgcc -static-libstdc++ \
    -luser32 -lgdi32 -lgdiplus -lshell32 -lcomctl32 -ldwmapi -lshcore -ladvapi32 -lole32 -luuid -loleaut32 -ldxva2
rm -f build-app.res
