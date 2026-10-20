#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace N2Engine::Editor::ClientText
{
    /**
     * What a client is told about a failure (security audit F10, item 11): the one place that decides it.
     *
     * A response must not carry a path on the host's disk or the text of an exception the engine or the file system
     * threw (a std::filesystem_error names both of its paths). A REMOTE client gets a generic message: what failed, which
     * res:// path, and a stable reason ("file not found", "permission denied"). The detail goes to the host's log. A LOCAL
     * client (EditorServer::IsLocalClient: no tokens file, and the caller is in this process or on loopback) keeps the
     * detail, as before.
     *
     * Which one a command answers to is the Scope EditorServer::ExecuteCommand opens around it. Outside a Scope nothing is
     * local: the functions fail closed, so a message built where no command is running gives nothing away.
     */

    /// A folder on the host's disk and what stands for it in a message to a remote client
    struct HostRoot
    {
        std::filesystem::path path;
        std::string label;
    };

    /// For as long as it lives, the commands that run on this thread answer to a local or a remote client, and Redact
    /// replaces these folders. Nests: the destructor puts back the previous one.
    class Scope
    {
    public:
        Scope(bool local, std::vector<HostRoot> roots);
        ~Scope();

        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;

    private:
        bool _previousLocal;
        std::vector<HostRoot> _previousRoots;
    };

    /// Whether the command running on this thread answers to a local client; false outside a Scope
    [[nodiscard]] bool IsLocal();

    /**
     * The message for a failure that has a detail naming the host (an exception's text, an error from the file system or
     * the engine). The detail is always logged, with `what`. Returns
     *     local:  "<what>: <detail>", or just the detail when `what` is empty
     *     remote: "<what>: <reason>", or just `reason` when `what` is empty
     * `reason` is a stable, generic sentence with no path in it; `detail` is whatever the failure said.
     */
    [[nodiscard]] std::string Failure(std::string_view what, std::string_view reason, std::string_view detail);

    /// `text` as a remote client may see it: the folders of the current Scope replaced by their labels, and anything
    /// that follows such a folder with its separators as '/'. A local client's text is returned as it is.
    [[nodiscard]] std::string Redact(std::string text);

    /// Redact for these roots, whoever the client is (the part that has no state, for the tests)
    [[nodiscard]] std::string RedactHostPaths(std::string text, const std::vector<HostRoot> &roots);

    /// A stable reason for a failed file operation: "file not found", "permission denied", "the disk is full" or "the
    /// file system refused it"
    [[nodiscard]] std::string Reason(const std::error_code &error);

    /// The file's path below `root` with '/' separators (".n2/play/Main-1a2b3c4d.scene"), or empty when it isn't below it
    [[nodiscard]] std::string RelativeTo(const std::filesystem::path &file, const std::filesystem::path &root);
}
