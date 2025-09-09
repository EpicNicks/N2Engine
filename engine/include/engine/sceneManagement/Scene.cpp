#include "engine/sceneManagement/Scene.hpp"

#include <algorithm>

using namespace N2Engine;

bool Scene::DestroyGameObject(std::shared_ptr<GameObject> gameObject)
{
    if (gameObject.get() == nullptr)
    {
        return false;
    }

    auto it = std::find(_sceneGameObjects.begin(), _sceneGameObjects.end(), gameObject);
    if (it != _sceneGameObjects.end())
    {
        _sceneGameObjects.erase(it);
        return true;
    }

    return false;
}