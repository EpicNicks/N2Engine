#include "engine/scripting/bindings/LuaBindings.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/GameObject.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Time.hpp"
#include "engine/network/InputLayout.hpp"
#include "engine/network/NetworkClient.hpp"
#include "engine/network/NetworkObject.hpp"
#include "engine/network/NetworkServer.hpp"

#include <math/Vector2.hpp>
#include <math/Vector3.hpp>

#include <algorithm>
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

    namespace
    {
        /// The server's tick and tick rate as this process knows them: the client's estimate when it runs a client (nothing
        /// until the clock has synced), else the server's own count
        struct ClockView
        {
            bool known = false;
            double tick = 0.0;
            double rate = 0.0;
        };

        ClockView CurrentClock()
        {
            ClockView view;
            if (Network::NetworkClient *const client = Network::NetworkClient::Active())
            {
                if (client->ClockSynced() && client->ServerTickRate() != 0)
                {
                    view.known = true;
                    view.tick = client->ServerTick();
                    view.rate = static_cast<double>(client->ServerTickRate());
                }
                return view;
            }
            if (Network::NetworkServer *const server = Network::NetworkServer::Active())
            {
                view.known = true;
                view.tick = static_cast<double>(server->CurrentTick());
                view.rate = static_cast<double>(server->TickRate());
            }
            return view;
        }

        /// Network.GetServerTime(): the server's time in seconds (its tick divided by its tick rate); on a client the
        /// estimate, which never decreases. nil until a client has synced its clock, and when this process runs neither.
        sol::object GetServerTime(sol::this_state state)
        {
            const ClockView view = CurrentClock();
            if (!view.known)
            {
                return sol::make_object(state, sol::lua_nil);
            }
            return sol::make_object(state, view.tick / view.rate);
        }

        /// Network.GetServerTick(): the server's tick, fractional on a client (an estimate)
        sol::object GetServerTick(sol::this_state state)
        {
            const ClockView view = CurrentClock();
            if (!view.known)
            {
                return sol::make_object(state, sol::lua_nil);
            }
            return sol::make_object(state, view.tick);
        }

        /// Network.GetRtt() and Network.GetJitter(): the round trip to the server and its jitter, in seconds; nil on a
        /// process that runs no client, or before its clock has synced
        sol::object ClientSeconds(sol::this_state state, const bool jitter)
        {
            Network::NetworkClient *const client = Network::NetworkClient::Active();
            if (client == nullptr || !client->ClockSynced())
            {
                return sol::make_object(state, sol::lua_nil);
            }
            const std::uint64_t micros = jitter ? client->JitterMicros() : client->RttMicros();
            return sol::make_object(state, static_cast<double>(micros) / 1.0e6);
        }

        /// Network.GetPredictionTuning(): { positionEpsilon, velocityEpsilon, snapDistance, smoothingMs, applySmoothing } of
        /// the client's prediction; nil when this process runs no client or its prediction is off
        sol::object GetPredictionTuning(sol::this_state state)
        {
            Network::NetworkClient *const client = Network::NetworkClient::Active();
            if (client == nullptr || !client->PredictionActive())
            {
                return sol::make_object(state, sol::lua_nil);
            }
            const Network::PredictionTuning tuning = client->GetPredictionTuning();
            sol::table table = sol::state_view(state).create_table();
            table["positionEpsilon"] = tuning.positionEpsilon;
            table["velocityEpsilon"] = tuning.velocityEpsilon;
            table["snapDistance"] = tuning.snapDistance;
            table["smoothingMs"] = static_cast<double>(tuning.smoothingMicros) / 1000.0;
            table["applySmoothing"] = tuning.applySmoothing;
            return sol::make_object(state, table);
        }

        /// Network.SetPredictionTuning(t): the fields present in `t` replace the client's; an unknown field, or one of the
        /// wrong type, is an error and changes nothing. Returns false when there is no client with prediction on.
        bool SetPredictionTuning(const sol::table &fields)
        {
            Network::NetworkClient *const client = Network::NetworkClient::Active();
            if (client == nullptr || !client->PredictionActive())
            {
                return false;
            }
            Network::PredictionTuning tuning = client->GetPredictionTuning();
            for (const auto &pair : fields)
            {
                if (pair.first.get_type() != sol::type::string)
                {
                    throw std::runtime_error("Network.SetPredictionTuning: the keys must be field names");
                }
                const std::string key = pair.first.as<std::string>();
                if (key == "applySmoothing")
                {
                    if (pair.second.get_type() != sol::type::boolean)
                    {
                        throw std::runtime_error("Network.SetPredictionTuning: applySmoothing must be a boolean");
                    }
                    tuning.applySmoothing = pair.second.as<bool>();
                    continue;
                }
                if (pair.second.get_type() != sol::type::number)
                {
                    throw std::runtime_error(std::format("Network.SetPredictionTuning: {} must be a number", key));
                }
                const double value = pair.second.as<double>();
                if (!std::isfinite(value) || value < 0.0)
                {
                    throw std::runtime_error(std::format("Network.SetPredictionTuning: {} must be a number of 0 or more", key));
                }
                if (key == "positionEpsilon")
                {
                    tuning.positionEpsilon = static_cast<float>(value);
                }
                else if (key == "velocityEpsilon")
                {
                    tuning.velocityEpsilon = static_cast<float>(value);
                }
                else if (key == "snapDistance")
                {
                    tuning.snapDistance = static_cast<float>(value);
                }
                else if (key == "smoothingMs")
                {
                    tuning.smoothingMicros = static_cast<std::uint64_t>(std::min(value, 5000.0) * 1000.0);
                }
                else
                {
                    throw std::runtime_error(std::format("Network.SetPredictionTuning: unknown field '{}'", key));
                }
            }
            return client->SetPredictionTuning(tuning);
        }
    }

    void BindNetwork(LuaRuntime &runtime)
    {
        auto &lua = runtime.GetState();
        // Lambdas, not the function pointers: sol catches a C++ exception around a lambda and turns it into a Lua error, but
        // an exception thrown out of a bare function pointer would unwind through Lua's C frames and leave its call stack
        // (NetworkInputLuaTest.AFailedCallLeavesTheLuaCallStackAsItWas)
        // The table is shared with BindNetworkInterest (netcode stage B9): add to it, never replace it
        sol::table network = lua["Network"].get_or_create<sol::table>();
        network["DeclareInputLayout"] = [](const sol::object &controls) { DeclareInputLayout(controls); };
        network["GetInput"] = [](const GameObjectRef *target, sol::this_state state) { return GetInput(target, state); };
        network["GetServerTime"] = [](sol::this_state state) { return GetServerTime(state); };
        network["GetServerTick"] = [](sol::this_state state) { return GetServerTick(state); };
        network["GetRtt"] = [](sol::this_state state) { return ClientSeconds(state, false); };
        network["GetJitter"] = [](sol::this_state state) { return ClientSeconds(state, true); };
        network["GetPredictionTuning"] = [](sol::this_state state) { return GetPredictionTuning(state); };
        network["SetPredictionTuning"] = [](const sol::table &fields) { return SetPredictionTuning(fields); };
    }
}
