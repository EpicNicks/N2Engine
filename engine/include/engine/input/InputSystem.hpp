#pragma once

namespace N2Engine
{
    namespace Input
    {
        class InputSystem
        {
            // TODO: create an interface for assigning input events to glfw input
            //  possibility 1: map of key -> EventHandler<void>
            //  need to also account for controller input which would pass a vector
            //  maybe include a map of key -> EventHandler<Vector2> etc

            // for now should be a simple mapping to glfw but should be portable for changes which could require owning windowing
        };
    }
}