#include "engine/Logger.hpp"

void Logger::Log(std::string log, Logger::LogLevel level)
{
    logEvent(log, level);
}

N2Engine::Base::EventHandler<std::string, Logger::LogLevel> Logger::logEvent;