#include "engine/ProjectSettings.hpp"

#include <cmath>
#include <exception>
#include <format>

#include "engine/Application.hpp"
#include "engine/Layers.hpp"
#include "engine/Time.hpp"
#include "engine/Window.hpp"
#include "engine/input/InputSystem.hpp"
#include "engine/network/NetworkPresets.hpp"
#include "engine/physics/IPhysicsBackend.hpp"
#include "engine/rendering/RenderSettings.hpp"

namespace N2Engine
{
    namespace
    {
        Input::InputSystem *CurrentInputSystem()
        {
            return Application::GetInstance().GetWindow().GetInputSystem();
        }

        void AddProblem(std::vector<std::string> *problems, std::string message)
        {
            if (problems != nullptr)
            {
                problems->push_back(std::move(message));
            }
        }

        /// A boolean member: absent or null is the default, a non-boolean is refused (default used)
        bool ReadNetworkBool(const nlohmann::json &block, const char *name, const std::string &path,
                             const bool defaultValue, std::vector<std::string> *problems)
        {
            const auto it = block.find(name);
            if (it == block.end() || it->is_null())
            {
                return defaultValue;
            }
            if (it->is_boolean())
            {
                return it->get<bool>();
            }
            AddProblem(problems, std::format("{}.{}: {} is not true or false; using {}", path, name, it->dump(),
                                             defaultValue));
            return defaultValue;
        }

        /// A numeric member with the range of Network::FindNumericSpec(specKey): absent or null is the default, a
        /// non-number (or a non-integer for a whole-number setting) is refused (default used), a number outside the
        /// range is clamped to it
        double ReadNetworkNumber(const nlohmann::json &block, const char *name, const std::string &path,
                                 const std::string_view specKey, std::vector<std::string> *problems)
        {
            const Network::NumericSettingSpec *spec = Network::FindNumericSpec(specKey);
            if (spec == nullptr)
            {
                return 0.0;
            }
            const auto it = block.find(name);
            if (it == block.end() || it->is_null())
            {
                return spec->defaultValue;
            }
            if (!it->is_number())
            {
                AddProblem(problems, std::format("{}.{}: {} is not a number; using {}", path, name, it->dump(),
                                                 spec->defaultValue));
                return spec->defaultValue;
            }
            const double value = it->get<double>();
            if (spec->integer && value != std::floor(value))
            {
                AddProblem(problems, std::format("{}.{}: {} is not a whole number; using {}", path, name, it->dump(),
                                                 spec->defaultValue));
                return spec->defaultValue;
            }
            if (value < spec->min || value > spec->max)
            {
                const double clamped = value < spec->min ? spec->min : spec->max;
                AddProblem(problems, std::format("{}.{}: {} is outside {} to {}; using {}", path, name, it->dump(),
                                                 spec->min, spec->max, clamped));
                return clamped;
            }
            return value;
        }

        int ReadNetworkInt(const nlohmann::json &block, const char *name, const std::string &path,
                           const std::string_view specKey, std::vector<std::string> *problems)
        {
            return static_cast<int>(ReadNetworkNumber(block, name, path, specKey, problems));
        }

        /// A nested object member: nullptr when absent, null, or not an object (the last is refused)
        const nlohmann::json *ReadNetworkSubBlock(const nlohmann::json &block, const char *name,
                                                  const std::string &path, std::vector<std::string> *problems)
        {
            const auto it = block.find(name);
            if (it == block.end() || it->is_null())
            {
                return nullptr;
            }
            if (!it->is_object())
            {
                AddProblem(problems, std::format("{}.{}: not an object; using the defaults", path, name));
                return nullptr;
            }
            return &*it;
        }

        /// A scale member of network.authoritative.timeDilation: absent or null is the default, a non-number is refused
        /// (default used), a number outside low..high is clamped
        double ReadDilationScale(const nlohmann::json &block, const char *name, const double low, const double high,
                                 const double defaultValue, std::vector<std::string> *problems)
        {
            const auto it = block.find(name);
            if (it == block.end() || it->is_null())
            {
                return defaultValue;
            }
            if (!it->is_number() || !std::isfinite(it->get<double>()))
            {
                AddProblem(problems, std::format("network.authoritative.timeDilation.{}: {} is not a number; using {}",
                                                 name, it->dump(), defaultValue));
                return defaultValue;
            }
            const double value = it->get<double>();
            if (value < low || value > high)
            {
                const double clamped = value < low ? low : high;
                AddProblem(problems, std::format("network.authoritative.timeDilation.{}: {} is outside {} to {}; using {}",
                                                 name, it->dump(), low, high, clamped));
                return clamped;
            }
            return value;
        }

        void ApplyPhysics(const nlohmann::json &physics, std::vector<std::string> &problems)
        {
            if (!physics.is_object())
            {
                problems.emplace_back("physics: not an object");
                return;
            }
            if (const auto timestep = physics.find("fixedTimestep"); timestep != physics.end() && !timestep->is_null())
            {
                if (!timestep->is_number() || !Time::SetFixedTimestep(timestep->get<double>()))
                {
                    problems.push_back(std::format("physics.fixedTimestep: {} is not a positive number of seconds",
                                                   timestep->dump()));
                }
            }
            // Validated here; the value itself is read before Init (ReadPhysicsGpuMode), so a change needs a restart
            (void)ReadPhysicsGpuMode(nlohmann::json{{"physics", physics}}, &problems);
            (void)ReadPhysicsDeterministic(nlohmann::json{{"physics", physics}}, &problems);
            (void)ReadPhysicsBackend(nlohmann::json{{"physics", physics}}, &problems);
            if (const auto gravity = physics.find("gravity"); gravity != physics.end() && !gravity->is_null())
            {
                const bool valid = gravity->is_object() && gravity->contains("x") && gravity->contains("y") &&
                                   gravity->contains("z") && gravity->at("x").is_number() &&
                                   gravity->at("y").is_number() && gravity->at("z").is_number();
                if (!valid)
                {
                    problems.push_back(std::format("physics.gravity: {} is not {{\"x\", \"y\", \"z\"}} numbers",
                                                   gravity->dump()));
                }
                else if (Physics::IPhysicsBackend *backend = Application::GetInstance().Get3DPhysicsBackend())
                {
                    backend->SetGravity(Math::Vector3(gravity->at("x").get<float>(), gravity->at("y").get<float>(),
                                                      gravity->at("z").get<float>()));
                }
            }
        }

        void ApplyRendering(const nlohmann::json &rendering, std::vector<std::string> &problems)
        {
            if (!rendering.is_object())
            {
                problems.emplace_back("rendering: not an object");
                return;
            }
            // forceShaderEncode: a boolean; absent or null means off. A wrong type is refused, changing nothing.
            const auto force = rendering.find("forceShaderEncode");
            if (force == rendering.end() || force->is_null())
            {
                Rendering::RenderSettings::SetForceShaderEncode(false);
            }
            else if (!force->is_boolean())
            {
                problems.push_back(std::format("rendering.forceShaderEncode: {} is not true or false", force->dump()));
            }
            else
            {
                Rendering::RenderSettings::SetForceShaderEncode(force->get<bool>());
            }
            const auto space = rendering.find("colorSpace");
            if (space == rendering.end() || space->is_null())
            {
                // The block is there without a colour space (or with it removed): the default
                Rendering::RenderSettings::SetColorSpace(Rendering::ColorSpace::Gamma);
            }
            else
            {
                const auto parsed = space->is_string()
                                        ? Rendering::RenderSettings::ParseColorSpace(space->get<std::string>())
                                        : std::nullopt;
                if (!parsed)
                {
                    problems.push_back(std::format("rendering.colorSpace: {} is not \"gamma\" or \"linear\"",
                                                   space->dump()));
                }
                else
                {
                    Rendering::RenderSettings::SetColorSpace(*parsed);
                }
            }
        }
    }

    std::vector<std::string> ApplyProjectSettings(const nlohmann::json &settings,
                                                  const std::optional<std::set<std::string>> &only)
    {
        std::vector<std::string> problems;
        if (settings.is_null())
        {
            return problems;
        }
        if (!settings.is_object())
        {
            problems.emplace_back("settings: not an object");
            return problems;
        }

        // Each block on its own: a subsystem that throws on a malformed block mustn't keep the others from applying
        const auto apply = [&](const char *key, const auto &applyBlock)
        {
            if (only && !only->contains(key))
            {
                return;
            }
            const auto block = settings.find(key);
            if (block == settings.end() || block->is_null())
            {
                return;
            }
            try
            {
                applyBlock(*block);
            }
            catch (const std::exception &e)
            {
                problems.push_back(std::format("{}: {}", key, e.what()));
            }
        };

        apply("layers", [&](const nlohmann::json &layers)
        {
            if (!Layers::Deserialize(layers))
            {
                problems.emplace_back("layers: refused by Layers::Deserialize (see the log)");
            }
        });
        apply("input", [&](const nlohmann::json &input)
        {
            // The shape is checked even without an input system (an editor host without a window), so a block that
            // could never load isn't saved
            const auto maps = input.is_object() ? input.find("actionMaps") : input.end();
            if (!input.is_object() || maps == input.end() || !maps->is_object())
            {
                problems.emplace_back("input: expected {\"actionMaps\": {...}}");
                return;
            }
            Input::InputSystem *inputSystem = CurrentInputSystem();
            if (inputSystem != nullptr && !inputSystem->Deserialize(input))
            {
                problems.emplace_back("input: refused by InputSystem::Deserialize (see the log)");
            }
        });
        apply("physics", [&](const nlohmann::json &physics) { ApplyPhysics(physics, problems); });
        // Validated only: the tier is chosen once, by Application::Init (ReadMathSimdTier), so a change needs a restart
        apply("math", [&](const nlohmann::json &math)
        {
            if (!math.is_object())
            {
                problems.emplace_back("math: not an object");
                return;
            }
            (void)ReadMathSimdTier(nlohmann::json{{"math", math}}, &problems);
        });
        // Validated only: the seed is read before the Lua state is created (ReadDeterminismLuaSeed), so a change needs a restart
        apply("determinism", [&](const nlohmann::json &determinism)
        {
            if (!determinism.is_object())
            {
                problems.emplace_back("determinism: not an object");
                return;
            }
            (void)ReadDeterminismLuaSeed(nlohmann::json{{"determinism", determinism}}, &problems);
        });
        // Validated only: the preset is read before the physics backend is created (ReadNetworkSettings), so a change needs a restart
        apply("network", [&](const nlohmann::json &network)
        {
            if (!network.is_object())
            {
                problems.emplace_back("network: not an object");
                return;
            }
            (void)ReadNetworkSettings(nlohmann::json{{"network", network}}, &problems);
        });
        apply("rendering", [&](const nlohmann::json &rendering) { ApplyRendering(rendering, problems); });
        // A patch that removed the whole block (only names it, and it is gone): back to the default too
        if (only && only->contains("rendering"))
        {
            const auto block = settings.find("rendering");
            if (block == settings.end() || block->is_null())
            {
                Rendering::RenderSettings::SetColorSpace(Rendering::ColorSpace::Gamma);
                Rendering::RenderSettings::SetForceShaderEncode(false);
            }
        }
        return problems;
    }

    Physics::GpuMode ReadPhysicsGpuMode(const nlohmann::json &settings, std::vector<std::string> *problems)
    {
        if (!settings.is_object())
        {
            return Physics::GpuMode::Off;
        }
        const auto block = settings.find("physics");
        if (block == settings.end() || !block->is_object())
        {
            return Physics::GpuMode::Off;
        }
        const auto gpu = block->find("gpu");
        if (gpu == block->end() || gpu->is_null())
        {
            return Physics::GpuMode::Off;
        }
        if (gpu->is_string())
        {
            if (const auto mode = Physics::ParseGpuMode(gpu->get<std::string>()))
            {
                return *mode;
            }
        }
        if (problems)
        {
            problems->push_back(std::format("physics.gpu: {} is not \"off\", \"auto\" or \"on\"; using off", gpu->dump()));
        }
        return Physics::GpuMode::Off;
    }

    bool ReadPhysicsDeterministic(const nlohmann::json &settings, std::vector<std::string> *problems)
    {
        if (!settings.is_object())
        {
            return false;
        }
        const auto block = settings.find("physics");
        if (block == settings.end() || !block->is_object())
        {
            return false;
        }
        const auto value = block->find("deterministic");
        if (value == block->end() || value->is_null())
        {
            return false;
        }
        if (value->is_boolean())
        {
            return value->get<bool>();
        }
        if (problems)
        {
            problems->push_back(std::format("physics.deterministic: {} is not true or false; using false", value->dump()));
        }
        return false;
    }

    std::optional<Physics::PhysicsBackendKind> ReadPhysicsBackend(const nlohmann::json &settings,
                                                                  std::vector<std::string> *problems)
    {
        if (!settings.is_object())
        {
            return std::nullopt;
        }
        const auto block = settings.find("physics");
        if (block == settings.end() || !block->is_object())
        {
            return std::nullopt;
        }
        const auto value = block->find("backend");
        if (value == block->end() || value->is_null())
        {
            return std::nullopt;
        }
        if (value->is_string())
        {
            if (const auto kind = Physics::ParsePhysicsBackendKind(value->get<std::string>()))
            {
                return kind;
            }
        }
        if (problems)
        {
            problems->push_back(
                std::format("physics.backend: {} is not \"physx\" or \"jolt\"; using physx", value->dump()));
        }
        return Physics::PhysicsBackendKind::PhysX;
    }

    std::optional<std::uint64_t> ReadDeterminismLuaSeed(const nlohmann::json &settings,
                                                        std::vector<std::string> *problems)
    {
        if (!settings.is_object())
        {
            return std::nullopt;
        }
        const auto block = settings.find("determinism");
        if (block == settings.end() || !block->is_object())
        {
            return std::nullopt;
        }
        const auto seed = block->find("luaSeed");
        if (seed == block->end() || seed->is_null())
        {
            return std::nullopt;
        }
        // is_number_integer is true for a signed integer too: a negative one is refused below
        if (seed->is_number_unsigned())
        {
            return seed->get<std::uint64_t>();
        }
        if (seed->is_number_integer() && seed->get<std::int64_t>() >= 0)
        {
            return static_cast<std::uint64_t>(seed->get<std::int64_t>());
        }
        if (problems)
        {
            problems->push_back(std::format("determinism.luaSeed: {} is not a non-negative integer; leaving it unset",
                                            seed->dump()));
        }
        return std::nullopt;
    }

    Math::SIMDTierRequest ReadMathSimdTier(const nlohmann::json &settings, std::vector<std::string> *problems)
    {
        if (!settings.is_object())
        {
            return Math::SIMDTierRequest::Auto;
        }
        const auto block = settings.find("math");
        if (block == settings.end() || !block->is_object())
        {
            return Math::SIMDTierRequest::Auto;
        }
        const auto tier = block->find("simdTier");
        if (tier == block->end() || tier->is_null())
        {
            return Math::SIMDTierRequest::Auto;
        }
        if (tier->is_string())
        {
            if (const auto request = Math::ParseSIMDTierRequest(tier->get<std::string>()))
            {
                return *request;
            }
        }
        if (problems)
        {
            problems->push_back(std::format(
                "math.simdTier: {} is not \"auto\", \"scalar\", \"sse2\", \"sse4.1\", \"avx\" or \"neon\"; using auto",
                tier->dump()));
        }
        return Math::SIMDTierRequest::Auto;
    }

    Network::NetworkSettings ReadNetworkSettings(const nlohmann::json &settings, std::vector<std::string> *problems)
    {
        Network::NetworkSettings result;
        if (!settings.is_object())
        {
            return result;
        }
        const auto found = settings.find("network");
        if (found == settings.end() || !found->is_object())
        {
            return result;
        }
        const nlohmann::json &block = *found;
        const std::string path = "network";

        if (const auto preset = block.find("preset"); preset != block.end() && !preset->is_null())
        {
            const auto parsed = preset->is_string() ? Network::ParseNetworkPreset(preset->get<std::string>())
                                                    : std::nullopt;
            if (parsed)
            {
                result.preset = *parsed;
            }
            else
            {
                AddProblem(problems, std::format(
                    "network.preset: {} is not \"none\", \"authoritative\" or \"deterministic\"; using none",
                    preset->dump()));
            }
        }
        result.tickRate = ReadNetworkInt(block, "tickRate", path, "tickRate", problems);

        if (const auto *transport = ReadNetworkSubBlock(block, "transport", path, problems))
        {
            result.transport.udp = ReadNetworkBool(*transport, "udp", "network.transport", true, problems);
            result.transport.webSocket = ReadNetworkBool(*transport, "webSocket", "network.transport", true, problems);
            result.transport.port = ReadNetworkInt(*transport, "port", "network.transport", "transport.port", problems);
        }
        if (const auto *authoritative = ReadNetworkSubBlock(block, "authoritative", path, problems))
        {
            const std::string where = "network.authoritative";
            auto &out = result.authoritative;
            out.snapshotRate = ReadNetworkInt(*authoritative, "snapshotRate", where, "authoritative.snapshotRate",
                                              problems);
            out.interpolationDelayMs = ReadNetworkInt(*authoritative, "interpolationDelayMs", where,
                                                      "authoritative.interpolationDelayMs", problems);
            out.inputRedundancy = ReadNetworkInt(*authoritative, "inputRedundancy", where,
                                                 "authoritative.inputRedundancy", problems);
            out.predictionMaxMs = ReadNetworkInt(*authoritative, "predictionMaxMs", where,
                                                 "authoritative.predictionMaxMs", problems);
            out.smoothingMs = ReadNetworkInt(*authoritative, "smoothingMs", where, "authoritative.smoothingMs",
                                             problems);
            out.snapThreshold = ReadNetworkNumber(*authoritative, "snapThreshold", where,
                                                  "authoritative.snapThreshold", problems);
            out.lagCompMaxMs = ReadNetworkInt(*authoritative, "lagCompMaxMs", where, "authoritative.lagCompMaxMs",
                                              problems);
            if (const auto *dilation = ReadNetworkSubBlock(*authoritative, "timeDilation", where, problems))
            {
                const std::string dilationWhere = where + ".timeDilation";
                out.timeDilation.enabled = ReadNetworkBool(*dilation, "enabled", dilationWhere, false, problems);
                out.timeDilation.minScale = ReadDilationScale(*dilation, "minScale", 0.5, 1.0, 0.9, problems);
                out.timeDilation.maxScale = ReadDilationScale(*dilation, "maxScale", 1.0, 2.0, 1.1, problems);
            }
        }
        if (const auto *deterministic = ReadNetworkSubBlock(block, "deterministic", path, problems))
        {
            const std::string where = "network.deterministic";
            auto &out = result.deterministic;
            if (const auto mode = deterministic->find("mode"); mode != deterministic->end() && !mode->is_null())
            {
                const auto parsed = mode->is_string() ? Network::ParseDeterministicMode(mode->get<std::string>())
                                                      : std::nullopt;
                if (parsed)
                {
                    out.mode = *parsed;
                }
                else
                {
                    AddProblem(problems, std::format(
                        "network.deterministic.mode: {} is not \"lockstep\" or \"rollback\"; using lockstep",
                        mode->dump()));
                }
            }
            out.inputDelay = ReadNetworkInt(*deterministic, "inputDelay", where, "deterministic.inputDelay", problems);
            out.rollbackWindow = ReadNetworkInt(*deterministic, "rollbackWindow", where,
                                                "deterministic.rollbackWindow", problems);
            out.hashInterval = ReadNetworkInt(*deterministic, "hashInterval", where, "deterministic.hashInterval",
                                              problems);
        }

        if (const auto overrides = block.find("overrides"); overrides != block.end() && !overrides->is_null())
        {
            if (!overrides->is_object())
            {
                AddProblem(problems, std::format("network.overrides: {} is not an object; ignoring it",
                                                 overrides->dump()));
            }
            else
            {
                for (auto it = overrides->begin(); it != overrides->end(); ++it)
                {
                    if (Network::IsKnownSettingKey(it.key()))
                    {
                        result.overrides[it.key()] = it.value();
                    }
                    else
                    {
                        AddProblem(problems, std::format(
                            "network.overrides.{}: not a setting the network presets govern; ignored", it.key()));
                    }
                }
            }
        }
        result.customised = ReadNetworkBool(block, "customised", path, false, problems);
        return result;
    }

    ProjectSettingsSnapshot ProjectSettingsSnapshot::Capture()
    {
        ProjectSettingsSnapshot snapshot;
        snapshot.layers = Layers::Serialize();
        if (const Input::InputSystem *inputSystem = CurrentInputSystem())
        {
            snapshot.input = inputSystem->Serialize();
        }
        snapshot.fixedTimestep = Time::GetFixedTimestep();
        snapshot.colorSpace = Rendering::RenderSettings::GetColorSpace();
        snapshot.forceShaderEncode = Rendering::RenderSettings::GetForceShaderEncode();
        if (const Physics::IPhysicsBackend *backend = Application::GetInstance().Get3DPhysicsBackend())
        {
            snapshot.gravity = backend->GetGravity();
        }
        return snapshot;
    }

    void ProjectSettingsSnapshot::Restore() const
    {
        (void)Layers::Deserialize(layers);
        if (input)
        {
            if (Input::InputSystem *inputSystem = CurrentInputSystem())
            {
                (void)inputSystem->Deserialize(*input);
            }
        }
        (void)Time::SetFixedTimestep(fixedTimestep);
        Rendering::RenderSettings::SetColorSpace(colorSpace);
        Rendering::RenderSettings::SetForceShaderEncode(forceShaderEncode);
        if (gravity)
        {
            if (Physics::IPhysicsBackend *backend = Application::GetInstance().Get3DPhysicsBackend())
            {
                backend->SetGravity(*gravity);
            }
        }
    }
}
