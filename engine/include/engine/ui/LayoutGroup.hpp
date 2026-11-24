#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <math/Vector2.hpp>

#include "engine/serialization/ComponentSerializer.hpp"
#include "engine/serialization/FieldInfo.hpp"
#include "engine/ui/Rect.hpp"

namespace N2Engine::UI
{
    class RectTransform;

    /// Where a layout group puts its children inside its padded rect when they don't fill it (Unity's TextAnchor)
    enum class ChildAlignment : std::uint8_t
    {
        UpperLeft,
        UpperCenter,
        UpperRight,
        MiddleLeft,
        MiddleCenter,
        MiddleRight,
        LowerLeft,
        LowerCenter,
        LowerRight
    };

    N2_SERIALIZE_ENUM(ChildAlignment, {
                      { ChildAlignment::UpperLeft, "UpperLeft" },
                      { ChildAlignment::UpperCenter, "UpperCenter" },
                      { ChildAlignment::UpperRight, "UpperRight" },
                      { ChildAlignment::MiddleLeft, "MiddleLeft" },
                      { ChildAlignment::MiddleCenter, "MiddleCenter" },
                      { ChildAlignment::MiddleRight, "MiddleRight" },
                      { ChildAlignment::LowerLeft, "LowerLeft" },
                      { ChildAlignment::LowerCenter, "LowerCenter" },
                      { ChildAlignment::LowerRight, "LowerRight" }
                      })

    /// The alignment's share of the free space to the left (x: 0 left, 0.5 centre, 1 right) and above (y: 0 upper, 0.5
    /// middle, 1 lower)
    [[nodiscard]] Math::Vector2 AlignmentFraction(ChildAlignment alignment);

    /// Space kept free inside a layout group's rect, in canvas units
    struct LayoutPadding
    {
        float left = 0.0f;
        float right = 0.0f;
        float top = 0.0f;
        float bottom = 0.0f;
        friend constexpr bool operator==(const LayoutPadding &, const LayoutPadding &) = default;
    };

    /**
     * Overrides the size a layout group or a ContentSizeFitter uses for its object (a part of Unity's LayoutElement), issue
     * #445. A preferred width or height below 0 (the default -1) is not set: the object's own size counts.
     * ignoreLayout leaves the object out of its parent's layout group altogether (it keeps its own RectTransform).
     */
    class LayoutElement final : public SerializableComponent
    {
    public:
        explicit LayoutElement(GameObject &gameObject);
        [[nodiscard]] std::string GetTypeName() const override { return "LayoutElement"; }

        [[nodiscard]] float GetPreferredWidth() const { return _preferredWidth; }
        void SetPreferredWidth(float width) { _preferredWidth = width; }
        [[nodiscard]] float GetPreferredHeight() const { return _preferredHeight; }
        void SetPreferredHeight(float height) { _preferredHeight = height; }
        [[nodiscard]] bool GetIgnoreLayout() const { return _ignoreLayout; }
        void SetIgnoreLayout(bool ignore) { _ignoreLayout = ignore; }

        static constexpr bool IsSingleton = true;

    private:
        float _preferredWidth = -1.0f;
        float _preferredHeight = -1.0f;
        bool _ignoreLayout = false;
    };

    /**
     * The base of the layout groups (Unity's LayoutGroup), issue #445: it places its object's children inside its rect.
     * The children it lays out are the active ones with a RectTransform, in order, except those with a LayoutElement whose
     * ignoreLayout is set. It drives their RectTransforms: anchors and pivot-relative position from its rect's top-left
     * corner (anchorMin = anchorMax = (0, 1)) and their size; their pivots are kept. Values are written only when they
     * change.
     *
     * When it runs: in the UI layout (UISystem's LayOut), right after its own rect is resolved and before its children's,
     * so the layout groups are applied in the same pass that the UI pass and the hit test lay out with (once per UI pass
     * and once per hit test, from the canvas down).
     */
    class LayoutGroup : public SerializableComponent
    {
    public:
        [[nodiscard]] const LayoutPadding &GetPadding() const { return _padding; }
        void SetPadding(const LayoutPadding &padding) { _padding = padding; }
        [[nodiscard]] ChildAlignment GetChildAlignment() const { return _childAlignment; }
        void SetChildAlignment(ChildAlignment alignment) { _childAlignment = alignment; }

        /// Places the children for this object's rect `rect` (canvas space)
        void Arrange(const Rect &rect) const;
        /// The children's rects (canvas space) inside `rect` for these child sizes, in LayoutChildren order (pure: changes nothing)
        [[nodiscard]] virtual std::vector<Rect> ComputeRects(const Rect &rect) const = 0;
        /// The size the children need, padding included: what a ContentSizeFitter on this object sets. `currentRect` is the
        /// object's rect now (a flexible grid's column count depends on its width)
        [[nodiscard]] virtual Math::Vector2 GetPreferredSize(const Rect &currentRect) const = 0;
        /// The children the group lays out (see the class comment)
        [[nodiscard]] std::vector<RectTransform *> LayoutChildren() const;

        /// Sets a child's RectTransform so it resolves to `childRect` inside `parentRect` (anchors at the parent's top-left)
        static void Place(RectTransform &child, const Rect &childRect, const Rect &parentRect);

        /// One per object (also across the kinds: give an object one group)
        static constexpr bool IsSingleton = true;

    protected:
        explicit LayoutGroup(GameObject &gameObject);
        /// The rect less the padding
        [[nodiscard]] Rect Inner(const Rect &rect) const;

        LayoutPadding _padding;
        ChildAlignment _childAlignment = ChildAlignment::UpperLeft;
    };

    /**
     * Lays the children out in a row (HorizontalLayoutGroup) or a column (VerticalLayoutGroup) with `spacing` between
     * them (Unity's HorizontalOrVerticalLayoutGroup), issue #445.
     *
     * Each child's size on an axis is, with controlChildWidth/Height off, its own (RectTransform size) and, with it on,
     * its preferred size (LayoutUtility::GetPreferredSize). With control and childForceExpand on: along the row the free
     * space is shared out equally between the children; across it each child fills the padded rect. The children's block
     * is placed by childAlignment, starting from the left (top) edge.
     */
    class HorizontalOrVerticalLayoutGroup : public LayoutGroup
    {
    public:
        [[nodiscard]] float GetSpacing() const { return _spacing; }
        void SetSpacing(float spacing) { _spacing = spacing; }
        [[nodiscard]] bool GetControlChildWidth() const { return _controlChildWidth; }
        void SetControlChildWidth(bool control) { _controlChildWidth = control; }
        [[nodiscard]] bool GetControlChildHeight() const { return _controlChildHeight; }
        void SetControlChildHeight(bool control) { _controlChildHeight = control; }
        [[nodiscard]] bool GetChildForceExpandWidth() const { return _childForceExpandWidth; }
        void SetChildForceExpandWidth(bool expand) { _childForceExpandWidth = expand; }
        [[nodiscard]] bool GetChildForceExpandHeight() const { return _childForceExpandHeight; }
        void SetChildForceExpandHeight(bool expand) { _childForceExpandHeight = expand; }
        [[nodiscard]] bool IsVertical() const { return _vertical; }

        [[nodiscard]] std::vector<Rect> ComputeRects(const Rect &rect) const override;
        [[nodiscard]] Math::Vector2 GetPreferredSize(const Rect &currentRect) const override;

    protected:
        HorizontalOrVerticalLayoutGroup(GameObject &gameObject, bool vertical);

    private:
        bool _vertical;
        float _spacing = 0.0f;
        bool _controlChildWidth = false;
        bool _controlChildHeight = false;
        bool _childForceExpandWidth = false;
        bool _childForceExpandHeight = false;

        /// Each child's size before any expansion
        [[nodiscard]] std::vector<Math::Vector2> ChildSizes(const std::vector<RectTransform *> &children) const;
    };

    class HorizontalLayoutGroup final : public HorizontalOrVerticalLayoutGroup
    {
    public:
        explicit HorizontalLayoutGroup(GameObject &gameObject) : HorizontalOrVerticalLayoutGroup(gameObject, false) {}
        [[nodiscard]] std::string GetTypeName() const override { return "HorizontalLayoutGroup"; }
    };

    class VerticalLayoutGroup final : public HorizontalOrVerticalLayoutGroup
    {
    public:
        explicit VerticalLayoutGroup(GameObject &gameObject) : HorizontalOrVerticalLayoutGroup(gameObject, true) {}
        [[nodiscard]] std::string GetTypeName() const override { return "VerticalLayoutGroup"; }
    };

    /// How a grid decides its column count (Unity's GridLayoutGroup.Constraint)
    enum class GridConstraint : std::uint8_t
    {
        /// As many columns as fit in the padded width (at least one)
        Flexible,
        /// constraintCount columns
        FixedColumnCount,
        /// constraintCount rows: as many columns as that needs
        FixedRowCount
    };

    N2_SERIALIZE_ENUM(GridConstraint, {
                      { GridConstraint::Flexible, "Flexible" },
                      { GridConstraint::FixedColumnCount, "FixedColumnCount" },
                      { GridConstraint::FixedRowCount, "FixedRowCount" }
                      })

    /**
     * Lays the children out in a grid of cellSize cells with `spacing` between them, row by row from the top-left (Unity's
     * GridLayoutGroup with its default start corner and axis), issue #445. Every child gets the cell's size. The block of
     * cells is placed by childAlignment.
     */
    class GridLayoutGroup final : public LayoutGroup
    {
    public:
        explicit GridLayoutGroup(GameObject &gameObject);
        [[nodiscard]] std::string GetTypeName() const override { return "GridLayoutGroup"; }

        [[nodiscard]] const Math::Vector2 &GetCellSize() const { return _cellSize; }
        void SetCellSize(const Math::Vector2 &size) { _cellSize = size; }
        [[nodiscard]] const Math::Vector2 &GetSpacing() const { return _spacing; }
        void SetSpacing(const Math::Vector2 &spacing) { _spacing = spacing; }
        [[nodiscard]] GridConstraint GetConstraint() const { return _constraint; }
        void SetConstraint(GridConstraint constraint) { _constraint = constraint; }
        /// The column (FixedColumnCount) or row (FixedRowCount) count; at least 1
        [[nodiscard]] int GetConstraintCount() const { return _constraintCount; }
        void SetConstraintCount(int count) { _constraintCount = count; }

        /// The column count for `childCount` children in a padded rect `innerWidth` wide
        [[nodiscard]] int ColumnCount(std::size_t childCount, float innerWidth) const;

        [[nodiscard]] std::vector<Rect> ComputeRects(const Rect &rect) const override;
        [[nodiscard]] Math::Vector2 GetPreferredSize(const Rect &currentRect) const override;

    private:
        Math::Vector2 _cellSize{100.0f, 100.0f};
        Math::Vector2 _spacing{0.0f, 0.0f};
        GridConstraint _constraint = GridConstraint::Flexible;
        int _constraintCount = 2;
    };

    /// What a ContentSizeFitter does on one axis (Unity's FitMode, without MinSize)
    enum class FitMode : std::uint8_t
    {
        /// Leaves the size alone
        Unconstrained,
        /// Sets it to the content's preferred size
        PreferredSize
    };

    N2_SERIALIZE_ENUM(FitMode, {
                      { FitMode::Unconstrained, "Unconstrained" },
                      { FitMode::PreferredSize, "PreferredSize" }
                      })

    /**
     * Sizes its object to its content (Unity's ContentSizeFitter), issue #445: on each axis set to PreferredSize, the
     * object's RectTransform size becomes the preferred size of what is on it (its layout group's, else its UIText's; see
     * LayoutUtility::GetContentPreferredSize), keeping the anchors (with stretched anchors the sizeDelta is the preferred
     * size less the anchor box). The pivot stays put, so a scroll view's content pinned at its top grows downwards with its
     * children. Applied in the UI layout just before its object's rect is resolved.
     */
    class ContentSizeFitter final : public SerializableComponent
    {
    public:
        explicit ContentSizeFitter(GameObject &gameObject);
        [[nodiscard]] std::string GetTypeName() const override { return "ContentSizeFitter"; }

        [[nodiscard]] FitMode GetHorizontalFit() const { return _horizontalFit; }
        void SetHorizontalFit(FitMode mode) { _horizontalFit = mode; }
        [[nodiscard]] FitMode GetVerticalFit() const { return _verticalFit; }
        void SetVerticalFit(FitMode mode) { _verticalFit = mode; }

        /// Sets the object's size for its parent's rect `parentRect` (the layout calls it)
        void Fit(const Rect &parentRect) const;

        static constexpr bool IsSingleton = true;

    private:
        FitMode _horizontalFit = FitMode::Unconstrained;
        FitMode _verticalFit = FitMode::PreferredSize;
    };

    /// The sizes the layout works with (issue #445)
    namespace LayoutUtility
    {
        /// The object's own size: per axis its sizeDelta when its anchors meet on that axis, else its last resolved rect's
        [[nodiscard]] Math::Vector2 GetCurrentSize(const RectTransform &rectTransform);
        /// The preferred size of what is on the object: its active layout group's (GetPreferredSize), else its active
        /// UIText's laid-out size (the width unwrapped, the height wrapped at its current width when wrap is on); nullopt
        /// for neither
        [[nodiscard]] std::optional<Math::Vector2> GetContentPreferredSize(const GameObject &gameObject);
        /// The size a layout group gives the object when it controls its size: per axis an active LayoutElement's preferred
        /// size when set, else GetContentPreferredSize, else GetCurrentSize
        [[nodiscard]] Math::Vector2 GetPreferredSize(const RectTransform &rectTransform);
    }
}
