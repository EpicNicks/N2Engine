#pragma once

#include "editor-server/HostOptions.hpp"

namespace N2Engine::Editor
{
    /**
     * Runs an editor host, as N2EditorHost does: reads the project (--project: its project.n2proj), initialises the
     * engine headless (with the chosen renderer), the project's resources (its id as the asset UUID namespace, its own
     * user:// folder), settings and startup scene, and an EditorServer (access token, stop on disconnect), prints the
     * ready line, then serves on this thread until a signal (SIGINT/SIGTERM), a Shutdown command, Application::Quit,
     * the window closing, or (with exitOnDisconnect) the client's session ending. Then it stops the server and shuts
     * the engine down. Returns the process exit code: 0, or 1 when the token, the project (no folder, no valid
     * project.n2proj, another engine major version) or the server couldn't be set up, or a std::exception escaped.
     * With options.showHelp it only prints HostUsage() and returns 0; with options.createPath it is RunCreate, and with
     * options.newTokenUser RunNewToken.
     *
     * With options.tokensFile (--tokens-file) every connection must Hello with the token of a user of that file and gets
     * the user's role (EditorServer::SetTokensFile); a build without N2ENGINE_EDITOR_TLS, or a file that can't be loaded,
     * ends the host before it starts, exit code 1 and a line on stderr. On POSIX SIGHUP makes the host re-read the file
     * (the handler is installed only with a tokens file, so a host without one keeps SIGHUP's default).
     *
     * With options.playScene (--play) the host is a play host (#82, E9): it loads that scene snapshot instead of the
     * startup scene and runs it as a game, with EditorServer::RunPlayFrame as its loop (a game frame, then the client's
     * commands, sixty times a second) rather than ProcessCommands. A snapshot that can't be loaded ends the host with
     * exit code 1 and a line on stderr, before the ready line.
     *
     * In the library rather than N2EditorHost's main so a game with its own C++ components can build its own editor
     * host: a main that registers them, then calls RunHost (#6, decision 18). Call it at most once at a time; it
     * installs SIGINT/SIGTERM handlers and uses the Application singleton.
     */
    int RunHost(const HostOptions &options);

    /**
     * N2EditorHost --create (RunHost calls it when options.createPath is set): makes options.createPath a project
     * with IO::CreateProject (options.projectName, options.projectId or projectIdFromPath), without initialising the
     * engine. On success prints FormatCreatedLine to stdout (and nothing else) and returns 0; otherwise prints one
     * line, "N2EditorHost --create: <reason>", to stderr and returns ExitCodeAlreadyAProject when the folder already
     * has a project.n2proj (nothing is written), or 1.
     */
    int RunCreate(const HostOptions &options);

    /**
     * N2EditorHost --new-token <user> <role> (RunHost calls it when options.newTokenUser is set): makes a token for that
     * user and role without initialising the engine, and prints it to stdout with the entry to add to the tokens file
     * (FormatNewTokenOutput) and returns 0. The token is shown only here: the tokens file keeps its SHA-256. A build
     * without N2ENGINE_EDITOR_TLS has neither the random generator nor SHA-256, so it prints "N2EditorHost --new-token:
     * <reason>" to stderr and returns 1.
     */
    int RunNewToken(const HostOptions &options);

    /// The command line form: parses the arguments after the program name (ParseHostArguments), prints a parse
    /// error to stderr and returns 1, otherwise returns RunHost(options). N2EditorHost's main is just this.
    int RunHost(int argc, char *argv[]);
}
