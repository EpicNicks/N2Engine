#pragma once

#include <string>
#include "engine/base/EventHandler.hpp"

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