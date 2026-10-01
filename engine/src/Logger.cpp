#include "engine/Logger.hpp"
#include <iostream>
#include <mutex>

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

namespace
{
    // Guards the backlog, the subscribers and the stream redirectors' line buffers. Recursive, so a
    // subscriber that logs (or writes to a redirected stream) re-enters on its own thread. Never destroyed,
    // so logging during static destruction still finds it.
    std::recursive_mutex &LogMutex()
    {
        static auto *mutex = new std::recursive_mutex();
        return *mutex;
    }
}

Logger::LogEventHandler Logger::logEvent;
std::atomic<bool> Logger::broadcastUnbroadcastLogs = false;
std::queue<Logger::QueuedLog> Logger::_logQueue;

size_t Logger::LogEventHandler::operator+=(const std::function<void(std::string_view, LogLevel)> &func)
{
    std::lock_guard lock(LogMutex());
    return _handler += func;
}

void Logger::LogEventHandler::operator-=(const size_t id)
{
    std::lock_guard lock(LogMutex());
    _handler -= id;
}

void Logger::LogEventHandler::operator()(const std::string_view message, const LogLevel level)
{
    std::lock_guard lock(LogMutex());
    _handler(message, level);
}

size_t Logger::LogEventHandler::GetSubscriberCount() const
{
    std::lock_guard lock(LogMutex());
    return _handler.GetSubscriberCount();
}

void Logger::Log(std::string_view log, LogLevel level)
{
    // Held across the backlog check and the delivery: another thread's log waits for this one to finish
    std::lock_guard lock(LogMutex());
    if (broadcastUnbroadcastLogs && logEvent.GetSubscriberCount() == 0)
    {
        _logQueue.push({std::string(log), level});
    }
    else
    {
        // Take the backlog first: a subscriber that logs re-enters here, and a nested drain used to
        // pop the entry the outer loop was still holding a reference to (delivering logs twice)
        std::queue<QueuedLog> backlog;
        backlog.swap(_logQueue);
        while (!backlog.empty())
        {
            const QueuedLog queuedLog = std::move(backlog.front());
            backlog.pop();
            logEvent(queuedLog.message, queuedLog.level);
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
    // Once per process: a second Application::Init used to add another console subscriber, printing
    // every line twice
    static bool initialized = false;
    if (initialized)
    {
        return;
    }
    initialized = true;

    auto originalStdout = RedirectStdout(LogLevel::Info, false);
    RedirectStderr(LogLevel::Error, false);

    // Deliberately never destroyed: it's created after statics such as the Application singleton,
    // so a unique_ptr here was destroyed first and later log calls during their teardown crashed
    static auto *originalStdoutStream = new std::ostream(originalStdout);
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

    // Any thread may write to a redirected std::cout; each thread builds its own line
    std::lock_guard lock(LogMutex());
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
        FlushLine();
    }
    else if (c != '\r')
    {
        lineBuffers[std::this_thread::get_id()] += static_cast<char>(c);
    }

    return result;
}

void Logger::StreamRedirector::LoggerStreambuf::FlushLine()
{
    const auto it = lineBuffers.find(std::this_thread::get_id());
    if (it == lineBuffers.end())
    {
        return;
    }
    // Taken out first: a subscriber that writes to this stream re-enters on this thread
    const std::string line = std::move(it->second);
    lineBuffers.erase(it);
    if (!line.empty())
    {
        logEvent(line, logLevel);
    }
}

int Logger::StreamRedirector::LoggerStreambuf::sync()
{
    std::lock_guard lock(LogMutex());
    // Flush the calling thread's remaining content
    FlushLine();

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