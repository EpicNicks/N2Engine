#pragma once

#include <array>
#include <expected>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <engine/network/NetworkSettings.hpp>
#include <engine/network/NetworkValidation.hpp>
#include <engine/physics/PhysicsBackendInfo.hpp>
#include <engine/physics/PhysicsBackendKind.hpp>

/// The host side of the editor's Network panel (B11, protocol 1.21.0): the network settings with the three preset
/// bundles, the validation report of a preset, the nudge, and the checks of a change. Pure functions of the project's
/// settings object and of what the host build offers (HostBuild), so the tests reach every combination of build flags in
/// any build. EditorServer's handlers only add the project and the scene.
namespace N2Engine::Editor::NetworkPanel
{
    /// What the host build offers, as the startup checks of Application::Init see it. CurrentHostBuild() reads it from
    /// the running build; a test fills it in by hand.
    struct HostBuild
    {
        /// Math::IsStrictFp()
        bool strictFp = false;
        /// Math::IsContractionFree()
        bool contractionFree = false;
        Physics::BackendAvailability available;
        /// Index = Physics::PhysicsBackendKind
        std::array<Physics::PhysicsBackendDescription, 2> backends;

        [[nodiscard]] const Physics::PhysicsBackendDescription &Backend(const Physics::PhysicsBackendKind kind) const
        {
            return backends[static_cast<std::size_t>(kind)];
        }
    };

    [[nodiscard]] HostBuild CurrentHostBuild();

    /// What the nudge looks at: the open scene
    struct NudgeInput
    {
        /// NetworkObject components in the open scene (0 without a scene)
        int networkObjects = 0;
    };

    /// The environment the startup checks of `preset` get for a project with these settings in this build: what
    /// Application::Init builds, with the physics backend chosen by Physics::ChooseBackend and no host option.
    [[nodiscard]] Network::NetworkEnvironment BuildEnvironment(const nlohmann::json &settings,
                                                               Network::NetworkPreset preset, const HostBuild &host);

    /// ValidateNetwork for the project's network block with `preset` put in its place
    [[nodiscard]] Network::NetworkValidation Validate(const nlohmann::json &settings, Network::NetworkPreset preset,
                                                      const HostBuild &host);

    /// Why `preset` cannot be chosen now (shown on its card), or empty when it can. Only Deterministic can be refused:
    /// when the physics backend the project would run on does not declare what the preset needs
    /// (the physics-backend and physics-features requirements).
    [[nodiscard]] std::string UnavailableReason(const nlohmann::json &settings, Network::NetworkPreset preset,
                                                const HostBuild &host);

    /// NetworkSettingsInfo (GetNetworkSettings, SetNetworkSettings): see protocol.json
    [[nodiscard]] nlohmann::json BuildSettingsInfo(const nlohmann::json &settings, const HostBuild &host,
                                                   const NudgeInput &nudge);

    /// NetworkValidationReport (GetNetworkValidation): the checks of `preset` against the project and this build
    [[nodiscard]] nlohmann::json BuildValidationReport(const nlohmann::json &settings, Network::NetworkPreset preset,
                                                       const HostBuild &host);

    /// The merge patch ({"network": {...}}) a SetNetworkSettings request means, or the Error to answer. `presetText`
    /// is empty for no change of preset. `tunable` must be an object whose keys are tunable settings of the network
    /// block (not the preset, the stored overrides or the customised flag, which the command or the preset table own);
    /// a null value removes a key. Choosing Deterministic is refused when UnavailableReason says so.
    [[nodiscard]] std::expected<nlohmann::json, std::string> BuildChangePatch(const nlohmann::json &settings,
                                                                              std::string_view presetText,
                                                                              const nlohmann::json &tunable,
                                                                              const HostBuild &host);
}
