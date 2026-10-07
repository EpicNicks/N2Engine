#pragma once

#include "editor-server/HostOptions.hpp"

namespace N2Engine::Editor
{
    /**
     * Runs an editor host, as N2EditorHost does: initialises the engine headless (with the chosen renderer), the
     * project's resources (--project), and an EditorServer (access token, stop on disconnect), prints the ready line,
     * then serves on this thread until a signal (SIGINT/SIGTERM), a Shutdown command, Application::Quit, the window
     * closing, or (with exitOnDisconnect) the client's session ending. Then it stops the server and shuts the engine
     * down. Returns the process exit code: 0, or 1 when the token, the project folder or the server couldn't be set
     * up, or a std::exception escaped. With options.showHelp it only prints HostUsage() and returns 0.
     *
     * In the library rather than N2EditorHost's main so a game with its own C++ components can build its own editor
     * host: a main that registers them, then calls RunHost (#6, decision 18). Call it at most once at a time; it
     * installs SIGINT/SIGTERM handlers and uses the Application singleton.
     */
    int RunHost(const HostOptions &options);

    /// The command line form: parses the arguments after the program name (ParseHostArguments), prints a parse
    /// error to stderr and returns 1, otherwise returns RunHost(options). N2EditorHost's main is just this.
    int RunHost(int argc, char *argv[]);
}
