#pragma once

#include <cstdint>
#include <string>

#include "engine/serialization/ComponentSerializer.hpp"

namespace N2Engine::UI
{
    /// How a canvas is drawn. Only the screen-space overlay exists so far; a world-space canvas comes later (#42).
    enum class CanvasRenderMode : std::uint8_t
    {
        ScreenSpaceOverlay
    };

    /**
     * The root of a UI tree. Its object's descendants with UI components (Image, ...) are laid out inside the
     * canvas rect and drawn by the UI pass after the scene; see UISystem.
     *
     * A screen-space overlay canvas covers the whole viewport: its rect is (0, 0, width, height) in window
     * coordinates' units, whatever the RectTransform on its object says. Canvases draw in ascending sortOrder
     * (ties in hierarchy order), so a higher sortOrder is on top and is hit first.
     *
     * Only canvases with no Canvas above them in the hierarchy count. A Canvas under another canvas is ignored
     * (its object is laid out and drawn as an ordinary element of the outer canvas), whether it is enabled
     * or not. A disabled root Canvas component, like an inactive object, hides its whole tree from drawing
     * and hit tests.
     */
    class Canvas final : public SerializableComponent
    {
    public:
        explicit Canvas(GameObject &gameObject);

        [[nodiscard]] std::string GetTypeName() const override { return "Canvas"; }

        [[nodiscard]] int GetSortOrder() const { return _sortOrder; }
        void SetSortOrder(const int sortOrder) { _sortOrder = sortOrder; }

        [[nodiscard]] CanvasRenderMode GetRenderMode() const { return CanvasRenderMode::ScreenSpaceOverlay; }

        /// One per object: AddComponent returns the existing one
        static constexpr bool IsSingleton = true;

    private:
        int _sortOrder = 0;
    };
}
