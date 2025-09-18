#include "engine/Logger.hpp"

N2Engine::Base::EventHandler<std::string, Logger::LogLevel> Logger::logEvent;

void Logger::Log(const std::string &log, Logger::LogLevel level)
{
    logEvent(log, level);
}

void Logger::Info(const std::string &log)
{
    Log(log, LogLevel::Info);
}

void Logger::Warn(const std::string &log)
{
    Log(log, LogLevel::Warn);
}

void Logger::Error(const std::string &log)
{
    Log(log, LogLevel::Error);
}
