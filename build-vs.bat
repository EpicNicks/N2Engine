@echo off
setlocal

set BUILD_TYPE=Debug
set RUN_AFTER_BUILD=0
set CLEAN_BUILD=0
set VERBOSE_BUILD=0

set ORIGINAL_ARGS=%*

for %%A in (%ORIGINAL_ARGS%) do (
    if /i "%%A"=="--release" set BUILD_TYPE=Release
    if /i "%%A"=="--run-test" set RUN_AFTER_BUILD=1
    if /i "%%A"=="--clean" set CLEAN_BUILD=1
    if /i "%%A"=="--verbose" set VERBOSE_BUILD=1
)

if not defined CMAKE_GENERATOR set CMAKE_GENERATOR=Visual Studio 17 2022

if %CLEAN_BUILD%==1 (
    echo Cleaning build directory...
    rd /s /q build
)

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
set BUILD_CMD=cmake --build build --config %BUILD_TYPE%
if %VERBOSE_BUILD%==1 set BUILD_CMD=%BUILD_CMD% --verbose
%BUILD_CMD% -j

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

if %RUN_AFTER_BUILD%==1 (
    build\bin\%BUILD_TYPE%\test_project.exe
)

endlocal