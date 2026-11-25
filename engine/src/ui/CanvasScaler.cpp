#include "engine/ui/CanvasScaler.hpp"

#include <algorithm>
#include <cmath>

#include <math/DeterministicMath.hpp>

namespace N2Engine::UI
{
    CanvasScaler::CanvasScaler(GameObject &gameObject) : SerializableComponent(gameObject)
    {
        RegisterMember("uiScaleMode", _uiScaleMode);
        RegisterMember("scaleFactor", _scaleFactor).Tooltip("Pixels per canvas unit, with ConstantPixelSize");
        RegisterMember("referenceResolution", _referenceResolution)
            .Tooltip("The size the UI is laid out for, with ScaleWithScreenSize");
        RegisterMember("screenMatchMode", _screenMatchMode);
        RegisterMember("matchWidthOrHeight", _matchWidthOrHeight)
            .Range(0.0, 1.0)
            .Tooltip("0 follows the width, 1 the height");
    }

    float CanvasScaler::ComputeScale(const Vector2i &viewport) const
    {
        const auto usable = [](const float value) { return std::isfinite(value) && value > 0.0f; };
        if (viewport[0] <= 0 || viewport[1] <= 0)
        {
            return 1.0f;
        }
        float scale = 1.0f;
        if (_uiScaleMode == CanvasScaleMode::ConstantPixelSize)
        {
            scale = _scaleFactor;
        }
        else
        {
            if (!usable(_referenceResolution.x) || !usable(_referenceResolution.y))
            {
                return 1.0f;
            }
            const float width = static_cast<float>(viewport[0]) / _referenceResolution.x;
            const float height = static_cast<float>(viewport[1]) / _referenceResolution.y;
            switch (_screenMatchMode)
            {
            case ScreenMatchMode::Expand:
                scale = std::min(width, height);
                break;
            case ScreenMatchMode::Shrink:
                scale = std::max(width, height);
                break;
            case ScreenMatchMode::MatchWidthOrHeight:
            {
                const float match = std::isfinite(_matchWidthOrHeight) ? std::clamp(_matchWidthOrHeight, 0.0f, 1.0f) : 0.0f;
                // The ends, and equal ratios, exactly
                if (match <= 0.0f || width == height)
                {
                    scale = width;
                }
                else if (match >= 1.0f)
                {
                    scale = height;
                }
                else
                {
                    const double low = Math::Log2(static_cast<double>(width));
                    const double high = Math::Log2(static_cast<double>(height));
                    constexpr double Ln2 = 0.69314718055994530942;
                    scale = static_cast<float>(Math::Exp((low + (high - low) * static_cast<double>(match)) * Ln2));
                }
                break;
            }
            }
        }
        return usable(scale) ? scale : 1.0f;
    }
}
