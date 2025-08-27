#pragma once

#include <string>
#include "EventHandler.hpp"

class Logger
{
public:
    enum class LogLevel
    {
        Info,
        Warn,
        Error
    };

    static EventHandler<std::string, LogLevel> logEvent;

    static void Log(std::string log, LogLevel level);
};