#include "engine/ui/RectTransform.hpp"

namespace N2Engine::UI
{
    RectTransform::RectTransform(GameObject &gameObject) : SerializableComponent(gameObject)
    {
        RegisterMember("anchorMin", _anchorMin);
        RegisterMember("anchorMax", _anchorMax);
        RegisterMember("pivot", _pivot);
        RegisterMember("anchoredPosition", _anchoredPosition);
        RegisterMember("sizeDelta", _sizeDelta);
    }

    Math::Vector2 RectTransform::GetOffsetMin() const
    {
        return Math::Vector2{_anchoredPosition.x - _sizeDelta.x * _pivot.x,
                             _anchoredPosition.y - _sizeDelta.y * _pivot.y};
    }

    void RectTransform::SetOffsetMin(const Math::Vector2 &value)
    {
        // Unity's formula: the corner moves by `offset`, the size shrinks by it, and the pivot follows its share
        const Math::Vector2 current = GetOffsetMin();
        const float dx = value.x - current.x;
        const float dy = value.y - current.y;
        _sizeDelta = Math::Vector2{_sizeDelta.x - dx, _sizeDelta.y - dy};
        _anchoredPosition = Math::Vector2{_anchoredPosition.x + dx * (1.0f - _pivot.x),
                                          _anchoredPosition.y + dy * (1.0f - _pivot.y)};
    }

    Math::Vector2 RectTransform::GetOffsetMax() const
    {
        return Math::Vector2{_anchoredPosition.x + _sizeDelta.x * (1.0f - _pivot.x),
                             _anchoredPosition.y + _sizeDelta.y * (1.0f - _pivot.y)};
    }

    void RectTransform::SetOffsetMax(const Math::Vector2 &value)
    {
        const Math::Vector2 current = GetOffsetMax();
        const float dx = value.x - current.x;
        const float dy = value.y - current.y;
        _sizeDelta = Math::Vector2{_sizeDelta.x + dx, _sizeDelta.y + dy};
        _anchoredPosition = Math::Vector2{_anchoredPosition.x + dx * _pivot.x,
                                          _anchoredPosition.y + dy * _pivot.y};
    }

    void RectTransform::StretchToParent()
    {
        _anchorMin = Math::Vector2{0.0f, 0.0f};
        _anchorMax = Math::Vector2{1.0f, 1.0f};
        _anchoredPosition = Math::Vector2{0.0f, 0.0f};
        _sizeDelta = Math::Vector2{0.0f, 0.0f};
    }

    Rect RectTransform::ResolveIn(const Rect &parent) const
    {
        return Resolve(_anchorMin, _anchorMax, _pivot, _anchoredPosition, _sizeDelta, parent);
    }

    Rect RectTransform::Resolve(const Math::Vector2 &anchorMin, const Math::Vector2 &anchorMax,
                                const Math::Vector2 &pivot, const Math::Vector2 &anchoredPosition,
                                const Math::Vector2 &sizeDelta, const Rect &parent)
    {
        // The anchor box, in canvas space
        const float boxMinX = parent.x + anchorMin.x * parent.width;
        const float boxMinY = parent.y + anchorMin.y * parent.height;
        const float boxWidth = (anchorMax.x - anchorMin.x) * parent.width;
        const float boxHeight = (anchorMax.y - anchorMin.y) * parent.height;

        Rect rect;
        rect.width = boxWidth + sizeDelta.x;
        rect.height = boxHeight + sizeDelta.y;

        // The pivot sits at the reference point (the point at `pivot` inside the anchor box) plus anchoredPosition
        const float pivotX = boxMinX + pivot.x * boxWidth + anchoredPosition.x;
        const float pivotY = boxMinY + pivot.y * boxHeight + anchoredPosition.y;
        rect.x = pivotX - pivot.x * rect.width;
        rect.y = pivotY - pivot.y * rect.height;
        return rect;
    }
}
