#include "engine/ProjectSettings.hpp"

#include <exception>
#include <format>

#include <nlohmann/json.hpp>

#include <math/Vector3.hpp>

#include "engine/Application.hpp"
#include "engine/Layers.hpp"
#include "engine/Time.hpp"
#include "engine/Window.hpp"
#include "engine/input/InputSystem.hpp"
#include "engine/physics/IPhysicsBackend.hpp"

namespace N2Engine
{
    namespace
    {
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
    }

    std::vector<std::string> ApplyProjectSettings(const nlohmann::json &settings)
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
            Input::InputSystem *inputSystem = Application::GetInstance().GetWindow().GetInputSystem();
            if (inputSystem != nullptr && !inputSystem->Deserialize(input))
            {
                problems.emplace_back("input: refused by InputSystem::Deserialize (expected {\"actionMaps\": {...}})");
            }
        });
        apply("physics", [&](const nlohmann::json &physics) { ApplyPhysics(physics, problems); });
        return problems;
    }
}
