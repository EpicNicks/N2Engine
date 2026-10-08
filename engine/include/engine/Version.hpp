#pragma once

#include <string_view>

namespace N2Engine
{
    /// The engine's version, "major.minor.patch": CMake's project version (the root CMakeLists.txt). A project file
    /// records the version that created it (IO::ProjectFile::engineVersion).
    [[nodiscard]] std::string_view EngineVersion();
}
