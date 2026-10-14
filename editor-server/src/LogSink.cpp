#include "editor-server/LogSink.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <memory>
#include <system_error>
#include <utility>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace N2Engine::Editor
{
    namespace
    {
        int64_t SystemClockUnixMilliseconds()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        }

        struct FileCloser
        {
            void operator()(std::FILE *file) const
            {
                if (file != nullptr)
                {
                    std::fclose(file);
                }
            }
        };
    }

    std::optional<Logger::LogLevel> ParseLogLevel(const std::string_view text)
    {
        if (text == "info")
        {
            return Logger::LogLevel::Info;
        }
        if (text == "warn")
        {
            return Logger::LogLevel::Warn;
        }
        if (text == "error")
        {
            return Logger::LogLevel::Error;
        }
        return std::nullopt;
    }

    std::string_view LogLevelName(const Logger::LogLevel level)
    {
        switch (level)
        {
        case Logger::LogLevel::Warn:
            return "WARN";
        case Logger::LogLevel::Error:
            return "ERROR";
        case Logger::LogLevel::Info:
        default:
            return "INFO";
        }
    }

    std::string FormatLogTimestamp(const int64_t unixMilliseconds)
    {
        constexpr int64_t MillisecondsPerDay = 86'400'000;
        // Floor division, so a time before 1970 counts back from midnight
        int64_t days = unixMilliseconds / MillisecondsPerDay;
        int64_t ofDay = unixMilliseconds % MillisecondsPerDay;
        if (ofDay < 0)
        {
            ofDay += MillisecondsPerDay;
            --days;
        }

        // The civil date of a day count (Howard Hinnant's civil_from_days), proleptic Gregorian
        const int64_t shifted = days + 719'468;
        const int64_t era = (shifted >= 0 ? shifted : shifted - 146'096) / 146'097;
        const int64_t dayOfEra = shifted - era * 146'097;
        const int64_t yearOfEra = (dayOfEra - dayOfEra / 1'460 + dayOfEra / 36'524 - dayOfEra / 146'096) / 365;
        const int64_t dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
        const int64_t monthIndex = (5 * dayOfYear + 2) / 153;
        const int64_t day = dayOfYear - (153 * monthIndex + 2) / 5 + 1;
        const int64_t month = monthIndex < 10 ? monthIndex + 3 : monthIndex - 9;
        const int64_t year = yearOfEra + era * 400 + (month <= 2 ? 1 : 0);

        const int64_t hour = ofDay / 3'600'000;
        const int64_t minute = ofDay / 60'000 % 60;
        const int64_t second = ofDay / 1'000 % 60;
        const int64_t millisecond = ofDay % 1'000;
        return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z", year, month, day, hour, minute, second,
                           millisecond);
    }

    std::string EscapeLogText(const std::string_view text)
    {
        std::string result;
        result.reserve(text.size());
        for (const char c : text)
        {
            const auto byte = static_cast<unsigned char>(c);
            if (byte >= 0x20 && byte != 0x7F)
            {
                result += c;
            }
            else if (c == '\n')
            {
                result += "\\n";
            }
            else if (c == '\r')
            {
                result += "\\r";
            }
            else if (c == '\t')
            {
                result += "\\t";
            }
            else
            {
                result += std::format("\\x{:02X}", static_cast<unsigned>(byte));
            }
        }
        return result;
    }

    std::string FormatLogLine(const int64_t unixMilliseconds, const Logger::LogLevel level, const std::string_view message)
    {
        return std::format("{} {} {}", FormatLogTimestamp(unixMilliseconds), LogLevelName(level), EscapeLogText(message));
    }

    LogOutput StderrLogOutput()
    {
        return [](const std::string_view line)
        {
            // The C stream, not std::cerr: a Debug build redirects std::cerr into the Logger, which would log this line again
            (void)std::fwrite(line.data(), 1, line.size(), stderr);
            (void)std::fflush(stderr);
        };
    }

    std::expected<LogOutput, std::string> OpenLogFileOutput(const std::string &path)
    {
        if (path.empty())
        {
            return std::unexpected("--log-file: the path is empty");
        }

        std::FILE *raw = nullptr;
        std::string reason;
#ifdef _WIN32
        const std::u8string utf8(path.begin(), path.end());
        const std::filesystem::path native(utf8);
        raw = _wfopen(native.c_str(), L"ab");
        if (raw == nullptr)
        {
            reason = std::generic_category().message(errno);
        }
#else
        // Created private (the umask only narrows it): the audit lines name users and peers
        const int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
        if (fd < 0)
        {
            reason = std::generic_category().message(errno);
        }
        else
        {
            struct stat info{};
            if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode))
            {
                reason = "it is not a regular file";
                ::close(fd);
            }
            else
            {
                raw = ::fdopen(fd, "a");
                if (raw == nullptr)
                {
                    reason = std::generic_category().message(errno);
                    ::close(fd);
                }
            }
        }
#endif
        if (raw == nullptr)
        {
            return std::unexpected(std::format("--log-file: {} can't be opened for appending: {}", path, reason));
        }

        std::shared_ptr<std::FILE> file(raw, FileCloser{});
        return LogOutput(
            [file](const std::string_view line)
            {
                // A full disk or a removed volume loses the line, never the host
                if (std::fwrite(line.data(), 1, line.size(), file.get()) == line.size())
                {
                    (void)std::fflush(file.get());
                }
            });
    }

    LogSink::LogSink(const Logger::LogLevel minLevel, std::vector<LogOutput> outputs, Clock clock)
        : _minLevel(minLevel), _outputs(std::move(outputs)),
          _clock(clock ? std::move(clock) : Clock(SystemClockUnixMilliseconds))
    {
        _subscription = Logger::logEvent += [this](const std::string_view message, const Logger::LogLevel level)
        { Write(message, level); };
    }

    LogSink::~LogSink()
    {
        // Once this returns no call of Write is running (dispatch holds the Logger's lock), so the members can go
        Logger::logEvent -= _subscription;
    }

    void LogSink::Write(const std::string_view message, const Logger::LogLevel level)
    {
        if (static_cast<int>(level) < static_cast<int>(_minLevel))
        {
            return;
        }
        try
        {
            std::string line = FormatLogLine(_clock(), level, message);
            line += '\n';
            for (const LogOutput &output : _outputs)
            {
                try
                {
                    output(line);
                }
                catch (...)
                {
                    // One output failing doesn't stop the next, and logging must never throw
                }
            }
        }
        catch (...)
        {
            // (bad_alloc) A line is lost, never the host
        }
    }
}
