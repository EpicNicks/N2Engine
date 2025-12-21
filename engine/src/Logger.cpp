#include "engine/Logger.hpp"
#include <iostream>

// Platform detection for TTY checking
#ifdef _WIN32
    #include <io.h>
    #define ISATTY _isatty
    #define FILENO _fileno
#else
    #include <unistd.h>
    #define ISATTY isatty
    #define FILENO fileno
#endif

#if defined(_WIN32)
    #if __has_include(<windows.h>)
        #include <windows.h>
        #define HAS_WINDOWS_CONSOLE_API
    #endif
#endif

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

static bool SupportsColor(std::ostream& stream)
{
    if (&stream == &std::cout)
    {
        if (!ISATTY(FILENO(stdout)))
            return false;
    }
    else if (&stream == &std::cerr)
    {
        if (!ISATTY(FILENO(stderr)))
            return false;
    }

#ifdef HAS_WINDOWS_CONSOLE_API
    // Enable Virtual Terminal Processing on Windows 10+
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hConsole != INVALID_HANDLE_VALUE)
    {
        DWORD mode = 0;
        if (GetConsoleMode(hConsole, &mode))
        {
            mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(hConsole, mode);
            return true;
        }
    }
    return false;
#else
    // On Unix-like systems (or Windows without console API), check TERM environment variable
    const char* term = std::getenv("TERM");
    if (term == nullptr)
        return false;

    std::string termStr(term);
    // Most modern terminals support colors, but "dumb" terminal doesn't
    return termStr != "dumb";
#endif
}

static const char* GetColoredLevelString(Logger::LogLevel level, bool useColors)
{
    if (!useColors)
    {
        switch (level)
        {
        case Logger::LogLevel::Info:
            return "INFO";
        case Logger::LogLevel::Warn:
            return "WARN";
        case Logger::LogLevel::Error:
            return "ERROR";
        default:
            return "INFO";
        }
    }

    // ANSI color codes
    switch (level)
    {
    case Logger::LogLevel::Info:
        return "\033[34mINFO\033[0m";  // Blue
    case Logger::LogLevel::Warn:
        return "\033[33mWARN\033[0m";  // Yellow
    case Logger::LogLevel::Error:
        return "\033[31mERROR\033[0m"; // Red
    default:
        return "\033[34mINFO\033[0m";
    }
}

void Logger::InitializeDebugConsoleHelper()
{
    auto originalStdout = RedirectStdout(LogLevel::Info, false);
    RedirectStderr(LogLevel::Error, false);

    static auto originalStdoutStream = std::make_unique<std::ostream>(originalStdout);
    static bool useColors = SupportsColor(std::cout);

    logEvent += [](const std::string_view msg, const LogLevel level)
    {
        const char* levelStr = GetColoredLevelString(level, useColors);
        *originalStdoutStream << "[" << levelStr << "] " << msg << std::endl;
    };
}

Logger::StreamRedirector::LoggerStreambuf::LoggerStreambuf(
    std::streambuf *original,
    const LogLevel level,
    const bool echo) : originalBuf(original), logLevel(level), echoToOriginal(echo) {}

int Logger::StreamRedirector::LoggerStreambuf::overflow(const int c)
{
    if (c == EOF)
    {
        return EOF;
    }

    int result = EOF;
    if (echoToOriginal && originalBuf)
    {
        result = originalBuf->sputc(static_cast<char>(c));
    }
    else
    {
        result = c;
    }

    if (c == '\n')
    {
        if (!lineBuffer.empty())
        {
            logEvent(lineBuffer, logLevel);
            lineBuffer.clear();
        }
    }
    else if (c != '\r')
    {
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
void Logger::AddStreamRedirector(std::unique_ptr<StreamRedirector> &&streamRedirector)
{
    _streamRedirectors.push_back(std::move(streamRedirector));
}