@echo off
setlocal

cmake -S . -B build -G "MinGW Makefiles" ^
    -DCMAKE_C_COMPILER=gcc ^
    -DCMAKE_CXX_COMPILER=g++ ^
    -DCMAKE_BUILD_TYPE=Debug ^
    -DCMAKE_CXX_FLAGS_DEBUG="-g -O0"

cmake --build build --config Debug

endlocal