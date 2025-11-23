@echo off
setlocal

set BUILD_TYPE=Debug
if /i "%~1"=="--release" set BUILD_TYPE=Release

if not defined CMAKE_GENERATOR set CMAKE_GENERATOR=Visual Studio 18 2026

echo Configuring N2Engine with Visual Studio...
cmake -S . -B build -G "%CMAKE_GENERATOR%" -A x64

if errorlevel 1 (
    echo.
    echo ========================================
    echo CMake configuration FAILED!
    echo ========================================
    exit /b 1
)

echo.
echo Building %BUILD_TYPE% configuration...
cmake --build build --config %BUILD_TYPE% -j

if errorlevel 1 (
    echo.
    echo ========================================
    echo Build FAILED!
    echo ========================================
    echo Check the errors above and fix them.
    exit /b 1
)

echo.
echo ========================================
echo Build SUCCESS!
echo ========================================
echo Executable: build\bin\%BUILD_TYPE%\test_project.exe
echo Editor: build\bin\%BUILD_TYPE%\editor.exe
echo.

endlocal