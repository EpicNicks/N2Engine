#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/Logger.hpp"

namespace N2Engine::Editor
{
    /// "info", "warn" or "error" (exactly, lowercase). The Logger has no Debug level, so there is no "debug".
    [[nodiscard]] std::optional<Logger::LogLevel> ParseLogLevel(std::string_view text);

    /// "INFO", "WARN" or "ERROR"
    [[nodiscard]] std::string_view LogLevelName(Logger::LogLevel level);

    /// A UTC time in ISO 8601 with milliseconds, "2026-10-14T12:34:56.789Z", from Unix milliseconds. Plain arithmetic, so it
    /// needs no time zone database and gives the same text on every platform.
    [[nodiscard]] std::string FormatLogTimestamp(int64_t unixMilliseconds);

    /// The text with every control character (below 0x20, and 0x7F) written out as an escape: \n, \r, \t, or \xNN. A
    /// log line is one line, so text a client supplied can't end it and start a line that looks like the host's own (an
    /// "audit:" line), and can't send escape sequences to a terminal.
    [[nodiscard]] std::string EscapeLogText(std::string_view text);

    /// One line of the host's log, without the newline: "<timestamp> <LEVEL> <message>", the message escaped
    /// (EscapeLogText), for example "2026-10-14T12:34:56.789Z INFO audit: user=bob role=editor command=CreateEntity peer=10.0.0.5"
    [[nodiscard]] std::string FormatLogLine(int64_t unixMilliseconds, Logger::LogLevel level, std::string_view message);

    /// Takes one finished line, newline included. Must not throw (the sink ignores one that does) and must not log.
    using LogOutput = std::function<void(std::string_view line)>;

    /// Writes each line to the process's stderr (the C stream, so a std::cerr that the Logger redirects can't loop back
    /// into it) and flushes it. What systemd's journal and `docker logs` collect. A closed or full stderr loses the line
    /// and nothing more.
    [[nodiscard]] LogOutput StderrLogOutput();

    /// Appends each line to the file at `path`, flushing each. It is opened (and created, with mode 0600 on POSIX, so the
    /// audit lines aren't readable by others; an existing file keeps the mode it has) once, here: an error names the
    /// path and the reason, for a path that can't be opened for appending or isn't a regular file. A line that
    /// can't be written (a full disk) is lost and nothing more. There is no rotation: logrotate with copytruncate works
    /// (the file is opened for append), or restart the host.
    [[nodiscard]] std::expected<LogOutput, std::string> OpenLogFileOutput(const std::string &path);

    /**
     * Writes every Logger line at or above `minLevel` to its outputs, one formatted line each (FormatLogLine), from the
     * moment it is constructed until it is destroyed. It subscribes to Logger::logEvent, so it runs on the thread that
     * logged, holding the Logger's lock: lines come out whole and in order, and the outputs must not wait on another thread
     * that logs. It never changes what other subscribers (the editor's PollEvents ring, a Debug build's console) receive.
     */
    class LogSink
    {
    public:
        /// The current time in Unix milliseconds; the default is the system clock
        using Clock = std::function<int64_t()>;

        LogSink(Logger::LogLevel minLevel, std::vector<LogOutput> outputs, Clock clock = {});
        ~LogSink();

        LogSink(const LogSink &) = delete;
        LogSink &operator=(const LogSink &) = delete;

    private:
        void Write(std::string_view message, Logger::LogLevel level);

        Logger::LogLevel _minLevel;
        std::vector<LogOutput> _outputs;
        Clock _clock;
        size_t _subscription = 0;
    };
}
