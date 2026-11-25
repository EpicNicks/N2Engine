#include "engine/ui/LayoutGroup.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

#include "engine/GameObjectScene.hpp"
#include "engine/ui/RectTransform.hpp"
#include "engine/ui/UIText.hpp"

namespace N2Engine::UI
{
    namespace
    {
        bool Same(const Math::Vector2 &a, const Math::Vector2 &b) { return a.x == b.x && a.y == b.y; }

        float NonNegative(const float value) { return std::isfinite(value) && value > 0.0f ? value : 0.0f; }

        /// The active layout group on an object, or nullptr
        const LayoutGroup *ActiveGroupOn(const GameObject &gameObject)
        {
            for (const LayoutGroup *group : gameObject.GetComponents<LayoutGroup>())
            {
                if (group && group->IsActive())
                {
                    return group;
                }
            }
            return nullptr;
        }
    }

    Math::Vector2 AlignmentFraction(const ChildAlignment alignment)
    {
        const auto index = static_cast<int>(alignment);
        return Math::Vector2{static_cast<float>(index % 3) * 0.5f, static_cast<float>(index / 3) * 0.5f};
    }

    // ===== LayoutElement =====

    LayoutElement::LayoutElement(GameObject &gameObject) : SerializableComponent(gameObject)
    {
        RegisterMember("preferredWidth", _preferredWidth).Tooltip("Below 0: not set (the object's own width counts)");
        RegisterMember("preferredHeight", _preferredHeight).Tooltip("Below 0: not set (the object's own height counts)");
        RegisterMember("ignoreLayout", _ignoreLayout).Tooltip("Left out of its parent's layout group");
    }

    // ===== LayoutUtility =====

    Math::Vector2 LayoutUtility::GetCurrentSize(const RectTransform &rectTransform)
    {
        const Math::Vector2 &anchorMin = rectTransform.GetAnchorMin();
        const Math::Vector2 &anchorMax = rectTransform.GetAnchorMax();
        const Math::Vector2 &sizeDelta = rectTransform.GetSizeDelta();
        const Rect &rect = rectTransform.GetRect();
        return Math::Vector2{anchorMin.x == anchorMax.x ? sizeDelta.x : rect.width,
                             anchorMin.y == anchorMax.y ? sizeDelta.y : rect.height};
    }

    std::optional<Math::Vector2> LayoutUtility::GetContentPreferredSize(const GameObject &gameObject)
    {
        const RectTransform *rectTransform = gameObject.GetComponent<RectTransform>();
        const Rect current = rectTransform ? rectTransform->GetRect() : Rect{};
        if (const LayoutGroup *group = ActiveGroupOn(gameObject))
        {
            return group->GetPreferredSize(current);
        }
        for (const UIText *text : gameObject.GetComponents<UIText>())
        {
            if (text && text->IsActive())
            {
                // The width unwrapped; the height wrapped at the current width (as the text will draw). Laid out here,
                // not through the text's own cache, so the draw's cached layout is left alone
                const std::shared_ptr<Text::Font> font = text->GetEffectiveFont();
                if (!font)
                {
                    return Math::Vector2{0.0f, 0.0f};
                }
                const Text::Rect natural = text->Measure(0.0f).bounds;
                const float wrapWidth = text->GetWrap() && current.width > 0.0f ? current.width : 0.0f;
                const float height = wrapWidth > 0.0f && natural.Width() > wrapWidth
                                         ? text->Measure(wrapWidth).bounds.Height()
                                         : natural.Height();
                return Math::Vector2{natural.Width(), height};
            }
        }
        return std::nullopt;
    }

    Math::Vector2 LayoutUtility::GetPreferredSize(const RectTransform &rectTransform)
    {
        const GameObject &gameObject = rectTransform.GetGameObject();
        Math::Vector2 size = GetCurrentSize(rectTransform);
        if (const std::optional<Math::Vector2> content = GetContentPreferredSize(gameObject))
        {
            size = *content;
        }
        if (const LayoutElement *element = gameObject.GetComponent<LayoutElement>(); element && element->IsActive())
        {
            if (element->GetPreferredWidth() >= 0.0f)
            {
                size.x = element->GetPreferredWidth();
            }
            if (element->GetPreferredHeight() >= 0.0f)
            {
                size.y = element->GetPreferredHeight();
            }
        }
        return size;
    }

    // ===== LayoutGroup =====

    LayoutGroup::LayoutGroup(GameObject &gameObject) : SerializableComponent(gameObject)
    {
        RegisterMember("paddingLeft", _padding.left);
        RegisterMember("paddingRight", _padding.right);
        RegisterMember("paddingTop", _padding.top);
        RegisterMember("paddingBottom", _padding.bottom);
        RegisterMember("childAlignment", _childAlignment);
    }

    Rect LayoutGroup::Inner(const Rect &rect) const
    {
        return Rect{rect.x + _padding.left, rect.y + _padding.bottom,
                    rect.width - _padding.left - _padding.right, rect.height - _padding.top - _padding.bottom};
    }

    std::vector<RectTransform *> LayoutGroup::LayoutChildren() const
    {
        std::vector<RectTransform *> children;
        for (const auto &child : GetGameObject().GetChildren())
        {
            if (!child || !child->IsActiveInHierarchy())
            {
                continue;
            }
            RectTransform *rectTransform = child->GetComponent<RectTransform>();
            if (!rectTransform)
            {
                continue;
            }
            if (const LayoutElement *element = child->GetComponent<LayoutElement>();
                element && element->IsActive() && element->GetIgnoreLayout())
            {
                continue;
            }
            children.push_back(rectTransform);
        }
        return children;
    }

    void LayoutGroup::Place(RectTransform &child, const Rect &childRect, const Rect &parentRect)
    {
        const Math::Vector2 corner{0.0f, 1.0f};
        const Math::Vector2 &pivot = child.GetPivot();
        const Math::Vector2 size{childRect.width, childRect.height};
        const Math::Vector2 position{childRect.x + pivot.x * childRect.width - parentRect.x,
                                     childRect.y + pivot.y * childRect.height - parentRect.YMax()};
        // Written only on a change, so an idle layout leaves the values (and an inspector showing them) alone
        if (!Same(child.GetAnchorMin(), corner))
        {
            child.SetAnchorMin(corner);
        }
        if (!Same(child.GetAnchorMax(), corner))
        {
            child.SetAnchorMax(corner);
        }
        if (!Same(child.GetSizeDelta(), size))
        {
            child.SetSizeDelta(size);
        }
        if (!Same(child.GetAnchoredPosition(), position))
        {
            child.SetAnchoredPosition(position);
        }
    }

    void LayoutGroup::Arrange(const Rect &rect) const
    {
        const std::vector<RectTransform *> children = LayoutChildren();
        if (children.empty())
        {
            return;
        }
        const std::vector<Rect> rects = ComputeRects(rect);
        for (std::size_t i = 0; i < children.size() && i < rects.size(); ++i)
        {
            Place(*children[i], rects[i], rect);
        }
    }

    // ===== Horizontal and vertical =====

    HorizontalOrVerticalLayoutGroup::HorizontalOrVerticalLayoutGroup(GameObject &gameObject, const bool vertical)
        : LayoutGroup(gameObject), _vertical(vertical)
    {
        RegisterMember("spacing", _spacing);
        RegisterMember("controlChildWidth", _controlChildWidth);
        RegisterMember("controlChildHeight", _controlChildHeight);
        RegisterMember("childForceExpandWidth", _childForceExpandWidth);
        RegisterMember("childForceExpandHeight", _childForceExpandHeight);
    }

    std::vector<Math::Vector2> HorizontalOrVerticalLayoutGroup::ChildSizes(const std::vector<RectTransform *> &children) const
    {
        std::vector<Math::Vector2> sizes;
        sizes.reserve(children.size());
        for (const RectTransform *child : children)
        {
            const Math::Vector2 current = LayoutUtility::GetCurrentSize(*child);
            const bool controlled = _controlChildWidth || _controlChildHeight;
            const Math::Vector2 preferred = controlled ? LayoutUtility::GetPreferredSize(*child) : current;
            sizes.push_back(Math::Vector2{NonNegative(_controlChildWidth ? preferred.x : current.x),
                                          NonNegative(_controlChildHeight ? preferred.y : current.y)});
        }
        return sizes;
    }

    std::vector<Rect> HorizontalOrVerticalLayoutGroup::ComputeRects(const Rect &rect) const
    {
        const std::vector<RectTransform *> children = LayoutChildren();
        std::vector<Math::Vector2> sizes = ChildSizes(children);
        std::vector<Rect> rects;
        if (sizes.empty())
        {
            return rects;
        }
        const Rect inner = Inner(rect);
        const Math::Vector2 fraction = AlignmentFraction(_childAlignment);
        const float innerMain = _vertical ? inner.height : inner.width;
        const float innerCross = _vertical ? inner.width : inner.height;
        const bool controlMain = _vertical ? _controlChildHeight : _controlChildWidth;
        const bool controlCross = _vertical ? _controlChildWidth : _controlChildHeight;
        const bool expandMain = controlMain && (_vertical ? _childForceExpandHeight : _childForceExpandWidth);
        const bool expandCross = controlCross && (_vertical ? _childForceExpandWidth : _childForceExpandHeight);

        float total = _spacing * static_cast<float>(sizes.size() - 1);
        for (const Math::Vector2 &size : sizes)
        {
            total += _vertical ? size.y : size.x;
        }
        // Force expand: the free space shared out equally along the row
        if (expandMain && total < innerMain)
        {
            const float extra = (innerMain - total) / static_cast<float>(sizes.size());
            for (Math::Vector2 &size : sizes)
            {
                (_vertical ? size.y : size.x) += extra;
            }
            total = innerMain;
        }

        // Along the row from the left (down the column from the top); the block placed by the alignment
        float cursor = (innerMain - total) * (_vertical ? fraction.y : fraction.x);
        rects.reserve(sizes.size());
        for (Math::Vector2 size : sizes)
        {
            float &cross = _vertical ? size.x : size.y;
            if (expandCross)
            {
                cross = NonNegative(innerCross);
            }
            const float crossOffset = (innerCross - cross) * (_vertical ? fraction.x : fraction.y);
            if (_vertical)
            {
                const float top = inner.YMax() - cursor;
                rects.push_back(Rect{inner.x + crossOffset, top - size.y, size.x, size.y});
                cursor += size.y + _spacing;
            }
            else
            {
                const float top = inner.YMax() - crossOffset;
                rects.push_back(Rect{inner.x + cursor, top - size.y, size.x, size.y});
                cursor += size.x + _spacing;
            }
        }
        return rects;
    }

    Math::Vector2 HorizontalOrVerticalLayoutGroup::GetPreferredSize(const Rect &) const
    {
        const std::vector<Math::Vector2> sizes = ChildSizes(LayoutChildren());
        float main = sizes.empty() ? 0.0f : _spacing * static_cast<float>(sizes.size() - 1);
        float cross = 0.0f;
        for (const Math::Vector2 &size : sizes)
        {
            main += _vertical ? size.y : size.x;
            cross = std::max(cross, _vertical ? size.x : size.y);
        }
        const float horizontalPadding = _padding.left + _padding.right;
        const float verticalPadding = _padding.top + _padding.bottom;
        return _vertical ? Math::Vector2{cross + horizontalPadding, main + verticalPadding}
                         : Math::Vector2{main + horizontalPadding, cross + verticalPadding};
    }

    // ===== Grid =====

    GridLayoutGroup::GridLayoutGroup(GameObject &gameObject) : LayoutGroup(gameObject)
    {
        RegisterMember("cellSize", _cellSize);
        RegisterMember("spacing", _spacing);
        RegisterMember("constraint", _constraint);
        RegisterMember("constraintCount", _constraintCount).Range(1.0, 10000.0).VisibleWhen("constraint", {"FixedColumnCount", "FixedRowCount"});
    }

    int GridLayoutGroup::ColumnCount(const std::size_t childCount, const float innerWidth) const
    {
        const int count = std::max(_constraintCount, 1);
        switch (_constraint)
        {
        case GridConstraint::FixedColumnCount:
            return count;
        case GridConstraint::FixedRowCount:
            return std::max(1, static_cast<int>((childCount + static_cast<std::size_t>(count) - 1) / static_cast<std::size_t>(count)));
        case GridConstraint::Flexible:
            break;
        }
        const float step = _cellSize.x + _spacing.x;
        if (!(step > 0.0f) || !std::isfinite(innerWidth))
        {
            return 1;
        }
        const float fit = std::floor((innerWidth + _spacing.x) / step);
        return std::clamp(static_cast<int>(std::max(fit, 1.0f)), 1, std::max(1, static_cast<int>(childCount)));
    }

    std::vector<Rect> GridLayoutGroup::ComputeRects(const Rect &rect) const
    {
        const std::size_t count = LayoutChildren().size();
        std::vector<Rect> rects;
        if (count == 0)
        {
            return rects;
        }
        const Rect inner = Inner(rect);
        const int columns = ColumnCount(count, inner.width);
        const int rows = static_cast<int>((count + static_cast<std::size_t>(columns) - 1) / static_cast<std::size_t>(columns));
        const int usedColumns = std::min(columns, static_cast<int>(count));
        const float blockWidth = static_cast<float>(usedColumns) * _cellSize.x + static_cast<float>(usedColumns - 1) * _spacing.x;
        const float blockHeight = static_cast<float>(rows) * _cellSize.y + static_cast<float>(rows - 1) * _spacing.y;
        const Math::Vector2 fraction = AlignmentFraction(_childAlignment);
        const float left = inner.x + (inner.width - blockWidth) * fraction.x;
        const float top = inner.YMax() - (inner.height - blockHeight) * fraction.y;
        rects.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto column = static_cast<float>(i % static_cast<std::size_t>(columns));
            const auto row = static_cast<float>(i / static_cast<std::size_t>(columns));
            const float cellTop = top - row * (_cellSize.y + _spacing.y);
            rects.push_back(Rect{left + column * (_cellSize.x + _spacing.x), cellTop - _cellSize.y, _cellSize.x, _cellSize.y});
        }
        return rects;
    }

    Math::Vector2 GridLayoutGroup::GetPreferredSize(const Rect &currentRect) const
    {
        const std::size_t count = LayoutChildren().size();
        const float horizontalPadding = _padding.left + _padding.right;
        const float verticalPadding = _padding.top + _padding.bottom;
        if (count == 0)
        {
            return Math::Vector2{horizontalPadding, verticalPadding};
        }
        const int columns = std::min(ColumnCount(count, currentRect.width - horizontalPadding), static_cast<int>(count));
        const int rows = static_cast<int>((count + static_cast<std::size_t>(columns) - 1) / static_cast<std::size_t>(columns));
        return Math::Vector2{horizontalPadding + static_cast<float>(columns) * _cellSize.x + static_cast<float>(columns - 1) * _spacing.x,
                             verticalPadding + static_cast<float>(rows) * _cellSize.y + static_cast<float>(rows - 1) * _spacing.y};
    }

    // ===== ContentSizeFitter =====

    ContentSizeFitter::ContentSizeFitter(GameObject &gameObject) : SerializableComponent(gameObject)
    {
        RegisterMember("horizontalFit", _horizontalFit);
        RegisterMember("verticalFit", _verticalFit);
    }

    void ContentSizeFitter::Fit(const Rect &parentRect) const
    {
        if (_horizontalFit == FitMode::Unconstrained && _verticalFit == FitMode::Unconstrained)
        {
            return;
        }
        RectTransform *rectTransform = GetGameObject().GetComponent<RectTransform>();
        if (!rectTransform)
        {
            return;
        }
        const std::optional<Math::Vector2> preferred = LayoutUtility::GetContentPreferredSize(GetGameObject());
        if (!preferred)
        {
            return;
        }
        // The sizeDelta that makes the rect that size with the current anchors
        const Math::Vector2 &anchorMin = rectTransform->GetAnchorMin();
        const Math::Vector2 &anchorMax = rectTransform->GetAnchorMax();
        Math::Vector2 sizeDelta = rectTransform->GetSizeDelta();
        if (_horizontalFit == FitMode::PreferredSize && std::isfinite(preferred->x))
        {
            sizeDelta.x = preferred->x - parentRect.width * (anchorMax.x - anchorMin.x);
        }
        if (_verticalFit == FitMode::PreferredSize && std::isfinite(preferred->y))
        {
            sizeDelta.y = preferred->y - parentRect.height * (anchorMax.y - anchorMin.y);
        }
        if (!Same(sizeDelta, rectTransform->GetSizeDelta()))
        {
            rectTransform->SetSizeDelta(sizeDelta);
        }
    }
}
