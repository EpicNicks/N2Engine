#pragma once

#include <string>
#include <ostream>
#include <streambuf>
#include <memory>
#include <string_view>
#include <queue>
#include <atomic>
#include <functional>
#include <thread>
#include <unordered_map>

#include "engine/base/EventHandler.hpp"

namespace N2Engine
{
    // meant to be received by any GUI or shell and otherwise not used elsewhere directly in the engine
    //
    // Thread-safe: Log, subscribing and unsubscribing may happen on any thread. They share one recursive
    // lock, held while the backlog and the subscribers run, so a log is delivered whole (never interleaved
    // with another thread's) and on the logging thread. Being recursive, a subscriber that logs re-enters
    // on its own thread as before. Subscribers must not wait on another thread that logs (that thread
    // blocks on the lock: a deadlock). Not async-signal-safe: don't log from a POSIX signal handler.
    class Logger
    {
    public:
        enum class LogLevel
        {
            Info,
            Warn,
            Error
        };

        /// An EventHandler whose subscribe, unsubscribe and dispatch hold the Logger's lock
        class LogEventHandler
        {
        public:
            size_t operator+=(const std::function<void(std::string_view, LogLevel)> &func);
            void operator-=(size_t id);
            void operator()(std::string_view message, LogLevel level);
            [[nodiscard]] size_t GetSubscriberCount() const;

        private:
            Base::EventHandler<std::string_view, LogLevel> _handler;
        };

        static LogEventHandler logEvent;
        static std::atomic<bool> broadcastUnbroadcastLogs;

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
                // One partial line per writing thread (under the Logger's lock), so lines written by two
                // threads at once each reach the log whole instead of mixed character by character
                std::unordered_map<std::thread::id, std::string> lineBuffers;
                bool echoToOriginal;

                /// Logs and clears the calling thread's partial line, if any
                void FlushLine();

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