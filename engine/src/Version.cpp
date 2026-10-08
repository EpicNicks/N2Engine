#include "engine/Version.hpp"

// CMake's project version (engine/CMakeLists.txt passes it)
#ifndef N2ENGINE_VERSION
#define N2ENGINE_VERSION "0.0.0"
#endif

namespace N2Engine
{
    std::string_view EngineVersion()
    {
        return N2ENGINE_VERSION;
    }
}
