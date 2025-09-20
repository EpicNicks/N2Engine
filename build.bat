@echo off
setlocal

:: Default build type
set BUILD_TYPE=Debug

:: Parse argument
if "%~1"=="" (
    echo No argument given, defaulting to Debug build.
) else if /i "%~1"=="--debug" (
    set BUILD_TYPE=Debug
) else if /i "%~1"=="--release" (
    set BUILD_TYPE=Release
) else (
    echo Invalid argument: %~1
    echo Usage: %~nx0 [--debug ^| --release]
    exit /b 1
)

:: Define build directory based on type
set BUILD_DIR=build/%BUILD_TYPE%

if "%BUILD_TYPE%"=="Debug" (
    set CXX_FLAGS=-g -O0
    set BUILD_TYPE_UPPER=DEBUG
) else if "%BUILD_TYPE%"=="Release" (
    set CXX_FLAGS=-O3 -DNDEBUG
    set BUILD_TYPE_UPPER=RELEASE
)

cmake -S . -B %BUILD_DIR% -G "MinGW Makefiles" ^
    -DCMAKE_C_COMPILER=gcc ^
    -DCMAKE_CXX_COMPILER=g++ ^
    -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
    -DCMAKE_CXX_FLAGS_%BUILD_TYPE_UPPER%="%CXX_FLAGS%"

:: Build
cmake --build %BUILD_DIR% --config %BUILD_TYPE%

endlocal
