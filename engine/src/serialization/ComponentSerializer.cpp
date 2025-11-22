#include "engine/serialization/ComponentSerializer.hpp"
#include "engine/GameObject.hpp"
#include "engine/serialization/ReferenceResolver.hpp"

using namespace N2Engine;
using json = nlohmann::json;

void SerializableComponent::RegisterGameObjectRef(const std::string &name, GameObject* gameObjectRef)
{
    _members.emplace_back(
        name,
        // Serialize: store UUID
        [&gameObjectRef, name](json &j)
        {
            if (gameObjectRef)
            {
                j[name] = gameObjectRef->GetUUID().ToString();
            }
            else
            {
                j[name] = nullptr;
            }
        },
        // Deserialize: schedule for later resolution
        [&gameObjectRef, name](const json &j, ReferenceResolver *resolver)
        {
            if (!j.contains(name) || j[name].is_null())
            {
                gameObjectRef = nullptr;
                return;
            }

            std::string uuidStr = j[name].get<std::string>();
            Math::UUID uuid(uuidStr);

            if (resolver)
            {
                // Add pending resolution - will be resolved after all objects loaded
                resolver->AddPendingReference([&gameObjectRef, uuid, resolver](){ gameObjectRef = resolver->FindGameObject(uuid); });
            }
        });
}

void SerializableComponent::RegisterGameObjectRefVector(const std::string &name, std::vector<GameObject*> &gameObjectRefs)
{
    _members.emplace_back(
        name,
        // Serialize: store array of UUIDs
        [&gameObjectRefs, name](json &j)
        {
            json arr = json::array();
            for (const auto &go : gameObjectRefs)
            {
                if (go)
                {
                    arr.push_back(go->GetUUID().ToString());
                }
                else
                {
                    arr.push_back(nullptr);
                }
            }
            j[name] = arr;
        },
        // Deserialize: schedule for later resolution
        [&gameObjectRefs, name](const json &j, ReferenceResolver *resolver)
        {
            gameObjectRefs.clear();

            if (!j.contains(name))
                return;

            const json &arr = j[name];
            if (!arr.is_array())
                return;

            // Pre-allocate space
            gameObjectRefs.resize(arr.size());

            if (resolver)
            {
                for (size_t i = 0; i < arr.size(); ++i)
                {
                    if (arr[i].is_null())
                    {
                        continue;
                    }

                    std::string uuidStr = arr[i].get<std::string>();
                    Math::UUID uuid(uuidStr);

                    // Capture index for resolution
                    resolver->AddPendingReference([&gameObjectRefs, i, uuid, resolver]()
                                                  { gameObjectRefs[i] = resolver->FindGameObject(uuid); });
                }
            }
        });
}