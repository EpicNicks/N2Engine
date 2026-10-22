# Notes (particles-gpu-probe, 2026-10-13)
Approved deviations (accepted by the user):
- lua_project/src/Main.cpp is outside the allowed paths; one-line edit (.projectPath = projectDir.string()) was the only fix for the high finding
Real CUDA context creation and the GPU scene are verified only on the user's Windows GPU PC (later).
Unverified: Real CUDA context creation and GPU scene on a Windows GPU PC
