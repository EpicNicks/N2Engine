#include "engine/ProjectSettings.hpp"

#include <exception>
#include <format>

#include "engine/Application.hpp"
#include "engine/Layers.hpp"
#include "engine/Time.hpp"
#include "engine/Window.hpp"
#include "engine/input/InputSystem.hpp"
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
        apply("rendering", [&](const nlohmann::json &rendering) { ApplyRendering(rendering, problems); });
        // A patch that removed the whole block (only names it, and it is gone): back to the default too
        if (only && only->contains("rendering"))
        {
            const auto block = settings.find("rendering");
            if (block == settings.end() || block->is_null())
            {
                Rendering::RenderSettings::SetColorSpace(Rendering::ColorSpace::Gamma);
            }
        }
        return problems;
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
        if (gravity)
        {
            if (Physics::IPhysicsBackend *backend = Application::GetInstance().Get3DPhysicsBackend())
            {
                backend->SetGravity(*gravity);
            }
        }
    }
}
