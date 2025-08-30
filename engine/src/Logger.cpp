#include "engine/Logger.hpp"

void Logger::Log(std::string log, Logger::LogLevel level)
{
    logEvent(log, level);
}