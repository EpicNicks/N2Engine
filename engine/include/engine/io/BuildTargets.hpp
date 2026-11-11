#pragma once

#include <expected>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "engine/io/ProjectFile.hpp"

namespace N2Engine::IO
{
    /**
     * A project's per-target build settings: project.n2proj's build.targets (build targets stage 2;
     * docs/build-targets.html#settings). An object keyed by target name (the names tools/n2build lists, or a private
     * target's), each holding that target's settings:
     *
     *     enabled       bool: whether the target is offered in the editor's build list
     *     displayName   the game's name on that platform (1 to 64 characters, no control characters)
     *     appId         the reverse-domain id stores use (com.example.game: two or more segments of letters, digits and
     *                   '_', each starting with a letter; at most 150 characters)
     *     version       "major.minor.patch", each part 1 to 5 digits
     *     buildNumber   a whole number from 1 to 2100000000 (Android's versionCode range)
     *     orientation   "any", "landscape" or "portrait"
     *     renderer      "auto", "software", "opengl", "vulkan" or "webgpu"
     *     config        the default configuration: "debug" or "release"
     *     icon          a res:// path to a .png inside the assets folder
     *     modules       the engine modules for this target, over build.modules (the same keys and values)
     *     signing       {"profile": "<name>"}: the NAME of a signing profile kept in the user's own settings, never the
     *                   key, certificate or password itself
     *
     * Keys this version does not know are kept, so a newer editor's settings survive, but no key at any depth may look
     * like a secret (a password, passphrase, token, private key, credential...) and no string may hold a PEM block:
     * project files are committed and shared, so signing material is only ever referred to by name.
     *
     * The project file itself keeps build as one of its unknownKeys (ProjectFile does not read it), so a project with
     * a hand-edited mistake here still opens; the rules apply when the editor writes the block (SetBuildTargets).
     */
    namespace BuildTargets
    {
        inline constexpr std::size_t MaxTargets = 32;
        /// The whole block's JSON text, at most
        inline constexpr std::size_t MaxBytes = 64 * 1024;
        inline constexpr long long MaxBuildNumber = 2100000000;

        /// Whether text is a target name: a lowercase letter, then lowercase letters, digits and '-', at most 32
        [[nodiscard]] bool IsTargetName(std::string_view text);

        /// Every rule above: an error naming the key at fault ("build.targets.android.version: ...")
        [[nodiscard]] std::expected<void, std::string> Validate(const nlohmann::json &targets);

        /// The project's build.targets, or an empty object when it has none
        [[nodiscard]] nlohmann::json Of(const ProjectFile &project);

        /// project with build.targets replaced by targets; an empty object removes the key (and build itself when
        /// nothing else is in it), so a project that never had targets is written back as it was
        void Set(ProjectFile &project, const nlohmann::json &targets);

        /**
         * current with a JSON merge patch (RFC 7386) applied, checked with Validate: a target set to null is removed,
         * a key set to null inside a target is removed, objects merge, anything else replaces. patch must be an object.
         */
        [[nodiscard]] std::expected<nlohmann::json, std::string> ApplyPatch(const nlohmann::json &current,
                                                                             const nlohmann::json &patch);
    }
}
