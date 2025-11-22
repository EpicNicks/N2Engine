#include "engine/Logger.hpp"
#include <iostream>

using namespace N2Engine;

Base::EventHandler<std::string_view, Logger::LogLevel> Logger::logEvent;
bool Logger::broadcastUnbroadcastLogs = false;
std::queue<Logger::QueuedLog> Logger::_logQueue;

void Logger::Log(std::string_view log, LogLevel level)
{
    if (broadcastUnbroadcastLogs && logEvent.GetSubscriberCount() == 0)
    {
        _logQueue.push({std::string(log), level});
    }
    else
    {
        while (!_logQueue.empty())
        {
            auto &queuedLog = _logQueue.front();
            logEvent(queuedLog.message, queuedLog.level);
            _logQueue.pop();
        }
        logEvent(log, level);
    }
}

void Logger::Info(const std::string_view log)
{
    Log(log, LogLevel::Info);
}

void Logger::Warn(const std::string_view log)
{
    Log(log, LogLevel::Warn);
}

void Logger::Error(const std::string_view log)
{
    Log(log, LogLevel::Error);
}

Logger::StreamRedirector::LoggerStreambuf::LoggerStreambuf(
    std::streambuf *original,
    const LogLevel level,
    const bool echo) : originalBuf(original), logLevel(level), echoToOriginal(echo) {}

int Logger::StreamRedirector::LoggerStreambuf::overflow(const int c)
{
    if (c == EOF)
        return EOF;

    // Echo to original output if requested
    int result = EOF;
    if (echoToOriginal && originalBuf)
    {
        result = originalBuf->sputc(c);
    }
    else
    {
        result = c; // Successful write (not echoed)
    }

    // Build up line buffer
    if (c == '\n')
    {
        // Complete line - send to logger
        if (!lineBuffer.empty())
        {
            Logger::logEvent(lineBuffer, logLevel);
            lineBuffer.clear();
        }
    }
    else if (c != '\r')
    { // Skip carriage returns
        lineBuffer += static_cast<char>(c);
    }

    return result;
}

int Logger::StreamRedirector::LoggerStreambuf::sync()
{
    // Flush any remaining content
    if (!lineBuffer.empty())
    {
        Logger::logEvent(lineBuffer, logLevel);
        lineBuffer.clear();
    }

    if (echoToOriginal && originalBuf)
    {
        return originalBuf->pubsync();
    }
    return 0;
}

// StreamRedirector Implementation
Logger::StreamRedirector::StreamRedirector(
    std::ostream &stream,
    LogLevel level,
    bool echo_to_original) : stream(stream)
{
    originalBuf = stream.rdbuf();
    logger_buf = std::make_unique<LoggerStreambuf>(originalBuf, level, echo_to_original);
    stream.rdbuf(logger_buf.get());
}

Logger::StreamRedirector::~StreamRedirector()
{
    // Flush any remaining content before restoring
    logger_buf->pubsync();
    stream.rdbuf(originalBuf);
}

// Convenience Factory Methods
std::streambuf *Logger::RedirectStdout(const LogLevel level, const bool echo)
{
    return RedirectStream(std::cout, level, echo);
}

std::streambuf *Logger::RedirectStderr(const LogLevel level, const bool echo)
{
    return RedirectStream(std::cerr, level, echo);
}

std::streambuf *Logger::RedirectStream(std::ostream &stream, LogLevel level, bool echo)
{
    std::streambuf *originalStreambuf = stream.rdbuf();
    AddStreamRedirector(std::make_unique<StreamRedirector>(stream, level, echo));
    return originalStreambuf;
}

std::vector<std::unique_ptr<Logger::StreamRedirector>> Logger::_streamRedirectors;
void Logger::AddStreamRedirector(std::unique_ptr<Logger::StreamRedirector> &&streamRedirector)
{
    _streamRedirectors.push_back(std::move(streamRedirector));
}