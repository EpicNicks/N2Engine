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

    static void Log(std::string log, LogLevel level);
};