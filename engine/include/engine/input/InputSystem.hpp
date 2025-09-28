#pragma once

#include <unordered_map>
#include <memory>
#include <string>

namespace N2Engine
{
    class Window;

    namespace Input
    {
        class ActionMap;

        class InputSystem
        {
        private:
            Window &_window;
            std::unordered_map<std::string, std::unique_ptr<ActionMap>> _actionMaps;
            std::string _curActionMapName;

        public:
            InputSystem(Window &window) : _window{window} {};
            ~InputSystem();

            ActionMap *LoadActionMap(std::string name);
            void AddActionMap(std::unique_ptr<ActionMap> &&actionMap);

            ActionMap *GetCurActionMap();

            void Update();
        };
    }
}