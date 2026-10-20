#pragma once

#include <string_view>

#include "engine/Logger.hpp"

namespace N2Engine::Editor::ServerLog
{
    /// How the editor host logs about itself. A line the host writes names its users, roles, peers, files or limits, so
    /// it is Admin by default: PollEvents gives it to an admin (and to a host with no authentication) and to nobody
    /// else. The sensitivity is decided here, at the call site, never by looking at the text of the line.
    ///
    /// The engine's own lines and the project's scripts and components log through Logger directly and are Public (every
    /// role reads them); the host uses PublicInfo only for a line that holds nothing but a res:// path or a name a
    /// client chose.
    inline void Info(const std::string_view message)
    {
        Logger::Info(message, Logger::LogSensitivity::Admin);
    }

    inline void Warn(const std::string_view message)
    {
        Logger::Warn(message, Logger::LogSensitivity::Admin);
    }

    inline void Error(const std::string_view message)
    {
        Logger::Error(message, Logger::LogSensitivity::Admin);
    }

    /// A line every role may read (see above): what the host did to the project through a res:// path
    inline void PublicInfo(const std::string_view message)
    {
        Logger::Info(message, Logger::LogSensitivity::Public);
    }
}
