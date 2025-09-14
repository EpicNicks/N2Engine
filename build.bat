@echo off
setlocal

REM --- Delete previous build directory to avoid stale caches ---
if exist build (
    rmdir /s /q build
)

REM --- Configure CMake explicitly for MinGW ---
cmake -S . -B build -G "MinGW Makefiles" ^
    -DCMAKE_C_COMPILER=gcc ^
    -DCMAKE_CXX_COMPILER=g++ ^
    -DCMAKE_BUILD_TYPE=Debug

REM --- Build ---
cmake --build build --config Debug

endlocal
