#pragma once

#include <math/Vector3.hpp>

#include "engine/Camera.hpp"

namespace N2Engine::Rendering
{
    /// The directions of the camera a frame is drawn with, in world space
    struct CameraBasis
    {
        Math::Vector3 right{1.0f, 0.0f, 0.0f};
        Math::Vector3 up{0.0f, 1.0f, 0.0f};
        /// The way the camera looks
        Math::Vector3 forward{0.0f, 0.0f, -1.0f};
    };

    /**
     * The camera of the frame being drawn, for renderables that face it (particle billboards): IRenderable draws
     * get no camera, so Scene::Render makes its camera current for the length of the draw. Outside a Scene::Render
     * (a renderable drawn directly) the current basis is the default one, a camera with no rotation looking
     * down -Z. Main thread only.
     */
    class RenderCamera
    {
    public:
        [[nodiscard]] static const CameraBasis &Current() { return Slot(); }

        /// Makes a camera current until it goes out of scope, then restores the one before
        class Scope
        {
        public:
            explicit Scope(const Camera &camera) : _previous(Slot())
            {
                Slot() = CameraBasis{camera.GetRight(), camera.GetUp(), camera.GetForward()};
            }
            ~Scope() { Slot() = _previous; }
            Scope(const Scope &) = delete;
            Scope &operator=(const Scope &) = delete;

        private:
            CameraBasis _previous;
        };

    private:
        static CameraBasis &Slot()
        {
            static CameraBasis basis;
            return basis;
        }
    };
}
