#!/usr/bin/env sh
# Cross-compile GPUFanCurveMonitor.exe with MinGW-w64 (Linux/MSYS2).
# Debian/Ubuntu: sudo apt install g++-mingw-w64-x86-64
set -e
cd "$(dirname "$0")"
CXX=${CXX:-x86_64-w64-mingw32-g++}
WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}
mkdir -p build dist
$WINDRES -I res res/app.rc -O coff -o build/app.res
$CXX -std=c++17 -O2 -s -municode -mwindows -static -static-libgcc -static-libstdc++ \
    -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
    src/GPUFanCurveMonitor.cpp build/app.res \
    -lgdiplus -lcomdlg32 -lgdi32 -luser32 -lshell32 -lole32 \
    -o dist/GPUFanCurveMonitor.exe
echo "Built dist/GPUFanCurveMonitor.exe"
