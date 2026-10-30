#include "editor-server/NetworkPanel.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <functional>
#include <optional>
#include <set>
#include <vector>

#include <math/DeterministicMath.hpp>
#include <math/FloatEnvironment.hpp>
#include <math/MathRegistrar.hpp>

#include "engine/ProjectSettings.hpp"
#include "engine/Time.hpp"
#include "engine/network/NetworkPresets.hpp"
#include "engine/physics/GpuStatus.hpp"

namespace N2Engine::Editor::NetworkPanel
{
    using nlohmann::json;
    using Network::NetworkPreset;

    namespace
    {
        constexpr std::array<NetworkPreset, 3> kPresets = {NetworkPreset::None, NetworkPreset::Authoritative,
                                                           NetworkPreset::Deterministic};

        std::string Lower(std::string text)
        {
            std::ranges::transform(text, text.begin(),
                                   [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        /// The value at a dotted path ("physics.gpu") of the settings object, or nullptr
        const json *Find(const json &settings, const std::string_view path)
        {
            const json *node = &settings;
            std::size_t start = 0;
            while (start <= path.size())
            {
                const std::size_t dot = path.find('.', start);
                const std::string part(path.substr(start, dot == std::string_view::npos ? dot : dot - start));
                if (!node->is_object())
                {
                    return nullptr;
                }
                const auto found = node->find(part);
                if (found == node->end() || found->is_null())
                {
                    return nullptr;
                }
                node = &*found;
                if (dot == std::string_view::npos)
                {
                    return node;
                }
                start = dot + 1;
            }
            return node;
        }

        std::string SimdName(const Math::SIMDTierRequest request)
        {
            switch (request)
            {
            case Math::SIMDTierRequest::Auto:
                return "auto";
            case Math::SIMDTierRequest::Scalar:
                return "scalar";
            case Math::SIMDTierRequest::SSE2:
                return "sse2";
            case Math::SIMDTierRequest::SSE41:
                return "sse4.1";
            case Math::SIMDTierRequest::AVX:
                return "avx";
            case Math::SIMDTierRequest::NEON:
                return "neon";
            }
            return "auto";
        }

        bool ValuesMatch(const json &a, const json &b)
        {
            if (a.is_string() && b.is_string())
            {
                return Lower(a.get<std::string>()) == Lower(b.get<std::string>());
            }
            if (a.is_number() && b.is_number())
            {
                const double x = a.get<double>();
                const double y = b.get<double>();
                return std::abs(x - y) <= std::max(std::abs(x), std::abs(y)) * 1e-3;
            }
            return a == b;
        }

        /// The network block's resolved values by setting key ("network.tickRate", ...)
        json NetworkValues(const Network::NetworkSettings &n)
        {
            return json{{"network.tickRate", n.tickRate},
                        {"network.transport.udp", n.transport.udp},
                        {"network.transport.webSocket", n.transport.webSocket},
                        {"network.transport.port", n.transport.port},
                        {"network.authoritative.snapshotRate", n.authoritative.snapshotRate},
                        {"network.authoritative.interpolationDelayMs", n.authoritative.interpolationDelayMs},
                        {"network.authoritative.inputRedundancy", n.authoritative.inputRedundancy},
                        {"network.authoritative.predictionMaxMs", n.authoritative.predictionMaxMs},
                        {"network.authoritative.smoothingMs", n.authoritative.smoothingMs},
                        {"network.authoritative.snapThreshold", n.authoritative.snapThreshold},
                        {"network.authoritative.lagCompMaxMs", n.authoritative.lagCompMaxMs},
                        {"network.deterministic.mode", std::string(Network::ToString(n.deterministic.mode))},
                        {"network.deterministic.inputDelay", n.deterministic.inputDelay},
                        {"network.deterministic.rollbackWindow", n.deterministic.rollbackWindow},
                        {"network.deterministic.hashInterval", n.deterministic.hashInterval}};
        }

        /// The value the project has now for a setting key (the explicit one, else the default), or null when the
        /// key has no value of its own (a requirement)
        json CurrentValue(const json &settings, const Network::NetworkSettings &network, const std::string &key)
        {
            if (key.starts_with("network."))
            {
                const json values = NetworkValues(network);
                const auto found = values.find(key);
                return found == values.end() ? json(nullptr) : *found;
            }
            if (key == "physics.gpu")
            {
                return std::string(Physics::ToString(ReadPhysicsGpuMode(settings)));
            }
            if (key == "physics.fixedTimestep")
            {
                const json *step = Find(settings, key);
                if (step != nullptr && step->is_number() && step->get<double>() > 0.0)
                {
                    return step->get<double>();
                }
                return Time::GetFixedTimestep();
            }
            if (key == "math.simdTier")
            {
                return SimdName(ReadMathSimdTier(settings));
            }
            if (key == "physics.backend")
            {
                if (const auto kind = ReadPhysicsBackend(settings))
                {
                    return std::string(Physics::ToString(*kind));
                }
                return nullptr;
            }
            return nullptr;
        }

        std::string SourceOf(const json &settings, const Network::NetworkSettings &network, const std::string &key)
        {
            if (key.empty())
            {
                return "build";
            }
            if (network.overrides.is_object() && network.overrides.contains(key))
            {
                return "override";
            }
            return Find(settings, key) != nullptr ? "project" : "default";
        }

        struct PresetText
        {
            const char *label;
            const char *summary;
        };

        PresetText TextOf(const NetworkPreset preset)
        {
            switch (preset)
            {
            case NetworkPreset::None:
                return {"None", "Offline. No network model: the engine behaves as it always has."};
            case NetworkPreset::Authoritative:
                return {"Server authoritative",
                        "A server simulates the game; clients predict their own avatar and interpolate the rest."};
            case NetworkPreset::Deterministic:
                return {"Deterministic lockstep/rollback",
                        "Every peer runs the same simulation from the same inputs; needs a cross-platform "
                        "deterministic build and refuses to start when a requirement is unmet."};
            }
            return {"", ""};
        }

        json RowJson(const Network::PresetRow &row, const NetworkPreset preset, const json &effective,
                     const json &settings, const Network::NetworkSettings &network)
        {
            const Network::PresetCell &cell = row.Cell(preset);
            json out = {{"key", row.key},
                        {"label", row.label},
                        {"governed", cell.classes != 0},
                        {"locked", Network::HasClass(cell.classes, Network::SettingClass::Locked)},
                        {"tunable", Network::HasClass(cell.classes, Network::SettingClass::Tunable)},
                        {"startupChecked", Network::HasClass(cell.classes, Network::SettingClass::StartupChecked)},
                        {"description", cell.description},
                        {"source", SourceOf(settings, network, cell.settingKey)}};
            json value = cell.value;
            if (!cell.settingKey.empty())
            {
                out["settingKey"] = cell.settingKey;
                if (const auto found = effective.find(cell.settingKey); found != effective.end())
                {
                    value = *found;
                }
                const json current = CurrentValue(settings, network, cell.settingKey);
                out["current"] = current;
                out["atPresetValue"] = !value.is_null() && !current.is_null() && ValuesMatch(current, value);
            }
            out["value"] = value;
            if (!cell.lostGuarantee.empty())
            {
                out["lostGuarantee"] = cell.lostGuarantee;
            }
            return out;
        }

        /// The stored overrides plus every explicit project value of a Locked setting: what the preset table's
        /// LostGuarantees judges
        json EffectiveOverrides(const json &settings, const Network::NetworkSettings &network)
        {
            json overrides = network.overrides.is_object() ? network.overrides : json::object();
            for (const Network::PresetRow &row : Network::PresetTable())
            {
                const Network::PresetCell &cell = row.Cell(network.preset);
                if (!Network::HasClass(cell.classes, Network::SettingClass::Locked) || cell.settingKey.empty() ||
                    overrides.contains(cell.settingKey))
                {
                    continue;
                }
                if (const json *explicitValue = Find(settings, cell.settingKey))
                {
                    overrides[cell.settingKey] = *explicitValue;
                }
            }
            return overrides;
        }

        json BlockValues(const Network::NetworkSettings &n)
        {
            return json{{"tickRate", n.tickRate},
                        {"transport", {{"udp", n.transport.udp},
                                       {"webSocket", n.transport.webSocket},
                                       {"port", n.transport.port}}},
                        {"authoritative", {{"snapshotRate", n.authoritative.snapshotRate},
                                           {"interpolationDelayMs", n.authoritative.interpolationDelayMs},
                                           {"inputRedundancy", n.authoritative.inputRedundancy},
                                           {"predictionMaxMs", n.authoritative.predictionMaxMs},
                                           {"smoothingMs", n.authoritative.smoothingMs},
                                           {"snapThreshold", n.authoritative.snapThreshold},
                                           {"lagCompMaxMs", n.authoritative.lagCompMaxMs},
                                           {"timeDilation", {{"enabled", n.authoritative.timeDilation.enabled},
                                                             {"minScale", n.authoritative.timeDilation.minScale},
                                                             {"maxScale", n.authoritative.timeDilation.maxScale}}}}},
                        {"deterministic", {{"mode", std::string(Network::ToString(n.deterministic.mode))},
                                           {"inputDelay", n.deterministic.inputDelay},
                                           {"rollbackWindow", n.deterministic.rollbackWindow},
                                           {"hashInterval", n.deterministic.hashInterval}}}};
        }

        /// The backends of this host that can carry the Deterministic preset
        std::vector<std::string> SelectableBackends(const HostBuild &host)
        {
            std::vector<std::string> ids;
            for (const auto kind : {Physics::PhysicsBackendKind::PhysX, Physics::PhysicsBackendKind::Jolt})
            {
                if (host.Backend(kind).DeterministicSelectable())
                {
                    ids.emplace_back(Physics::ToString(kind));
                }
            }
            return ids;
        }

        /// A one-click fix for an unmet requirement: a merge patch for SetProjectSettings, or null
        json FixFor(const std::string &id, const json &settings, const Network::NetworkSettings &network,
                    const Network::NetworkEnvironment &env, const HostBuild &host)
        {
            const auto fix = [](std::string label, json patch)
            { return json{{"label", std::move(label)}, {"settings", std::move(patch)}}; };
            if (id == "simd-scalar")
            {
                return fix("Set math.simdTier to scalar", {{"math", {{"simdTier", "scalar"}}}});
            }
            if (id == "gpu-physics-off")
            {
                return fix("Set physics.gpu to off", {{"physics", {{"gpu", "off"}}}});
            }
            if (id == "fixed-timestep" && env.fixedTimestep)
            {
                const double step = 1.0 / static_cast<double>(std::max(1, network.tickRate));
                return fix(std::format("Set physics.fixedTimestep to 1/{} s", network.tickRate),
                           {{"physics", {{"fixedTimestep", step}}}});
            }
            if (id == "physics-backend" || id == "physics-features")
            {
                for (const std::string &backend : SelectableBackends(host))
                {
                    return fix(std::format("Use the {} backend with physics.deterministic on", backend),
                               {{"physics", {{"backend", backend}, {"deterministic", true}}}});
                }
                return nullptr;
            }
            if (id == "physics-threads" && !ReadPhysicsDeterministic(settings) && env.hasBackend)
            {
                const auto kind = Physics::ParsePhysicsBackendKind(env.backendName);
                if (kind && host.Backend(*kind).determinism >= Physics::DeterminismLevel::SameBinary)
                {
                    return fix("Set physics.deterministic on", {{"physics", {{"deterministic", true}}}});
                }
            }
            return nullptr;
        }
    }

    HostBuild CurrentHostBuild()
    {
        HostBuild host;
        host.strictFp = Math::IsStrictFp();
        host.contractionFree = Math::IsContractionFree();
        host.available = Physics::CompiledInBackends();
        host.backends[static_cast<std::size_t>(Physics::PhysicsBackendKind::PhysX)] =
            Physics::DescribePhysicsBackend(Physics::PhysicsBackendKind::PhysX);
        host.backends[static_cast<std::size_t>(Physics::PhysicsBackendKind::Jolt)] =
            Physics::DescribePhysicsBackend(Physics::PhysicsBackendKind::Jolt);
        return host;
    }

    Network::NetworkEnvironment BuildEnvironment(const json &settings, const NetworkPreset preset,
                                                 const HostBuild &host)
    {
        // The same inputs Application::Init gives ValidateNetwork (engine/src/Application.cpp), with the preset put in
        // the project's place and no host option for the backend
        Network::NetworkEnvironment env;
        env.gpuMode = ReadPhysicsGpuMode(settings);
        env.simdTier = ReadMathSimdTier(settings);
        env.luaSeedSet = ReadDeterminismLuaSeed(settings).has_value();
        env.fixedTimestepSeconds = Time::GetFixedTimestep();
        if (const json *step = Find(settings, "physics.fixedTimestep");
            step != nullptr && step->is_number() && step->get<double>() > 0.0)
        {
            env.fixedTimestepSeconds = step->get<double>();
        }
        env.strictFp = host.strictFp;
        env.engineOwnedTrig = host.contractionFree;
        env.ownedTickLoop = Network::SimulationModeFor(preset) != SimulationMode::None;

        const std::optional<Physics::PhysicsBackendKind> requested = ReadPhysicsBackend(settings);
        const Physics::BackendChoice choice =
            Physics::ChooseBackend(requested, Physics::BackendRequestSource::ProjectSetting,
                                   preset == NetworkPreset::Deterministic, host.available);
        env.backendName = choice.selected ? std::string(Physics::ToString(*choice.selected)) : std::string{};
        if (choice.refused && choice.requested)
        {
            env.backendUnavailable = true;
            env.backendRequested = std::string(Physics::ToString(*choice.requested));
            env.backendRequestSource = choice.source;
        }
        else if (choice.selected)
        {
            const Physics::PhysicsBackendDescription &description = host.Backend(*choice.selected);
            env.hasBackend = true;
            env.backendLevel = ReadPhysicsDeterministic(settings) ? description.determinism
                                                                  : description.determinismDefault;
            env.backendSupportsSnapshot = description.supportsSnapshot;
            env.backendSupportsSetState = description.supportsSetState;
            env.backendFeatures = description.features;
        }
        return env;
    }

    Network::NetworkValidation Validate(const json &settings, const NetworkPreset preset, const HostBuild &host)
    {
        Network::NetworkSettings network = ReadNetworkSettings(settings);
        network.preset = preset;
        return Network::ValidateNetwork(network, BuildEnvironment(settings, preset, host));
    }

    std::string UnavailableReason(const json &settings, const NetworkPreset preset, const HostBuild &host)
    {
        if (preset != NetworkPreset::Deterministic)
        {
            return {};
        }
        const Network::NetworkValidation validation = Validate(settings, preset, host);
        std::string detail;
        for (const Network::NetworkRequirement *requirement : validation.Unmet())
        {
            if (requirement->id == "physics-backend" || requirement->id == "physics-features")
            {
                detail += (detail.empty() ? "" : "; ") + requirement->detail;
            }
        }
        if (detail.empty())
        {
            return {};
        }
        const std::vector<std::string> selectable = SelectableBackends(host);
        if (selectable.empty())
        {
            return "Requires a deterministic physics backend (none in this build): " + detail;
        }
        std::string names;
        for (const std::string &id : selectable)
        {
            names += (names.empty() ? "" : ", ") + id;
        }
        return std::format("Requires a deterministic physics backend: {}. Choose {} for physics.backend first", detail,
                           names);
    }

    json BuildSettingsInfo(const json &settings, const HostBuild &host, const NudgeInput &nudgeInput)
    {
        std::vector<std::string> problems;
        const Network::NetworkSettings network = ReadNetworkSettings(settings, &problems);

        const json overrides = EffectiveOverrides(settings, network);
        json lost = json::array();
        for (const Network::LostGuarantee &guarantee :
             Network::LostGuarantees(network.preset, overrides, network.tickRate))
        {
            lost.push_back({{"settingKey", guarantee.settingKey},
                            {"rowKey", guarantee.rowKey},
                            {"text", guarantee.text}});
        }

        json presets = json::array();
        for (const NetworkPreset preset : kPresets)
        {
            const json effective = Network::EffectiveValues(preset, json::object(), network.tickRate);
            json rows = json::array();
            for (const Network::PresetRow &row : Network::PresetTable())
            {
                rows.push_back(RowJson(row, preset, effective, settings, network));
            }
            const std::string reason = UnavailableReason(settings, preset, host);
            const PresetText text = TextOf(preset);
            json entry = {{"id", std::string(Network::ToString(preset))},
                          {"label", text.label},
                          {"summary", text.summary},
                          {"selected", preset == network.preset},
                          {"selectable", reason.empty()},
                          {"rows", std::move(rows)}};
            if (!reason.empty())
            {
                entry["unavailableReason"] = reason;
            }
            presets.push_back(std::move(entry));
        }

        json ranges = json::array();
        for (const Network::NumericSettingSpec &spec : Network::NumericSettingSpecs())
        {
            ranges.push_back({{"key", "network." + std::string(spec.key)},
                              {"min", spec.min},
                              {"max", spec.max},
                              {"default", spec.defaultValue},
                              {"integer", spec.integer}});
        }

        // The nudge: the project looks networked but has no network model. The open scene's NetworkObject components and
        // a network block with settings in it are what it looks at; it never blocks anything.
        json reasons = json::array();
        if (nudgeInput.networkObjects > 0)
        {
            reasons.push_back("scene");
        }
        bool blockHasSettings = false;
        if (const json *block = Find(settings, "network"); block != nullptr && block->is_object())
        {
            for (const auto &[key, value] : block->items())
            {
                if (key != "preset" && !value.is_null())
                {
                    blockHasSettings = true;
                }
            }
        }
        if (blockHasSettings)
        {
            reasons.push_back("settings");
        }
        const bool show = network.preset == NetworkPreset::None && !reasons.empty();
        std::string hint;
        if (show)
        {
            if (nudgeInput.networkObjects > 0)
            {
                hint = std::format("This scene has {} NetworkObject component{} but the project has no network model "
                                   "(network.preset is none). Choose Server authoritative or Deterministic.",
                                   nudgeInput.networkObjects, nudgeInput.networkObjects == 1 ? "" : "s");
            }
            else
            {
                hint = "The project has network settings but no network model (network.preset is none). Choose "
                       "Server authoritative or Deterministic.";
            }
        }

        json selectable = json::array();
        for (const std::string &id : SelectableBackends(host))
        {
            selectable.push_back(id);
        }
        const NetworkPreset selectedForBackend = network.preset;
        const Network::NetworkEnvironment env = BuildEnvironment(settings, selectedForBackend, host);

        return json{{"preset", std::string(Network::ToString(network.preset))},
                    {"customised", !lost.empty()},
                    {"storedCustomised", network.customised},
                    {"tickRate", network.tickRate},
                    {"values", BlockValues(network)},
                    {"overrides", network.overrides},
                    {"ranges", std::move(ranges)},
                    {"lostGuarantees", std::move(lost)},
                    {"presets", std::move(presets)},
                    {"backend", {{"selected", env.backendName},
                                 {"deterministicSelectable", std::move(selectable)}}},
                    {"nudge", {{"show", show},
                               {"reasons", std::move(reasons)},
                               {"networkObjects", nudgeInput.networkObjects},
                               {"hint", std::move(hint)}}},
                    {"problems", problems}};
    }

    json BuildValidationReport(const json &settings, const NetworkPreset preset, const HostBuild &host)
    {
        const Network::NetworkSettings project = ReadNetworkSettings(settings);
        Network::NetworkSettings network = project;
        network.preset = preset;
        const Network::NetworkEnvironment env = BuildEnvironment(settings, preset, host);
        const Network::NetworkValidation validation = Network::ValidateNetwork(network, env);

        json requirements = json::array();
        for (const Network::NetworkRequirement &requirement : validation.requirements)
        {
            json entry = {{"id", requirement.id},
                          {"text", requirement.text},
                          {"met", requirement.met},
                          {"detail", requirement.detail}};
            if (!requirement.met)
            {
                if (json fix = FixFor(requirement.id, settings, network, env, host); !fix.is_null())
                {
                    entry["fix"] = std::move(fix);
                }
            }
            requirements.push_back(std::move(entry));
        }
        json warnings = json::array();
        for (const std::string &warning : validation.warnings)
        {
            warnings.push_back(warning);
        }

        const std::size_t unmet = validation.Unmet().size();
        std::string summary;
        switch (preset)
        {
        case NetworkPreset::None:
            summary = "Preset none: nothing is checked.";
            break;
        case NetworkPreset::Authoritative:
            summary = validation.warnings.empty()
                          ? std::string("Server authoritative: no warnings.")
                          : std::format("Server authoritative: {} warning{}; the engine starts anyway.",
                                        validation.warnings.size(), validation.warnings.size() == 1 ? "" : "s");
            break;
        case NetworkPreset::Deterministic:
            summary = unmet == 0 ? std::format("Deterministic: all {} requirements are met.",
                                               validation.requirements.size())
                                 : std::format("Deterministic: {} of {} requirements are unmet; the engine would refuse "
                                               "to start this project.",
                                               unmet, validation.requirements.size());
            break;
        }

        return json{{"preset", std::string(Network::ToString(preset))},
                    {"projectPreset", std::string(Network::ToString(project.preset))},
                    {"refused", validation.Refused()},
                    {"unmet", unmet},
                    {"summary", summary},
                    {"requirements", std::move(requirements)},
                    {"warnings", std::move(warnings)},
                    {"environment",
                     {{"backend", env.backendName},
                      {"backendDeterminism", std::string(Physics::ToString(env.backendLevel))},
                      {"strictFp", env.strictFp},
                      {"engineOwnedTrig", env.engineOwnedTrig},
                      {"simdTier", SimdName(env.simdTier)},
                      {"luaSeedSet", env.luaSeedSet},
                      {"physicsGpu", std::string(Physics::ToString(env.gpuMode))},
                      {"fixedTimestepSeconds", env.fixedTimestepSeconds},
                      {"tickRate", network.tickRate},
                      {"deterministicMode", std::string(Network::ToString(network.deterministic.mode))}}}};
    }

    std::expected<json, std::string> BuildChangePatch(const json &settings, const std::string_view presetText,
                                                      const json &tunable, const HostBuild &host)
    {
        static const std::set<std::string, std::less<>> allowed = []
        {
            std::set<std::string, std::less<>> keys = {"transport.udp", "transport.webSocket", "transport.port",
                                                       "deterministic.mode"};
            for (const Network::NumericSettingSpec &spec : Network::NumericSettingSpecs())
            {
                keys.insert(std::string(spec.key));
            }
            return keys;
        }();
        static const std::set<std::string, std::less<>> blocks = {"transport", "authoritative", "deterministic"};
        static const std::set<std::string, std::less<>> dilationKeys = {"enabled", "minScale", "maxScale"};

        json network = json::object();
        if (!presetText.empty())
        {
            const std::optional<NetworkPreset> preset = Network::ParseNetworkPreset(presetText);
            if (!preset)
            {
                return std::unexpected(
                    "preset must be \"none\", \"authoritative\" or \"deterministic\" (or empty to keep the preset)");
            }
            if (const std::string reason = UnavailableReason(settings, *preset, host); !reason.empty())
            {
                return std::unexpected("The Deterministic preset can't be chosen: " + reason);
            }
            network["preset"] = std::string(Network::ToString(*preset));
        }

        if (!tunable.is_object())
        {
            return std::unexpected("settings must be a JSON object: a merge patch for the tunable settings of the "
                                   "project's network block");
        }
        for (const auto &[key, value] : tunable.items())
        {
            const auto refuse = [&](const std::string &path) -> std::unexpected<std::string>
            {
                return std::unexpected(std::format(
                    "{} is not a tunable network setting this command changes (tickRate, transport.*, authoritative.*, "
                    "deterministic.*); the preset is the preset field, and the other settings of a preset go through "
                    "SetProjectSettings",
                    path.size() > 64 ? path.substr(0, 64) + "..." : path));
            };
            if (blocks.contains(key))
            {
                if (value.is_null())
                {
                    network[key] = nullptr;
                    continue;
                }
                if (!value.is_object())
                {
                    return std::unexpected(std::format("{} must be an object", key));
                }
                for (const auto &[subKey, subValue] : value.items())
                {
                    // authoritative.timeDilation (since 1.23.0) is an object of its own: its three keys, each of its
                    // type or null (the default). The scale ranges are the engine's read, as for the other settings
                    if (key == "authoritative" && subKey == "timeDilation")
                    {
                        if (subValue.is_null())
                        {
                            continue;
                        }
                        if (!subValue.is_object())
                        {
                            return std::unexpected("authoritative.timeDilation must be an object (enabled, minScale, "
                                                   "maxScale) or null");
                        }
                        for (const auto &[dilationKey, dilationValue] : subValue.items())
                        {
                            const std::string path = "authoritative.timeDilation." + dilationKey;
                            if (!dilationKeys.contains(dilationKey))
                            {
                                return refuse(path);
                            }
                            if (dilationValue.is_null())
                            {
                                continue;
                            }
                            if (dilationKey == "enabled" && !dilationValue.is_boolean())
                            {
                                return std::unexpected(path + " must be true, false or null");
                            }
                            if (dilationKey != "enabled" && !dilationValue.is_number())
                            {
                                return std::unexpected(path + " must be a number or null");
                            }
                        }
                        continue;
                    }
                    if (!allowed.contains(key + "." + subKey))
                    {
                        return refuse(key + "." + subKey);
                    }
                }
                network[key] = value;
                continue;
            }
            // The only tunable setting that is not in a block (a dotted name is not a key of the block itself)
            if (key != "tickRate")
            {
                return refuse(key);
            }
            network[key] = value;
        }
        return json{{"network", std::move(network)}};
    }
}
