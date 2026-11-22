#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include <math/UUID.hpp>
#include <nlohmann/json.hpp>

namespace N2Engine::PrefabNesting
{
    /**
     * Nested prefabs (orchestrator/proposals/prefab-nesting.md, docs/serialization.html "Prefab links"). An object in
     * scene or prefab JSON can be a LINK to a prefab instead of a full object:
     *
     *   { "uuid": L, "name": ..., "positionable": {...},
     *     "prefabInstance": { "prefab": <prefab asset UUID>,
     *                         "overrides": [ { "source": S, "component"?: C, "path": P, "value": V } ],
     *                         "removedObjects": [S], "removedComponents": [C],
     *                         "addedComponents": [ { "source": S, "component": <component entry> } ],
     *                         "addedChildren": [ { "source": S, "object": <GameObject JSON> } ] } }
     *
     * S and C name an object or component of the linked prefab by its id there; an object of a link inside that
     * prefab is named by a path of ids, outer to inner ("b2.../d4..."). Expand replaces the link by the prefab's
     * tree. Every object and component inside gets a deterministic id: id(x) = DeriveId(id(its link root),
     * its source id), so every expansion of the same file gives the same ids, and two links to one prefab never
     * collide. The root takes the link's own uuid, name, local position and local rotation (instance-owned values).
     *
     * Expanded objects carry marks (GameObject::IsPrefabLinked, GetPrefabLink, GetPrefabSource and
     * Component::GetPrefabSource) that Collapse uses to write the link back, and that
     * PrefabManager::InstantiatePrefabs uses to derive the ids of a copy from its fresh link ids.
     */

    /// Most links inside links one expansion follows
    inline constexpr std::size_t MaxLinkDepth = 64;
    /// Most objects one link may expand to (the editor's paste and instantiate cap)
    inline constexpr std::size_t MaxLinkObjects = 10000;

    /// Where expansion reads prefabs from: the root object's JSON of a prefab asset, or nullptr when there is none
    class PrefabSource
    {
    public:
        virtual ~PrefabSource() = default;
        /// The prefab's "rootObject" value; it must stay valid for the source's lifetime
        [[nodiscard]] virtual const nlohmann::json *LoadRoot(const Math::UUID &prefab) = 0;
    };

    /// Prefab assets through IO::ResourceLoader (PrefabFile), each loaded once per source
    class ResourcePrefabSource final : public PrefabSource
    {
    public:
        [[nodiscard]] const nlohmann::json *LoadRoot(const Math::UUID &prefab) override;

    private:
        // By the UUID's text: the loaded files (kept alive) and their root objects (nullptr: tried, none)
        std::unordered_map<std::string, std::shared_ptr<const void>> _held;
        std::unordered_map<std::string, const nlohmann::json *> _roots;
    };

    /// What one Expand call did, for a summary log line
    struct ExpandReport
    {
        std::size_t links = 0;           // links expanded (at every depth)
        std::size_t errors = 0;          // links that failed (missing prefab, cycle, a cap): each became a placeholder
        std::size_t droppedChanges = 0;  // overrides, removals and additions naming something the prefab lacks
    };

    /// The id of an object or component with source id `source` inside the link whose root has id `link`
    [[nodiscard]] Math::UUID DeriveId(const Math::UUID &link, const Math::UUID &source);

    /// Whether a GameObject JSON tree (or an array of them) holds a link anywhere
    [[nodiscard]] bool ContainsLinks(const nlohmann::json &objectsOrObject);

    /**
     * Replaces every link in a GameObject JSON tree (or an array of root trees) by the linked prefab's tree, in
     * place. Never throws for bad data: a link that cannot be expanded (missing or unreadable prefab, a cycle, past
     * MaxLinkDepth or MaxLinkObjects, no valid uuid) is logged and becomes an empty object with its own uuid, name
     * and transform; a change naming something the prefab does not have is logged and skipped.
     */
    void Expand(nlohmann::json &objectsOrObject, PrefabSource &source, ExpandReport &report);

    /// Expand with prefabs read through ResourceLoader; logs one summary line when it expanded or dropped anything
    void Expand(nlohmann::json &objectsOrObject, const char *what);

    /**
     * The inverse of Expand, for the editor: every expanded link root in a GameObject JSON tree (as
     * GameObject::Serialize writes it, marks included) is replaced by its link. The instance is compared with a fresh
     * expansion of its prefab (same ids, so objects correspond by id): what differs becomes overrides, removed
     * and added objects and components. The root's name, local position and local rotation are the link's own values.
     * Marks are not written into links (only expanded objects carry them). Returns false (logged) when a prefab can't
     * be loaded: that instance is then left expanded, so nothing is lost.
     */
    bool Collapse(nlohmann::json &objectsOrObject, PrefabSource &source);

    /// Removes the link marks (prefabLinked, prefabLink, prefabSource and a component entry's prefabSource) from every
    /// object of a GameObject JSON tree. Links themselves (prefabInstance) carry none and are left as they are.
    void StripMarks(nlohmann::json &objectJson);

    /**
     * ApplyPrefab's content for an expanded instance (GameObject JSON as Serialize writes it, its root a link root):
     * the prefab's rootObject as the instance has it now, in the prefab's OWN ids. The root and every object and
     * component of this link get back their id in the prefab (their prefabSource); objects and components the instance
     * added keep theirs; links inside it are collapsed into links, with their ids in the prefab; marks and the root's
     * prefabReference are removed. nullopt (logged) when the prefab can't be loaded or the data is not a link root.
     */
    [[nodiscard]] std::optional<nlohmann::json> InstanceAsPrefab(const nlohmann::json &instance, PrefabSource &source);
}
