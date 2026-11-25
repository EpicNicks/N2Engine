#pragma once

#include <cstdint>
#include <string>

#include <math/Vector2.hpp>
#include <math/VectorN.hpp>
#include <renderer/common/Renderer.hpp>

#include "engine/serialization/ComponentSerializer.hpp"
#include "engine/serialization/FieldInfo.hpp"

namespace N2Engine
{
    using Vector2i = Math::VectorN<int, 2>; // as in Window.hpp
}

namespace N2Engine::UI
{
    /// How a CanvasScaler picks the size of a canvas unit (Unity's CanvasScaler.uiScaleMode, without physical sizes)
    enum class CanvasScaleMode : std::uint8_t
    {
        /// A canvas unit is scaleFactor pixels
        ConstantPixelSize,
        /// A canvas unit is whatever makes the viewport match the reference resolution (the default)
        ScaleWithScreenSize
    };

    /// How ScaleWithScreenSize fits a viewport whose aspect differs from the reference's (Unity's screenMatchMode)
    enum class ScreenMatchMode : std::uint8_t
    {
        /// Between the width's and the height's ratios, by matchWidthOrHeight (the default)
        MatchWidthOrHeight,
        /// The canvas is never smaller than the reference on either axis: the smaller ratio
        Expand,
        /// The canvas is never larger than the reference on either axis: the larger ratio
        Shrink
    };

    N2_SERIALIZE_ENUM(CanvasScaleMode, {
                      { CanvasScaleMode::ScaleWithScreenSize, "ScaleWithScreenSize" },
                      { CanvasScaleMode::ConstantPixelSize, "ConstantPixelSize" }
                      })

    N2_SERIALIZE_ENUM(ScreenMatchMode, {
                      { ScreenMatchMode::MatchWidthOrHeight, "MatchWidthOrHeight" },
                      { ScreenMatchMode::Expand, "Expand" },
                      { ScreenMatchMode::Shrink, "Shrink" }
                      })

    /**
     * Scales a screen-space overlay canvas with the window (issue #445, Unity's CanvasScaler), so a UI laid out once for a
     * reference resolution keeps its proportions at any size. Put it on the object with the root Canvas.
     *
     * With a scale s, the canvas's rect is the viewport divided by s, in canvas units, and everything under it is laid out
     * in those units and drawn (and hit) s times as large: the canvas's RectTransform::GetLocalToCanvas, and every
     * element's, includes the scale, so GetRect is in canvas units while pointer positions and UI hit tests stay in window
     * coordinates. Text is drawn from its distance field, so it stays sharp.
     *
     * ScaleWithScreenSize: with w = viewport width / referenceResolution.x and h likewise for the height, s is
     * 2^(lerp(log2 w, log2 h, matchWidthOrHeight)) for MatchWidthOrHeight (0 the width's ratio, 1 the height's, in between
     * a geometric mix, as Unity's), min(w, h) for Expand and max(w, h) for Shrink. ConstantPixelSize: s = scaleFactor.
     * A reference resolution, scale factor or result that is not a positive finite number gives 1.
     *
     * Ignored on a world-space canvas (its object's scale is the size of a unit) and on any object without the root
     * canvas. A disabled scaler scales nothing. One per object.
     */
    class CanvasScaler final : public SerializableComponent
    {
    public:
        explicit CanvasScaler(GameObject &gameObject);

        [[nodiscard]] std::string GetTypeName() const override { return "CanvasScaler"; }

        [[nodiscard]] CanvasScaleMode GetUIScaleMode() const { return _uiScaleMode; }
        void SetUIScaleMode(const CanvasScaleMode mode) { _uiScaleMode = mode; }
        /// Pixels per canvas unit with ConstantPixelSize
        [[nodiscard]] float GetScaleFactor() const { return _scaleFactor; }
        void SetScaleFactor(const float factor) { _scaleFactor = factor; }
        [[nodiscard]] const Math::Vector2 &GetReferenceResolution() const { return _referenceResolution; }
        void SetReferenceResolution(const Math::Vector2 &resolution) { _referenceResolution = resolution; }
        [[nodiscard]] ScreenMatchMode GetScreenMatchMode() const { return _screenMatchMode; }
        void SetScreenMatchMode(const ScreenMatchMode mode) { _screenMatchMode = mode; }
        /// 0 follows the width, 1 the height (clamped to 0..1)
        [[nodiscard]] float GetMatchWidthOrHeight() const { return _matchWidthOrHeight; }
        void SetMatchWidthOrHeight(const float match) { _matchWidthOrHeight = match; }

        /// The scale these settings give for a viewport (pixels per canvas unit); 1 for an empty viewport
        [[nodiscard]] float ComputeScale(const Vector2i &viewport) const;
        /// The scale the last layout used (1 before any)
        [[nodiscard]] float GetAppliedScale() const { return _appliedScale; }
        /// Called by the layout
        void SetAppliedScale(const float scale) { _appliedScale = scale; }

        static constexpr bool IsSingleton = true;

    private:
        CanvasScaleMode _uiScaleMode = CanvasScaleMode::ScaleWithScreenSize;
        float _scaleFactor = 1.0f;
        Math::Vector2 _referenceResolution{800.0f, 600.0f};
        ScreenMatchMode _screenMatchMode = ScreenMatchMode::MatchWidthOrHeight;
        float _matchWidthOrHeight = 0.0f;
        float _appliedScale = 1.0f;
    };
}
