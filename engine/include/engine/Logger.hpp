#pragma once

#include <string>
#include <ostream>
#include <streambuf>
#include <memory>
#include <string_view>
#include <queue>

#include "engine/base/EventHandler.hpp"

namespace N2Engine
{
    // meant to be received by any GUI or shell and otherwise not used elsewhere directly in the engine
    class Logger
    {
    public:
        enum class LogLevel
        {
            Info,
            Warn,
            Error
        };

        static Base::EventHandler<std::string_view, LogLevel> logEvent;
        static bool broadcastUnbroadcastLogs;

        static void Log(std::string_view log, LogLevel level);

        // helpers

        static void Info(std::string_view log);
        static void Warn(std::string_view log);
        static void Error(std::string_view log);

        // General color/coded debugging sent to stdout
        static void InitializeDebugConsoleHelper();

        class StreamRedirector
        {
        public:
            std::streambuf *originalBuf;

        private:
            std::ostream &stream;

            class LoggerStreambuf : public std::streambuf
            {
            private:
                std::streambuf *originalBuf;
                LogLevel logLevel;
                std::string lineBuffer;
                bool echoToOriginal;

            public:
                LoggerStreambuf(std::streambuf *original, LogLevel level, bool echo = true);

            protected:
                int overflow(int c) override;
                int sync() override;
            };

            std::unique_ptr<LoggerStreambuf> logger_buf;

        public:
            // Constructor takes any output stream
            explicit StreamRedirector(std::ostream &stream, LogLevel level = LogLevel::Info, bool echoToOriginal = true);
            ~StreamRedirector();

            // Prevent copying and moving for safety
            StreamRedirector(const StreamRedirector &) = delete;
            StreamRedirector &operator=(const StreamRedirector &) = delete;
            StreamRedirector(StreamRedirector &&) = delete;
            StreamRedirector &operator=(StreamRedirector &&) = delete;
        };

        // Convenience factory methods
        static std::streambuf *RedirectStdout(LogLevel level = LogLevel::Info, bool echo = true);
        static std::streambuf *RedirectStderr(LogLevel level = LogLevel::Error, bool echo = true);
        static std::streambuf *RedirectStream(std::ostream &stream, LogLevel level = LogLevel::Info, bool echo = true);

    private:
        struct QueuedLog
        {
            std::string message;
            LogLevel level;
        };

        static std::queue<QueuedLog> _logQueue;
        static std::vector<std::unique_ptr<StreamRedirector>> _streamRedirectors;
        static void AddStreamRedirector(std::unique_ptr<StreamRedirector> &&streamRedirector);
    };
}