#include "editor-server/ShaderCook.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <sstream>
#include <iterator>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <crt_externs.h>
#include <mach-o/dyld.h>
#define N2_ENVIRON (*_NSGetEnviron())
#else
extern char **environ;
#define N2_ENVIRON environ
#endif
#endif

namespace N2Engine::Editor::ShaderCook
{
    namespace
    {
        std::string Env(const char *name)
        {
            const char *value = std::getenv(name);
            return value != nullptr ? std::string(value) : std::string();
        }

        bool IsFile(const std::filesystem::path &path)
        {
            std::error_code error;
            return std::filesystem::is_regular_file(path, error);
        }

        std::filesystem::path FindOnPath(const std::string &name)
        {
            const std::string pathVariable = Env("PATH");
#ifdef _WIN32
            constexpr char Separator = ';';
            const std::string file = name + ".exe";
#else
            constexpr char Separator = ':';
            const std::string &file = name;
#endif
            std::istringstream stream(pathVariable);
            std::string entry;
            while (std::getline(stream, entry, Separator))
            {
                if (entry.empty())
                {
                    continue;
                }
                const std::filesystem::path candidate = std::filesystem::path(entry) / file;
                if (IsFile(candidate))
                {
                    return candidate;
                }
            }
            return {};
        }

        /// The folder of the running executable (empty when the system will not say)
        std::filesystem::path ExecutableFolder()
        {
#ifdef _WIN32
            wchar_t buffer[MAX_PATH * 4];
            const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
            if (length == 0 || length >= std::size(buffer))
            {
                return {};
            }
            return std::filesystem::path(std::wstring(buffer, length)).parent_path();
#elif defined(__APPLE__)
            char buffer[4096];
            uint32_t size = sizeof(buffer);
            if (_NSGetExecutablePath(buffer, &size) != 0)
            {
                return {};
            }
            std::error_code error;
            return std::filesystem::weakly_canonical(buffer, error).parent_path();
#else
            std::error_code error;
            return std::filesystem::read_symlink("/proc/self/exe", error).parent_path();
#endif
        }

        std::string Trim(std::string text)
        {
            while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ' || text.back() == '\t'))
            {
                text.pop_back();
            }
            return text;
        }
    }

    Tooling FindTooling(const std::vector<std::filesystem::path> &searchFrom)
    {
        Tooling tooling;
        if (const std::string node = Env("N2ENGINE_NODE"); !node.empty())
        {
            if (IsFile(node))
            {
                tooling.node = node;
            }
            else
            {
                tooling.unavailable = "N2ENGINE_NODE names no file: " + node;
                return tooling;
            }
        }
        else
        {
            tooling.node = FindOnPath("node");
        }
        if (tooling.node.empty())
        {
            tooling.unavailable = "Node.js was not found (put node on PATH or set N2ENGINE_NODE): the shader cook runs on it";
            return tooling;
        }

        if (const std::string script = Env("N2SHADER_TOOL"); !script.empty())
        {
            if (IsFile(script))
            {
                tooling.script = script;
            }
            else
            {
                tooling.unavailable = "N2SHADER_TOOL names no file: " + script;
            }
            return tooling;
        }
        const std::filesystem::path relative = std::filesystem::path("tools") / "n2shader" / "n2shader.mjs";
        std::vector<std::filesystem::path> roots = searchFrom;
        roots.push_back(ExecutableFolder());
        for (std::filesystem::path folder : roots)
        {
            std::error_code error;
            if (folder.empty())
            {
                continue;
            }
            folder = std::filesystem::absolute(folder, error);
            for (int up = 0; up < 10 && !folder.empty(); ++up)
            {
                if (IsFile(folder / relative))
                {
                    tooling.script = folder / relative;
                    return tooling;
                }
                const std::filesystem::path parent = folder.parent_path();
                if (parent == folder)
                {
                    break;
                }
                folder = parent;
            }
        }
        tooling.unavailable = "tools/n2shader/n2shader.mjs was not found near the host (set N2SHADER_TOOL to it): the shader cook is that script";
        return tooling;
    }

    std::optional<ProcessOutput> RunProcess(const std::filesystem::path &program, const std::vector<std::string> &arguments,
                                            const std::chrono::milliseconds timeout, std::string &error)
    {
        using Clock = std::chrono::steady_clock;
        const auto deadline = Clock::now() + timeout;
        ProcessOutput result;
#ifdef _WIN32
        auto quote = [](const std::wstring &text) {
            if (!text.empty() && text.find_first_of(L" \t\"") == std::wstring::npos)
            {
                return text;
            }
            std::wstring out = L"\"";
            std::size_t backslashes = 0;
            for (const wchar_t c : text)
            {
                if (c == L'\\')
                {
                    ++backslashes;
                    continue;
                }
                if (c == L'"')
                {
                    out.append(backslashes * 2 + 1, L'\\');
                }
                else
                {
                    out.append(backslashes, L'\\');
                }
                backslashes = 0;
                out.push_back(c);
            }
            out.append(backslashes * 2, L'\\');
            out.push_back(L'"');
            return out;
        };
        std::wstring commandLine = quote(program.wstring());
        for (const std::string &argument : arguments)
        {
            commandLine += L' ';
            commandLine += quote(std::filesystem::path(std::u8string(argument.begin(), argument.end())).wstring());
        }

        SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE readEnd = nullptr;
        HANDLE writeEnd = nullptr;
        if (!CreatePipe(&readEnd, &writeEnd, &inheritable, 0))
        {
            error = "the output pipe could not be made";
            return std::nullopt;
        }
        SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
        HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0, nullptr);

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = input;
        startup.hStdOutput = writeEnd;
        startup.hStdError = writeEnd;
        PROCESS_INFORMATION process{};
        const BOOL started = CreateProcessW(program.c_str(), commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                            nullptr, &startup, &process);
        const DWORD startError = started ? 0 : GetLastError();
        CloseHandle(writeEnd);
        if (input != INVALID_HANDLE_VALUE)
        {
            CloseHandle(input);
        }
        if (!started)
        {
            CloseHandle(readEnd);
            error = std::format("{} could not be started (Windows error {})", program.string(), startError);
            return std::nullopt;
        }
        CloseHandle(process.hThread);

        char buffer[4096];
        for (;;)
        {
            DWORD available = 0;
            const BOOL peeked = PeekNamedPipe(readEnd, nullptr, 0, nullptr, &available, nullptr);
            if (peeked && available > 0)
            {
                DWORD got = 0;
                if (ReadFile(readEnd, buffer, std::min<DWORD>(available, sizeof(buffer)), &got, nullptr) && got > 0)
                {
                    result.output.append(buffer, got);
                    if (result.output.size() > 4 * 1024 * 1024)
                    {
                        result.output.resize(4 * 1024 * 1024);
                    }
                }
                continue;
            }
            if (!peeked || WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0)
            {
                // the pipe is drained when the process is gone and nothing is left to read
                DWORD left = 0;
                if (peeked && PeekNamedPipe(readEnd, nullptr, 0, nullptr, &left, nullptr) && left > 0)
                {
                    continue;
                }
                break;
            }
            if (Clock::now() >= deadline)
            {
                TerminateProcess(process.hProcess, 1);
                result.timedOut = true;
                break;
            }
            Sleep(10);
        }
        WaitForSingleObject(process.hProcess, 5000);
        DWORD code = 1;
        GetExitCodeProcess(process.hProcess, &code);
        result.exitCode = static_cast<int>(code);
        CloseHandle(process.hProcess);
        CloseHandle(readEnd);
        return result;
#else
        int pipeEnds[2];
        if (pipe(pipeEnds) != 0)
        {
            error = "the output pipe could not be made";
            return std::nullopt;
        }
        fcntl(pipeEnds[0], F_SETFD, FD_CLOEXEC);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, pipeEnds[1], 1);
        posix_spawn_file_actions_adddup2(&actions, pipeEnds[1], 2);
        posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addclose(&actions, pipeEnds[1]);

        std::vector<std::string> storage;
        storage.push_back(program.string());
        storage.insert(storage.end(), arguments.begin(), arguments.end());
        std::vector<char *> argv;
        for (std::string &text : storage)
        {
            argv.push_back(text.data());
        }
        argv.push_back(nullptr);

        pid_t pid = 0;
        const int spawned = posix_spawn(&pid, storage[0].c_str(), &actions, nullptr, argv.data(), N2_ENVIRON);
        posix_spawn_file_actions_destroy(&actions);
        close(pipeEnds[1]);
        if (spawned != 0)
        {
            close(pipeEnds[0]);
            error = std::format("{} could not be started ({})", program.string(), std::system_category().message(spawned));
            return std::nullopt;
        }

        char buffer[4096];
        for (;;)
        {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (left <= 0)
            {
                kill(pid, SIGKILL);
                result.timedOut = true;
                break;
            }
            pollfd watched{pipeEnds[0], POLLIN, 0};
            const int ready = poll(&watched, 1, static_cast<int>(std::min<long long>(left, 200)));
            if (ready < 0)
            {
                break;
            }
            if (ready > 0)
            {
                const ssize_t got = read(pipeEnds[0], buffer, sizeof(buffer));
                if (got <= 0)
                {
                    break; // the process closed its output: it is done or about to be
                }
                if (result.output.size() < 4 * 1024 * 1024)
                {
                    result.output.append(buffer, static_cast<std::size_t>(got));
                }
            }
        }
        close(pipeEnds[0]);
        int status = 0;
        waitpid(pid, &status, 0);
        result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1);
        return result;
#endif
    }

    CookResult Cook(const Tooling &tooling, const std::filesystem::path &projectRoot, const std::string &assetPath,
                    const std::string &targets, const std::filesystem::path &outDir, const std::chrono::milliseconds timeout)
    {
        CookResult result;
        if (!tooling.Available())
        {
            result.lines.push_back(tooling.unavailable);
            result.summary = tooling.unavailable;
            return result;
        }
        const std::filesystem::path file = projectRoot / std::filesystem::path(std::u8string(assetPath.begin(), assetPath.end()));
        std::vector<std::string> arguments{tooling.script.string(), outDir.empty() ? "check" : "cook", file.string(), "--cite", assetPath,
                                           "--targets", targets, "--require-payload"};
        if (!outDir.empty())
        {
            arguments.push_back("--out");
            arguments.push_back(outDir.string());
        }
        std::string error;
        const auto output = RunProcess(tooling.node, arguments, timeout, error);
        if (!output)
        {
            result.lines.push_back(error);
            result.summary = error;
            return result;
        }
        result.ran = true;
        result.exitCode = output->exitCode;
        std::istringstream stream(output->output);
        std::string line;
        while (std::getline(stream, line))
        {
            line = Trim(line);
            if (!line.empty())
            {
                result.lines.push_back(line);
            }
        }
        if (output->timedOut)
        {
            result.lines.push_back(std::format("the shader cook did not finish in {} s and was stopped", timeout.count() / 1000));
            result.summary = result.lines.back();
            return result;
        }
        result.ok = output->exitCode == 0;
        if (result.ok)
        {
            result.summary = outDir.empty() ? "checked for " + targets : "cooked for " + targets;
        }
        else
        {
            const auto firstError = std::ranges::find_if(result.lines, [](const std::string &text) { return text.find(": error:") != std::string::npos; });
            result.summary = firstError != result.lines.end() ? *firstError
                                                              : std::format("the shader cook failed (exit code {})", output->exitCode);
        }
        return result;
    }
}
