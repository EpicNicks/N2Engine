#pragma once

#include <string>
#include <memory>
#include <vector>

class GameObject;

namespace N2Engine
{
    struct Scene
    {

    private:
        std::vector<std::shared_ptr<GameObject>> _sceneGameObjects;

    public:
        std::string sceneName;

        bool DestroyGameObject(std::shared_ptr<GameObject> gameObject);
    };
}