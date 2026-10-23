Accepted by the user 2026-10-13:
- Alt+left press is already orbit, so the Alt 'fully inside' modifier is read during the drag/at release, not at the press
- Shift=add, Ctrl/Cmd=remove (Ctrl wins when both are held); no modifier replaces, an empty plain marquee clears the selection
- Inactive (activeInHierarchy=false) objects are not marquee candidates, matching click picking (includeInactive=false)
- GetEntityBounds boxes include descendants, so in intersect mode a parent whose union box touches the rectangle is selected too (plan's AABB-level precision taken literally); user may want leaf-preferring or own-bounds semantics later
- Bounds cache key = hierarchy revision + connection + scene-change count (any objectsChanged invalidates too); chunking reuses createPickBackend's existing 4096 batches
- Wheel zoom is refused while a marquee is drawn; selected rows are revealed (ancestors expanded) via the public HierarchyState.expandAncestors, hierarchy-state.ts untouched
- DOM tests drive the panel's left-button glue (selectGesture) on a plain element with real pointer/key events; the Viewport panel itself is not rendered (jsdom has no canvas)
- Frame Selected (F / toolbar) is refused while a marquee is drawn, and rechecked after its awaits (review fix)
