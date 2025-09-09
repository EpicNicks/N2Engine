@echo off
setlocal

cmake -S . -B build
cmake --build build

endlocal
