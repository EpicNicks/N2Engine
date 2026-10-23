#include "engine/GameObjectScene.hpp"
#include "engine/network/NetworkObject.hpp"
#include "engine/network/NetworkServer.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/scripting/bindings/LuaBindings.hpp"
#include <stdexcept>

namespace N2Engine::Scripting::Bindings
{
    void BindNetworkInterest(LuaRuntime &runtime)
    {
        auto &lua = runtime.GetState();
        sol::table network = lua["Network"].get_or_create<sol::table>();
        network.set_function("SetInterestPolicy", [](const GameObjectRef &object, const std::string &mode, float radius,
                                                     float priority) {
            auto *server = Network::NetworkServer::PhysicsQueryServer();
            if (!server)
                return false;
            auto go = object.Pin();
            auto *component = go->GetComponent<Network::NetworkObject>();
            if (!component || server->World().Find(component->GetNetworkId()) != component)
                return false;
            Network::Relevance relevance;
            if (mode == "always")
                relevance = Network::Relevance::Always;
            else if (mode == "owner")
                relevance = Network::Relevance::OwnerOnly;
            else if (mode == "radius")
                relevance = Network::Relevance::Radius;
            else
                throw std::runtime_error("Network.SetInterestPolicy: expected always, owner or radius");
            return component->SetInterestPolicy({relevance, radius, priority});
        });
        network.set_function("SetObserver", [](Network::ClientId client, const GameObjectRef &object) {
            auto *server = Network::NetworkServer::PhysicsQueryServer();
            auto go = object.Pin();
            auto *component = go->GetComponent<Network::NetworkObject>();
            return server && component && server->World().Find(component->GetNetworkId()) == component &&
                   server->SetObserver(client, component->GetNetworkId());
        });
        network.set_function("ClearObserver", [](Network::ClientId client) {
            auto *server = Network::NetworkServer::PhysicsQueryServer();
            return server && server->SetObserver(client, {});
        });
    }
} // namespace N2Engine::Scripting::Bindings
