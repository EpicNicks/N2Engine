@echo off
setlocal

set BUILD_TYPE=Debug
set RUN_AFTER_BUILD=0
set CLEAN_BUILD=0
set VERBOSE_BUILD=0

set BUILD_TESTS=ON
set TESTS_EXPLICIT=0

set ORIGINAL_ARGS=%*

for %%A in (%ORIGINAL_ARGS%) do (
    if /i "%%A"=="--release" set BUILD_TYPE=Release
    if /i "%%A"=="--run" set RUN_AFTER_BUILD=1
    if /i "%%A"=="--clean" set CLEAN_BUILD=1
    if /i "%%A"=="--verbose" set VERBOSE_BUILD=1

    if /i "%%A"=="--skip-tests" (
        set BUILD_TESTS=OFF
        set TESTS_EXPLICIT=1
    )

    if /i "%%A"=="--tests" (
        set BUILD_TESTS=ON
        set TESTS_EXPLICIT=1
    )
)

REM Default policy: Release disables tests unless explicitly overridden
if "%BUILD_TYPE%"=="Release" if "%TESTS_EXPLICIT%"=="0" (
    set BUILD_TESTS=OFF
)

if not defined CMAKE_GENERATOR set CMAKE_GENERATOR=Visual Studio 17 2022

if %CLEAN_BUILD%==1 (
    echo Cleaning build directory...
    rd /s /q build
)

echo.
echo Configuring N2Engine with Visual Studio...
echo   Build type: %BUILD_TYPE%
echo   Unit tests: %BUILD_TESTS%
echo.

cmake -S . -B build ^
    -G "%CMAKE_GENERATOR%" ^
    -A x64 ^
    -DN2ENGINE_BUILD_TESTS=%BUILD_TESTS%

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
    exit /b 1
)

echo.
echo ========================================
echo Build SUCCESS!
echo ========================================
echo Editor: build\bin\%BUILD_TYPE%\editor.exe

if "%BUILD_TESTS%"=="ON" (
    echo Tests:  build\bin\%BUILD_TYPE%\engine_tests.exe
)

echo.

if %RUN_AFTER_BUILD%==1 (
    if "%BUILD_TESTS%"=="ON" (
        build\bin\%BUILD_TYPE%\engine_tests.exe
    ) else (
        echo Tests are disabled -- cannot run.
    )
)

endlocal
