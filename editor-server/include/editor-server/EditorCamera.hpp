#pragma once

#include <engine/Camera.hpp>
#include <math/Quaternion.hpp>
#include <math/Vector3.hpp>

#include <cmath>
#include <optional>
#include <string>

namespace N2Engine::Editor
{
    /**
     * The editor's viewport camera (#79, E7a): where the scene view looks from. The server owns it: it is not a scene
     * object, is never saved with a scene and is not part of undo, so an edit-mode scene with no camera of its own can
     * still be viewed. The client moves it (orbit, pan, zoom and fly are computed client-side) with SetEditorCamera,
     * and GetEditorCamera answers with the exact view and projection frames are rendered with.
     *
     * The aspect ratio is not part of the state: it is the viewport's (SetViewportSize), so a resize needs no camera
     * update. Angles are degrees, as in Camera.
     */
    struct EditorCameraState
    {
        Math::Vector3 position{0.0f, 0.0f, 10.0f};
        /// A unit quaternion; the camera looks down its local -Z (Camera's convention)
        Math::Quaternion rotation = Math::Quaternion::Identity;
        /// The vertical field of view of a perspective camera, in degrees
        float fovY = 60.0f;
        bool orthographic = false;
        /// An orthographic camera's half height in world units (the width follows the viewport's aspect ratio)
        float orthoSize = 5.0f;
        float nearPlane = 0.1f;
        float farPlane = 1000.0f;

        /// The limits a client's values must stay within. They keep the matrices finite and the depth range usable.
        static constexpr float MaxCoordinate = 1.0e6f;
        static constexpr float MinFovY = 1.0f;
        static constexpr float MaxFovY = 179.0f;
        static constexpr float MinOrthoSize = 1.0e-4f;
        static constexpr float MaxOrthoSize = 1.0e6f;
        /// A perspective camera's near plane must be at least this (a plane at the eye has no projection)
        static constexpr float MinPerspectiveNear = 1.0e-4f;
        static constexpr float MaxNear = 1.0e6f;
        /// An orthographic camera may have a near plane behind it, down to this
        static constexpr float MinOrthographicNear = -1.0e6f;
        static constexpr float MaxFar = 1.0e7f;
        /// The far plane must be at least this much beyond the near plane
        static constexpr float MinDepthRange = 1.0e-3f;

        /// Exactly equal, number for number (Vector3's and Quaternion's operator== allow a small difference, which would
        /// swallow a camera moved by a tiny step: it must still make a new frame)
        bool operator==(const EditorCameraState &other) const
        {
            return position.x == other.position.x && position.y == other.position.y && position.z == other.position.z &&
                   rotation.GetX() == other.rotation.GetX() && rotation.GetY() == other.rotation.GetY() &&
                   rotation.GetZ() == other.rotation.GetZ() && rotation.GetW() == other.rotation.GetW() &&
                   fovY == other.fovY && orthographic == other.orthographic && orthoSize == other.orthoSize &&
                   nearPlane == other.nearPlane && farPlane == other.farPlane;
        }

        /**
         * The Camera frames are rendered with, at the viewport's aspect ratio (width / height; a non-finite or
         * non-positive one is replaced by 1). A perspective camera is SetPerspective(fovY, aspect, near, far); an
         * orthographic one spans orthoSize above and below its centre and orthoSize * aspect to each side.
         */
        [[nodiscard]] Camera ToCamera(float aspect) const
        {
            if (!std::isfinite(aspect) || !(aspect > 0.0f))
            {
                aspect = 1.0f;
            }
            Camera camera;
            if (orthographic)
            {
                const float halfWidth = orthoSize * aspect;
                camera.SetOrthographic(-halfWidth, halfWidth, -orthoSize, orthoSize, nearPlane, farPlane);
            }
            else
            {
                camera.SetPerspective(fovY, aspect, nearPlane, farPlane);
            }
            camera.SetPosition(position);
            camera.SetRotation(rotation);
            return camera;
        }
    };

    /// The result of checking a client's camera: the state to store (the rotation normalised), or why it was refused
    struct EditorCameraCheck
    {
        std::optional<EditorCameraState> state;
        std::string error;
    };

    namespace EditorCameraDetail
    {
        inline bool Finite(const Math::Vector3 &v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        inline bool WithinCoordinates(const Math::Vector3 &v)
        {
            return std::fabs(v.x) <= EditorCameraState::MaxCoordinate && std::fabs(v.y) <= EditorCameraState::MaxCoordinate &&
                   std::fabs(v.z) <= EditorCameraState::MaxCoordinate;
        }
    }

    /**
     * Checks a camera a client sent, as the untrusted input it is. Every number must be finite (NaN and infinities
     * are refused, never clamped), within the limits of EditorCameraState, and consistent: a perspective camera has a
     * near plane in front of it, and every camera a far plane beyond the near one. The rotation is normalised (a
     * client may have accumulated rounding); a zero quaternion, or one too large to normalise, is refused. Only the
     * values of the projection that is in use are checked: an orthographic camera's fovY is kept as sent when it is
     * finite, but isn't limited (and the reverse for a perspective camera's orthoSize), so a client that stores both
     * can switch projections without resending.
     */
    [[nodiscard]] inline EditorCameraCheck CheckEditorCamera(EditorCameraState camera)
    {
        using State = EditorCameraState;
        EditorCameraCheck result;

        if (!EditorCameraDetail::Finite(camera.position))
        {
            result.error = "The camera position has a value that isn't a finite number";
            return result;
        }
        if (!EditorCameraDetail::WithinCoordinates(camera.position))
        {
            result.error = "The camera position is out of range (each coordinate must be within +-1000000)";
            return result;
        }

        const Math::Quaternion &rotation = camera.rotation;
        if (!std::isfinite(rotation.GetX()) || !std::isfinite(rotation.GetY()) || !std::isfinite(rotation.GetZ()) ||
            !std::isfinite(rotation.GetW()))
        {
            result.error = "The camera rotation has a value that isn't a finite number";
            return result;
        }
        const float rotationLength = rotation.Length();
        if (!std::isfinite(rotationLength) || !(rotationLength > 1e-6f))
        {
            result.error = "The camera rotation is a zero quaternion, or too large to normalise";
            return result;
        }
        camera.rotation = rotation.Normalized();

        if (!std::isfinite(camera.fovY) || !std::isfinite(camera.orthoSize) || !std::isfinite(camera.nearPlane) ||
            !std::isfinite(camera.farPlane))
        {
            result.error = "The camera has a projection value that isn't a finite number";
            return result;
        }

        if (camera.orthographic)
        {
            if (!(camera.orthoSize >= State::MinOrthoSize && camera.orthoSize <= State::MaxOrthoSize))
            {
                result.error = "The orthographic size is out of range (0.0001 to 1000000)";
                return result;
            }
            if (!(camera.nearPlane >= State::MinOrthographicNear && camera.nearPlane <= State::MaxNear))
            {
                result.error = "The near plane is out of range (-1000000 to 1000000 for an orthographic camera)";
                return result;
            }
        }
        else
        {
            if (!(camera.fovY >= State::MinFovY && camera.fovY <= State::MaxFovY))
            {
                result.error = "The field of view is out of range (1 to 179 degrees)";
                return result;
            }
            if (!(camera.nearPlane >= State::MinPerspectiveNear && camera.nearPlane <= State::MaxNear))
            {
                result.error = "The near plane is out of range (0.0001 to 1000000 for a perspective camera)";
                return result;
            }
        }
        if (!(camera.farPlane <= State::MaxFar) || !(camera.farPlane - camera.nearPlane >= State::MinDepthRange))
        {
            result.error = "The far plane must be beyond the near plane (by at least 0.001) and at most 10000000";
            return result;
        }

        result.state = camera;
        return result;
    }
}
