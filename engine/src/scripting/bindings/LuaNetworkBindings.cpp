#include "engine/scripting/bindings/LuaBindings.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/GameObject.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Time.hpp"
#include "engine/network/InputLayout.hpp"
#include "engine/network/NetworkObject.hpp"
#include "engine/network/NetworkServer.hpp"

#include <math/Vector2.hpp>
#include <math/Vector3.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace N2Engine::Scripting::Bindings
{
    namespace
    {
        [[noreturn]] void Fail(const std::string &text)
        {
            throw std::runtime_error("Network.DeclareInputLayout: " + text);
        }

        /// One control table of the list: { name = "jump", kind = "button" }, { name = "move", kind = "vector2", bits = 8,
        /// range = 1, signed = true }. Every field is type-checked; unknown fields are an error (a typo must not pass).
        Network::InputControlSpec ParseControl(const sol::table &entry, const std::size_t index)
        {
            Network::InputControlSpec spec;
            bool haveName = false;
            bool haveKind = false;
            for (const auto &pair : entry)
            {
                if (pair.first.get_type() != sol::type::string)
                {
                    Fail(std::format("control {} has a key that is not a name", index));
                }
                const std::string key = pair.first.as<std::string>();
                const sol::object &value = pair.second;
                if (key == "name")
                {
                    if (value.get_type() != sol::type::string)
                    {
                        Fail(std::format("control {}: name must be a string", index));
                    }
                    spec.name = value.as<std::string>();
                    haveName = true;
                }
                else if (key == "kind")
                {
                    if (value.get_type() != sol::type::string)
                    {
                        Fail(std::format("control {}: kind must be \"button\", \"axis\", \"vector2\" or \"vector3\"", index));
                    }
                    const std::optional<Network::InputControlKind> kind =
                        Network::ParseInputControlKind(value.as<std::string>());
                    if (!kind.has_value())
                    {
                        Fail(std::format("control {}: kind must be \"button\", \"axis\", \"vector2\" or \"vector3\"", index));
                    }
                    spec.kind = *kind;
                    haveKind = true;
                }
                else if (key == "bits")
                {
                    if (value.get_type() != sol::type::number)
                    {
                        Fail(std::format("control {}: bits must be a whole number", index));
                    }
                    const double bits = value.as<double>();
                    if (!(bits >= 1.0 && bits <= 255.0) || bits != std::floor(bits))
                    {
                        Fail(std::format("control {}: bits must be a whole number from 1 to {}", index,
                                         static_cast<unsigned>(Network::kMaxInputComponentBits)));
                    }
                    spec.bits = static_cast<std::uint8_t>(bits);
                }
                else if (key == "range")
                {
                    if (value.get_type() != sol::type::number)
                    {
                        Fail(std::format("control {}: range must be a number", index));
                    }
                    spec.range = static_cast<float>(value.as<double>());
                }
                else if (key == "signed")
                {
                    if (value.get_type() != sol::type::boolean)
                    {
                        Fail(std::format("control {}: signed must be true or false", index));
                    }
                    spec.isSigned = value.as<bool>();
                }
                else
                {
                    Fail(std::format("control {} has a field that is not name, kind, bits, range or signed", index));
                }
            }
            if (!haveName || !haveKind)
            {
                Fail(std::format("control {} needs a name and a kind", index));
            }
            return spec;
        }

        void DeclareInputLayout(const sol::object &argument)
        {
            if (argument.get_type() != sol::type::table)
            {
                Fail("pass a list of control tables");
            }
            const sol::table list = argument.as<sol::table>();
            const std::size_t count = list.size();
            if (count > Network::kMaxInputControls)
            {
                Fail(std::format("a layout holds at most {} controls, {} were given", Network::kMaxInputControls, count));
            }
            // Only the list part: a table with other keys is a mistake, not a layout
            std::size_t keys = 0;
            for (const auto &pair : list)
            {
                (void)pair;
                ++keys;
                if (keys > count)
                {
                    Fail("pass a plain list of control tables, with no other keys");
                }
            }

            std::vector<Network::InputControlSpec> specs;
            specs.reserve(count);
            for (std::size_t i = 1; i <= count; ++i)
            {
                const sol::object entry = list.get<sol::object>(i);
                if (entry.get_type() != sol::type::table)
                {
                    Fail(std::format("control {} is not a table", i));
                }
                specs.push_back(ParseControl(entry.as<sol::table>(), i));
            }
            std::expected<Network::InputLayout, std::string> layout = Network::InputLayout::Create(std::move(specs));
            if (!layout)
            {
                Fail(layout.error());
            }
            Network::SetDeclaredInputLayout(std::move(*layout));
        }

        /// Network.GetInput(gameObject): the input of the client that owns the object, for the current fixed tick, as a table
        /// of the layout's controls (a button is a boolean, an axis a number, a vector a Vector2 or Vector3), and a second
        /// value, "frame", "repeated" or "neutral". nil when this process runs no network server, the object has no
        /// NetworkObject, the server owns it, or its owner has no input.
        sol::variadic_results GetInput(const GameObjectRef *target, sol::this_state state)
        {
            if (target == nullptr)
            {
                throw std::runtime_error("Network.GetInput: pass a GameObject");
            }
            const GameObject::Ptr gameObject = target->Pin();
            sol::variadic_results results;
            Network::NetworkServer *const server = Network::NetworkServer::Active();
            if (server == nullptr)
            {
                return results;
            }
            const Network::NetworkObject *const object = gameObject->GetComponent<Network::NetworkObject>();
            if (object == nullptr || !object->HasNetworkId())
            {
                return results;
            }
            const std::optional<Network::ControlledInput> input =
                server->InputFor(object->GetNetworkId(), static_cast<std::uint32_t>(Time::GetTick()));
            if (!input.has_value())
            {
                return results;
            }

            sol::state_view lua(state);
            sol::table table = lua.create_table();
            const Network::InputLayout &layout = server->GetInputLayout();
            for (std::size_t i = 0; i < layout.ControlCount(); ++i)
            {
                const Network::InputControlSpec &control = layout.Control(i);
                const std::size_t offset = layout.ValueOffset(i);
                switch (control.kind)
                {
                case Network::InputControlKind::Button:
                    table[control.name] = input->values[offset] >= 0.5f;
                    break;
                case Network::InputControlKind::Axis:
                    table[control.name] = input->values[offset];
                    break;
                case Network::InputControlKind::Vector2:
                    table[control.name] = Math::Vector2(input->values[offset], input->values[offset + 1]);
                    break;
                case Network::InputControlKind::Vector3:
                    table[control.name] =
                        Math::Vector3(input->values[offset], input->values[offset + 1], input->values[offset + 2]);
                    break;
                }
            }
            results.push_back(sol::make_object(state, table));
            results.push_back(sol::make_object(state, std::string(Network::ToString(input->source))));
            return results;
        }
    }

    void BindNetwork(LuaRuntime &runtime)
    {
        auto &lua = runtime.GetState();
        // Lambdas, not the function pointers: sol catches a C++ exception around a lambda and turns it into a Lua error, but
        // an exception thrown out of a bare function pointer would unwind through Lua's C frames and leave its call stack
        // (NetworkInputLuaTest.AFailedCallLeavesTheLuaCallStackAsItWas)
        lua["Network"] = lua.create_table_with(
            "DeclareInputLayout", [](const sol::object &controls) { DeclareInputLayout(controls); },
            "GetInput", [](const GameObjectRef *target, sol::this_state state) { return GetInput(target, state); });
    }
}
