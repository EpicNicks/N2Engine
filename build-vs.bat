@echo off
setlocal enabledelayedexpansion

set BUILD_TYPE=Debug
set RUN_EDITOR=0
set RUN_TESTS=0
set CLEAN_BUILD=0
set VERBOSE_BUILD=0
set CMAKE_GENERATOR=Visual Studio 17 2022
set TEST_FILTER=

set BUILD_TESTS=ON
set TESTS_EXPLICIT=0

REM Check for help flag first
if /i "%~1"=="--help" goto :show_help
if /i "%~1"=="-h" goto :show_help
if /i "%~1"=="/?" goto :show_help

REM Parse arguments
:parse_args
if "%~1"=="" goto :done_parsing

if /i "%~1"=="--release" (
    set BUILD_TYPE=Release
    shift
    goto :parse_args
)

if /i "%~1"=="--debug" (
    set BUILD_TYPE=Debug
    shift
    goto :parse_args
)

if /i "%~1"=="--run" (
    set RUN_EDITOR=1
    shift
    goto :parse_args
)

if /i "%~1"=="--run-tests" (
    set RUN_TESTS=1
    shift
    goto :parse_args
)

if /i "%~1"=="--clean" (
    set CLEAN_BUILD=1
    shift
    goto :parse_args
)

if /i "%~1"=="--verbose" (
    set VERBOSE_BUILD=1
    shift
    goto :parse_args
)

REM Parse --filter=value or --filter value
if /i "%~1"=="--filter" (
    if not "%~2"=="" (
        set TEST_FILTER=%~2
        shift
        shift
        goto :parse_args
    )
)

echo %~1 | findstr /i /b "^--filter=" >nul
if not errorlevel 1 (
    set ARG=%~1
    set TEST_FILTER=!ARG:--filter=!
    shift
    goto :parse_args
)

REM Parse --generator=value or --generator value
if /i "%~1"=="--generator" (
    if not "%~2"=="" (
        set CMAKE_GENERATOR=%~2
        shift
        shift
        goto :parse_args
    )
)

echo %~1 | findstr /i /b "^--generator=" >nul
if not errorlevel 1 (
    set ARG=%~1
    set CMAKE_GENERATOR=!ARG:--generator=!
    shift
    goto :parse_args
)

if /i "%~1"=="--skip-tests" (
    set BUILD_TESTS=OFF
    set TESTS_EXPLICIT=1
    shift
    goto :parse_args
)

if /i "%~1"=="--tests" (
    set BUILD_TESTS=ON
    set TESTS_EXPLICIT=1
    shift
    goto :parse_args
)

REM Unknown argument
echo Warning: Unknown argument: %~1
shift
goto :parse_args

:done_parsing

REM Default policy: Release disables tests unless explicitly overridden
if "%BUILD_TYPE%"=="Release" if "%TESTS_EXPLICIT%"=="0" (
    set BUILD_TESTS=OFF
)

if %CLEAN_BUILD%==1 (
    echo Cleaning build directory...
    rd /s /q build
)

echo.
echo Configuring N2Engine...
echo   Generator:  %CMAKE_GENERATOR%
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
    echo Tests:  build\bin\%BUILD_TYPE%\*_tests.exe
)

echo.

REM Run tests if requested
if %RUN_TESTS%==1 (
    if "%BUILD_TESTS%"=="ON" (
        echo.
        echo ========================================
        echo Running tests...
        echo ========================================
        echo.

        set CTEST_CMD=ctest --test-dir build --build-config %BUILD_TYPE% --output-on-failure

        if not "%TEST_FILTER%"=="" (
            echo Filter: %TEST_FILTER%
            echo.
            set CTEST_CMD=!CTEST_CMD! -R "%TEST_FILTER%"
        )

        !CTEST_CMD!

        if errorlevel 1 (
            echo.
            echo ========================================
            echo Tests FAILED!
            echo ========================================
            exit /b 1
        )
        echo.
        echo ========================================
        echo All tests PASSED!
        echo ========================================
    ) else (
        echo.
        echo Tests are disabled -- cannot run.
        echo Use --tests to enable test building.
    )
)

REM Run editor if requested
if %RUN_EDITOR%==1 (
    echo.
    echo ========================================
    echo Launching editor...
    echo ========================================
    echo.
    start "" "build\bin\%BUILD_TYPE%\test_project.exe"
)

endlocal
exit /b 0

:show_help
echo.
echo N2Engine Build Script
echo =====================
echo.
echo Usage: build.bat [OPTIONS]
echo.
echo Options:
echo   --help, -h, /?        Show this help message
echo.
echo Build Configuration:
echo   --debug               Build in Debug mode (default)
echo   --release             Build in Release mode
echo   --clean               Clean the build directory before building
echo   --verbose             Enable verbose build output
echo.
echo Generator:
echo   --generator "NAME"    Specify CMake generator
echo   --generator="NAME"    (default: "Visual Studio 17 2022")
echo.
echo   Common generators:
echo     - "Visual Studio 17 2022"
echo     - "Visual Studio 16 2019"
echo     - "Ninja"
echo     - "Ninja Multi-Config"
echo     - "Unix Makefiles"
echo     - "MinGW Makefiles"
echo.
echo Testing:
echo   --tests               Force enable unit tests (default: ON in Debug)
echo   --skip-tests          Force disable unit tests (default: OFF in Release)
echo   --run-tests           Run all tests after successful build
echo   --filter "PATTERN"    Only run tests matching PATTERN (regex)
echo   --filter="PATTERN"
echo.
echo Running:
echo   --run                 Launch editor after successful build
echo   --run-tests           Run tests after successful build
echo.
echo Examples:
echo   build.bat
echo   build.bat --release --clean
echo   build.bat --generator="Ninja" --verbose
echo   build.bat --release --tests --run-tests
echo   build.bat --run-tests --filter="Serialization"
echo   build.bat --run-tests --filter="math_tests"
echo   build.bat --run                              (launches editor)
echo   build.bat --run --run-tests                  (runs tests, then launches editor)
echo.
echo Notes:
echo   - Tests are enabled by default in Debug builds
echo   - Tests are disabled by default in Release builds
echo   - Use --tests or --skip-tests to override the default behavior
echo.
endlocal
exit /b 0