#pragma once

#include <string>
#include "engine/base/EventHandler.hpp"

// meant to be received by any GUI or shell and otherwise not used elsewhere directly in the engine
class Logger
{
public:
    enum class LogLevel
    {
        Info,
        Warn,
        Error
    };

    static N2Engine::Base::EventHandler<std::string, LogLevel> logEvent;

    static void Log(const std::string &log, LogLevel level);

    // helpers
    static void Info(const std::string &log);
    static void Warn(const std::string &log);
    static void Error(const std::string &log);
};