// The web size probe's baseline: a program that does nothing with Jolt (cmake/jolt/CMakeLists.txt). Its .wasm is what the
// probe's .wasm is measured against.
#include <cstdio>

int main()
{
    std::printf("[JOLT-SIZE] baseline\n");
    return 0;
}
