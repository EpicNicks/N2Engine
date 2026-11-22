#include "engine/prefabs/PrefabNesting.hpp"

#include <algorithm>
#include <exception>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "engine/GameObject.hpp"
#include "engine/Logger.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/prefabs/PrefabFile.hpp"
#include "engine/serialization/JsonLimits.hpp"
#include "engine/serialization/ReferenceResolver.hpp"

namespace N2Engine::PrefabNesting
{
    using json = nlohmann::json;

    namespace
    {
        // The canonical text of a UUID string, or nullopt if the value isn't one
        std::optional<std::string> Canon(const json &value)
        {
            if (!value.is_string())
            {
                return std::nullopt;
            }
            const auto &text = value.get_ref<const std::string &>();
            if (text.size() != 36)
            {
                return std::nullopt;
            }
            if (const auto uuid = Math::UUID::FromString(text))
            {
                return uuid->ToString();
            }
            return std::nullopt;
        }

        std::optional<std::string> CanonKey(const json &object, const char *key)
        {
            if (!object.is_object())
            {
                return std::nullopt;
            }
            const auto it = object.find(key);
            return it == object.end() ? std::nullopt : Canon(*it);
        }

        std::string Derive(const std::string &link, const std::string &source)
        {
            return DeriveId(*Math::UUID::FromString(link), *Math::UUID::FromString(source)).ToString();
        }

        // An object's name for a log line, whatever its "name" holds
        std::string NameOf(const json &object)
        {
            const auto it = object.is_object() ? object.find("name") : object.end();
            return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
        }

        bool IsLinkedRoot(const json &object)
        {
            const auto it = object.find("prefabLinked");
            return it != object.end() && it->is_boolean() && it->get<bool>();
        }

        json *ChildrenOf(json &object)
        {
            const auto it = object.find("children");
            return it != object.end() && it->is_array() ? &*it : nullptr;
        }

        json *ComponentsOf(json &object)
        {
            const auto it = object.find("components");
            return it != object.end() && it->is_array() ? &*it : nullptr;
        }

        // Visits a GameObject JSON tree, parents before children, with each object's parent (nullptr for the root).
        // A stack on the heap, not recursion: the tree can be as deep as scene data allows.
        void ForEachObject(json &root, const std::function<void(json &object, json *parent)> &visit)
        {
            std::vector<std::pair<json *, json *>> pending{{&root, nullptr}};
            while (!pending.empty())
            {
                auto [object, parent] = pending.back();
                pending.pop_back();
                if (!object->is_object())
                {
                    continue;
                }
                visit(*object, parent);
                if (json *children = ChildrenOf(*object))
                {
                    for (auto it = children->rbegin(); it != children->rend(); ++it)
                    {
                        pending.emplace_back(&*it, object);
                    }
                }
            }
        }

        // Rewrites every string that is a key of `ids` to its value, anywhere in `value`, except the values of
        // "prefabSource" keys (they name ids in a prefab, which must stay as they are). Iterative.
        void Renumber(json &value, const std::unordered_map<std::string, std::string> &ids)
        {
            std::vector<json *> pending{&value};
            while (!pending.empty())
            {
                json &item = *pending.back();
                pending.pop_back();
                if (item.is_string())
                {
                    if (const auto canonical = Canon(item))
                    {
                        if (const auto it = ids.find(*canonical); it != ids.end())
                        {
                            item = it->second;
                        }
                    }
                }
                else if (item.is_object())
                {
                    for (auto &entry : item.items())
                    {
                        if (entry.key() != "prefabSource")
                        {
                            pending.push_back(&entry.value());
                        }
                    }
                }
                else if (item.is_array())
                {
                    for (auto &element : item)
                    {
                        pending.push_back(&element);
                    }
                }
            }
        }

        // "a/b/c" -> the id an object or component has in the expanded prefab it was named in (fold with Derive)
        std::optional<std::string> FoldPath(const json &path)
        {
            if (!path.is_string())
            {
                return std::nullopt;
            }
            const std::string &text = path.get_ref<const std::string &>();
            std::optional<std::string> current;
            std::size_t start = 0;
            while (start <= text.size())
            {
                const std::size_t slash = text.find('/', start);
                const std::size_t end = slash == std::string::npos ? text.size() : slash;
                const auto part = Canon(json(text.substr(start, end - start)));
                if (!part)
                {
                    return std::nullopt;
                }
                current = current ? Derive(*current, *part) : *part;
                if (slash == std::string::npos)
                {
                    break;
                }
                start = slash + 1;
            }
            return current;
        }

        // Sets `value` at a dotted path ("positionable.localScale", "scriptData.speed") inside `target`
        void SetAtPath(json &target, const std::string &path, json value)
        {
            json *node = &target;
            std::size_t start = 0;
            while (true)
            {
                const std::size_t dot = path.find('.', start);
                const std::string key = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
                if (!node->is_object())
                {
                    *node = json::object();
                }
                if (dot == std::string::npos)
                {
                    (*node)[key] = std::move(value);
                    return;
                }
                node = &(*node)[key];
                start = dot + 1;
            }
        }

        // An empty object in place of a link that could not be expanded: its own id, name and transform
        json Placeholder(const json &link)
        {
            json placeholder = json::object();
            for (const char *key : {"uuid", "name", "tag", "layer", "isActive", "positionable"})
            {
                if (const auto it = link.find(key); it != link.end())
                {
                    placeholder[key] = *it;
                }
            }
            if (!placeholder.contains("name"))
            {
                placeholder["name"] = "Missing prefab";
            }
            if (!CanonKey(placeholder, "uuid"))
            {
                placeholder["uuid"] = Math::UUID::Random().ToString();
            }
            placeholder["components"] = json::array();
            placeholder["children"] = json::array();
            return placeholder;
        }

        struct Expander
        {
            PrefabSource &source;
            ExpandReport &report;
            std::vector<std::string> visiting; // the prefabs being expanded, outer to inner (cycle check)

            // Walked with a stack on the heap, not recursion: a tree can be as deep as the hierarchy cap, and a frame per
            // level overflowed the 1 MiB stack of an MSVC Debug build (the depth-capped prefab test). Link replaces only
            // the object it is given, never a pending one, so the pointers kept here stay valid.
            void Tree(json &root, const std::size_t rootLevel)
            {
                std::vector<std::pair<json *, std::size_t>> pending{{&root, rootLevel}};
                while (!pending.empty())
                {
                    auto [object, level] = pending.back();
                    pending.pop_back();
                    if (!object->is_object() || level > JsonLimits::MaxHierarchyDepth)
                    {
                        continue; // past the cap GameObject::Deserialize refuses the data anyway
                    }
                    if (object->contains("prefabInstance"))
                    {
                        Link(*object, level);
                        continue;
                    }
                    if (json *children = ChildrenOf(*object))
                    {
                        for (auto it = children->rbegin(); it != children->rend(); ++it)
                        {
                            pending.emplace_back(&*it, level + 1);
                        }
                    }
                }
            }

            void Fail(json &link, const std::string &why)
            {
                const std::string id = link.contains("uuid") && link["uuid"].is_string() ? link["uuid"].get<std::string>() : "?";
                Logger::Error(std::format("Prefab link '{}' ({}): {}", NameOf(link), id, why));
                ++report.errors;
                link = Placeholder(link);
            }

            void Dropped(const json &link, const std::string &what)
            {
                Logger::Warn(std::format("Prefab link '{}': {} (skipped)", NameOf(link), what));
                ++report.droppedChanges;
            }

            void Link(json &link, const std::size_t level)
            {
                ++report.links;
                const auto linkId = CanonKey(link, "uuid");
                if (!linkId)
                {
                    Fail(link, "the link has no valid uuid");
                    return;
                }
                const json &instance = link["prefabInstance"];
                const auto prefab = CanonKey(instance, "prefab");
                if (!instance.is_object() || !prefab)
                {
                    Fail(link, "prefabInstance names no prefab");
                    return;
                }
                if (std::ranges::find(visiting, *prefab) != visiting.end())
                {
                    Fail(link, std::format("prefab {} contains itself (a cycle)", *prefab));
                    return;
                }
                if (visiting.size() >= MaxLinkDepth)
                {
                    Fail(link, std::format("links nest more than {} deep", MaxLinkDepth));
                    return;
                }
                const json *root = source.LoadRoot(*Math::UUID::FromString(*prefab));
                if (root == nullptr || !root->is_object())
                {
                    Fail(link, std::format("prefab {} can't be loaded", *prefab));
                    return;
                }

                // The prefab's tree, its own links expanded first (they keep the prefab's own ids as link ids)
                json tree = JsonLimits::IterativeCopy(*root);
                visiting.push_back(*prefab);
                Tree(tree, level);
                visiting.pop_back();
                const auto rootOld = CanonKey(tree, "uuid");
                if (!rootOld)
                {
                    Fail(link, std::format("prefab {} has a root with no valid uuid", *prefab));
                    return;
                }

                // Marks, then ids: id(x) = Derive(id(x's link root), x's source id), parents first
                std::unordered_map<std::string, std::string> ids{{*rootOld, *linkId}};
                std::size_t objects = 0;
                ForEachObject(tree, [&](json &object, json *)
                {
                    ++objects;
                    const bool isRoot = &object == &tree;
                    const auto oldId = CanonKey(object, "uuid");
                    std::string componentLink; // the new id of the link this object's components came from
                    if (isRoot)
                    {
                        object["prefabLinked"] = true;
                        object["prefabReference"] = *prefab;
                        object.erase("prefabLink");
                        object.erase("prefabSource");
                        componentLink = *linkId;
                    }
                    else
                    {
                        const auto ownLink = CanonKey(object, "prefabLink");
                        const bool direct = !ownLink || *ownLink == *rootOld || !ids.contains(*ownLink);
                        if (direct)
                        {
                            object["prefabLink"] = *rootOld;
                            if (oldId)
                            {
                                object["prefabSource"] = *oldId;
                            }
                        }
                        const auto link = CanonKey(object, "prefabLink");
                        const auto source = CanonKey(object, "prefabSource");
                        if (oldId && link && source && ids.contains(*link))
                        {
                            ids[*oldId] = Derive(ids[*link], *source);
                        }
                        if (IsLinkedRoot(object) && oldId && ids.contains(*oldId))
                        {
                            componentLink = ids[*oldId]; // a link inside the prefab: its components are its own
                        }
                        else if (direct)
                        {
                            componentLink = *linkId;
                        }
                        else if (link && ids.contains(*link))
                        {
                            componentLink = ids[*link];
                        }
                    }
                    const bool ownComponents = isRoot || (!IsLinkedRoot(object) &&
                                                          CanonKey(object, "prefabLink") == rootOld);
                    if (json *components = ComponentsOf(object))
                    {
                        for (json &entry : *components)
                        {
                            if (!entry.is_object() || !entry.contains("data"))
                            {
                                continue;
                            }
                            const auto oldComponent = CanonKey(entry["data"], "uuid");
                            if (ownComponents && oldComponent)
                            {
                                entry["prefabSource"] = *oldComponent;
                            }
                            const auto source = CanonKey(entry, "prefabSource");
                            if (oldComponent && source && !componentLink.empty())
                            {
                                ids[*oldComponent] = Derive(componentLink, *source);
                            }
                        }
                    }
                });
                if (objects > MaxLinkObjects)
                {
                    Fail(link, std::format("prefab {} expands to more than {} objects", *prefab, MaxLinkObjects));
                    return;
                }
                Renumber(tree, ids);

                // The instance's changes, addressed by ids in the prefab (paths through its links)
                auto target = [&](const json &path) -> std::optional<std::string>
                {
                    const auto folded = FoldPath(path);
                    if (!folded)
                    {
                        return std::nullopt;
                    }
                    const auto it = ids.find(*folded);
                    return it == ids.end() ? std::nullopt : std::optional<std::string>(it->second);
                };
                std::unordered_map<std::string, std::pair<json *, json *>> index; // id -> (object, parent)
                auto reindex = [&]
                {
                    index.clear();
                    ForEachObject(tree, [&](json &object, json *parent)
                    {
                        if (const auto id = CanonKey(object, "uuid"))
                        {
                            index[*id] = {&object, parent};
                        }
                    });
                };
                auto findObject = [&](const json &path) -> json *
                {
                    const auto id = target(path);
                    const auto it = id ? index.find(*id) : index.end();
                    return it == index.end() ? nullptr : it->second.first;
                };
                auto findComponent = [&](json &object, const std::string &id) -> json *
                {
                    if (json *components = ComponentsOf(object))
                    {
                        for (json &entry : *components)
                        {
                            if (entry.is_object() && entry.contains("data") && CanonKey(entry["data"], "uuid") == id)
                            {
                                return &entry;
                            }
                        }
                    }
                    return nullptr;
                };
                auto list = [&instance](const char *key) -> const json &
                {
                    static const json empty = json::array();
                    const auto it = instance.find(key);
                    return it != instance.end() && it->is_array() ? *it : empty;
                };

                reindex();
                for (const json &change : list("overrides"))
                {
                    json *object = change.is_object() ? findObject(change.value("source", json())) : nullptr;
                    const auto pathIt = change.is_object() ? change.find("path") : change.end();
                    if (object == nullptr || pathIt == change.end() || !pathIt->is_string() || !change.contains("value"))
                    {
                        Dropped(link, "an override names nothing in the prefab");
                        continue;
                    }
                    const std::string path = pathIt->get<std::string>();
                    if (change.contains("component"))
                    {
                        const auto componentId = target(change["component"]);
                        json *entry = componentId ? findComponent(*object, *componentId) : nullptr;
                        if (entry == nullptr)
                        {
                            Dropped(link, "an override names a component the prefab does not have");
                            continue;
                        }
                        SetAtPath((*entry)["data"], path, change["value"]);
                        continue;
                    }
                    if (object == &tree && (path == "name" || path == "positionable.localPosition" ||
                                            path == "positionable.localRotation"))
                    {
                        continue; // the link's own values (below)
                    }
                    SetAtPath(*object, path, change["value"]);
                }
                for (const json &added : list("addedComponents"))
                {
                    json *object = added.is_object() ? findObject(added.value("source", json())) : nullptr;
                    if (object == nullptr || !added.contains("component") || !added["component"].is_object())
                    {
                        Dropped(link, "an added component names no object of the prefab");
                        continue;
                    }
                    json entry = added["component"];
                    entry.erase("prefabSource");
                    if (!object->contains("components") || !(*object)["components"].is_array())
                    {
                        (*object)["components"] = json::array();
                    }
                    (*object)["components"].push_back(std::move(entry));
                }
                for (const json &removed : list("removedComponents"))
                {
                    const auto id = target(removed);
                    bool done = false;
                    if (id)
                    {
                        for (auto &[objectId, place] : index)
                        {
                            if (json *components = ComponentsOf(*place.first))
                            {
                                for (auto it = components->begin(); it != components->end(); ++it)
                                {
                                    if (it->is_object() && it->contains("data") && CanonKey((*it)["data"], "uuid") == id)
                                    {
                                        components->erase(it);
                                        done = true;
                                        break;
                                    }
                                }
                            }
                            if (done)
                            {
                                break;
                            }
                        }
                    }
                    if (!done)
                    {
                        Dropped(link, "a removed component is not in the prefab");
                    }
                }
                for (const json &removed : list("removedObjects"))
                {
                    reindex();
                    const auto id = target(removed);
                    const auto it = id ? index.find(*id) : index.end();
                    if (it == index.end() || it->second.second == nullptr)
                    {
                        Dropped(link, "a removed object is not in the prefab (or is its root)");
                        continue;
                    }
                    json *children = ChildrenOf(*it->second.second);
                    const json *object = it->second.first;
                    if (children != nullptr)
                    {
                        for (auto child = children->begin(); child != children->end(); ++child)
                        {
                            if (&*child == object)
                            {
                                children->erase(child);
                                break;
                            }
                        }
                    }
                }
                for (const json &added : list("addedChildren"))
                {
                    reindex();
                    json *object = added.is_object() ? findObject(added.value("source", json())) : nullptr;
                    if (object == nullptr || !added.contains("object") || !added["object"].is_object())
                    {
                        Dropped(link, "an added child names no object of the prefab");
                        continue;
                    }
                    json child = JsonLimits::IterativeCopy(added["object"]);
                    Tree(child, level + 1); // it may hold links of its own
                    if (!object->contains("children") || !(*object)["children"].is_array())
                    {
                        (*object)["children"] = json::array();
                    }
                    (*object)["children"].push_back(std::move(child));
                }

                // The link's own values: its id (already), name, local position and local rotation
                if (const auto name = link.find("name"); name != link.end() && name->is_string())
                {
                    tree["name"] = *name;
                }
                if (const auto positionable = link.find("positionable");
                    positionable != link.end() && positionable->is_object())
                {
                    for (const char *part : {"localPosition", "localRotation"})
                    {
                        if (const auto value = positionable->find(part); value != positionable->end())
                        {
                            tree["positionable"][part] = *value;
                        }
                    }
                }
                link = std::move(tree);
            }
        };

        // ===== Collapse =====

        void StripMarksOf(json &root)
        {
            ForEachObject(root, [](json &object, json *)
            {
                object.erase("prefabLinked");
                object.erase("prefabLink");
                object.erase("prefabSource");
                if (json *components = ComponentsOf(object))
                {
                    for (json &entry : *components)
                    {
                        if (entry.is_object())
                        {
                            entry.erase("prefabSource");
                        }
                    }
                }
            });
        }

        struct Collapser
        {
            PrefabSource &source;
            bool ok = true;

            // Iterative, as Expander::Tree (a frame per level overflows a Debug build's stack at the hierarchy cap).
            // Link replaces only the object it is given, so the pending pointers stay valid.
            void Tree(json &root)
            {
                std::vector<json *> pending{&root};
                while (!pending.empty())
                {
                    json *object = pending.back();
                    pending.pop_back();
                    if (!object->is_object())
                    {
                        continue;
                    }
                    if (IsLinkedRoot(*object) && CanonKey(*object, "prefabReference") && CanonKey(*object, "uuid"))
                    {
                        if (Link(*object))
                        {
                            continue;
                        }
                    }
                    if (json *children = ChildrenOf(*object))
                    {
                        for (auto it = children->rbegin(); it != children->rend(); ++it)
                        {
                            pending.push_back(&*it);
                        }
                    }
                }
            }

            // The instance as its prefab gives it now, normalized through the components' own serializers
            std::optional<json> Baseline(const json &instance, const std::string &linkId, const std::string &prefab)
            {
                json base = {{"uuid", linkId},
                             {"name", NameOf(instance)},
                             {"prefabInstance", {{"prefab", prefab}}}};
                if (const auto positionable = instance.find("positionable"); positionable != instance.end())
                {
                    base["positionable"] = *positionable;
                }
                ExpandReport report;
                Expander expander{source, report, {}};
                expander.Tree(base, 1);
                if (report.errors > 0)
                {
                    return std::nullopt;
                }
                try
                {
                    ReferenceResolver resolver;
                    const auto built = GameObject::Deserialize(base, &resolver);
                    resolver.ResolveAll();
                    if (built)
                    {
                        return built->Serialize();
                    }
                }
                catch (const std::exception &e)
                {
                    Logger::Warn(std::format("Collapse: the prefab {} did not rebuild ({}); comparing its raw data", prefab, e.what()));
                }
                return base;
            }

            bool Link(json &instance)
            {
                const std::string linkId = *CanonKey(instance, "uuid");
                const std::string prefab = *CanonKey(instance, "prefabReference");
                const json *prefabRoot = source.LoadRoot(*Math::UUID::FromString(prefab));
                const auto rootSource = prefabRoot ? CanonKey(*prefabRoot, "uuid") : std::nullopt;
                std::optional<json> baseline = rootSource ? Baseline(instance, linkId, prefab) : std::nullopt;
                if (!baseline)
                {
                    Logger::Error(std::format("Collapse: prefab {} can't be loaded; '{}' is saved expanded", prefab,
                                              NameOf(instance)));
                    ok = false;
                    return false;
                }

                struct Entry
                {
                    json *object;
                    json *parent;
                };
                std::unordered_map<std::string, Entry> base, mine;
                std::vector<std::string> baseOrder;
                ForEachObject(*baseline, [&](json &object, json *parent)
                {
                    if (const auto id = CanonKey(object, "uuid"))
                    {
                        base[*id] = {&object, parent};
                        baseOrder.push_back(*id);
                    }
                });
                ForEachObject(instance, [&](json &object, json *parent)
                {
                    if (const auto id = CanonKey(object, "uuid"))
                    {
                        mine[*id] = {&object, parent};
                    }
                });

                // The path an id of the baseline has in the prefab ("source", or "link/source" through its links)
                std::unordered_map<std::string, std::string> paths;
                std::function<std::string(const std::string &)> linkPrefix = [&](const std::string &link) -> std::string
                {
                    if (link == linkId)
                    {
                        return "";
                    }
                    const auto it = paths.find(link);
                    return it == paths.end() ? "" : it->second + "/";
                };
                paths[linkId] = *rootSource;
                for (const std::string &id : baseOrder)
                {
                    if (id == linkId)
                    {
                        continue;
                    }
                    const json &object = *base[id].object;
                    const auto link = CanonKey(object, "prefabLink");
                    const auto src = CanonKey(object, "prefabSource");
                    if (link && src)
                    {
                        paths[id] = linkPrefix(*link) + *src;
                    }
                }
                auto componentPath = [&](const json &object, const std::string &objectId, const json &entry)
                    -> std::optional<std::string>
                {
                    const auto src = CanonKey(entry, "prefabSource");
                    if (!src)
                    {
                        return std::nullopt;
                    }
                    std::string link = linkId;
                    if (objectId != linkId)
                    {
                        if (IsLinkedRoot(object))
                        {
                            link = objectId;
                        }
                        else if (const auto own = CanonKey(object, "prefabLink"))
                        {
                            link = *own;
                        }
                    }
                    return linkPrefix(link) + *src;
                };

                json overrides = json::array(), removedObjects = json::array(), removedComponents = json::array(),
                     addedComponents = json::array(), addedChildren = json::array();
                auto addOverride = [&](const std::string &object, const std::optional<std::string> &component,
                                       const std::string &path, const json &value)
                {
                    json change = {{"source", object}};
                    if (component)
                    {
                        change["component"] = *component;
                    }
                    change["path"] = path;
                    change["value"] = value;
                    overrides.push_back(std::move(change));
                };
                auto cleanCopy = [&](const json &object)
                {
                    json copy = JsonLimits::IterativeCopy(object);
                    Tree(copy); // an added object can itself be an instance
                    StripMarksOf(copy);
                    return copy;
                };

                for (const std::string &id : baseOrder)
                {
                    const auto pathIt = paths.find(id);
                    if (pathIt == paths.end())
                    {
                        continue;
                    }
                    const std::string &path = pathIt->second;
                    const Entry &b = base[id];
                    const auto mineIt = mine.find(id);
                    if (mineIt == mine.end())
                    {
                        const auto parentId = b.parent ? CanonKey(*b.parent, "uuid") : std::nullopt;
                        if (parentId && mine.contains(*parentId))
                        {
                            removedObjects.push_back(path); // the topmost removed object
                        }
                        continue;
                    }
                    const bool isRoot = id == linkId;
                    json &theirs = *b.object;
                    json &ours = *mineIt->second.object;
                    for (const char *key : {"name", "tag", "layer", "isActive"})
                    {
                        if (isRoot && std::string_view(key) == "name")
                        {
                            continue;
                        }
                        if (ours.contains(key) && (!theirs.contains(key) || theirs[key] != ours[key]))
                        {
                            addOverride(path, std::nullopt, key, ours[key]);
                        }
                    }
                    if (ours.contains("positionable") && ours["positionable"].is_object())
                    {
                        const json &own = ours["positionable"];
                        const json prefabTransform = theirs.contains("positionable") ? theirs["positionable"] : json::object();
                        for (const char *part : {"localPosition", "localRotation", "localScale"})
                        {
                            if (isRoot && std::string_view(part) != "localScale")
                            {
                                continue;
                            }
                            if (own.contains(part) && (!prefabTransform.contains(part) || prefabTransform[part] != own[part]))
                            {
                                addOverride(path, std::nullopt, std::string("positionable.") + part, own[part]);
                            }
                        }
                    }
                    // Components, matched by id
                    std::unordered_map<std::string, json *> theirComponents;
                    if (json *components = ComponentsOf(theirs))
                    {
                        for (json &entry : *components)
                        {
                            if (entry.is_object() && entry.contains("data"))
                            {
                                if (const auto componentId = CanonKey(entry["data"], "uuid"))
                                {
                                    theirComponents[*componentId] = &entry;
                                }
                            }
                        }
                    }
                    std::unordered_map<std::string, bool> seen;
                    if (json *components = ComponentsOf(ours))
                    {
                        for (json &entry : *components)
                        {
                            const auto componentId = entry.is_object() && entry.contains("data") ? CanonKey(entry["data"], "uuid") : std::nullopt;
                            const auto it = componentId ? theirComponents.find(*componentId) : theirComponents.end();
                            if (it == theirComponents.end())
                            {
                                json added = JsonLimits::IterativeCopy(entry);
                                added.erase("prefabSource");
                                addedComponents.push_back({{"source", path}, {"component", std::move(added)}});
                                continue;
                            }
                            seen[*componentId] = true;
                            const auto cpath = componentPath(theirs, id, *it->second);
                            if (!cpath)
                            {
                                continue;
                            }
                            const json &prefabData = (*it->second)["data"];
                            const json &ownData = entry["data"];
                            for (const auto &field : ownData.items())
                            {
                                if (field.key() == "uuid")
                                {
                                    continue;
                                }
                                const auto prefabField = prefabData.find(field.key());
                                if (field.key() == "scriptData" && field.value().is_object() && prefabField != prefabData.end() &&
                                    prefabField->is_object())
                                {
                                    for (const auto &inner : field.value().items())
                                    {
                                        const auto prefabInner = prefabField->find(inner.key());
                                        if (prefabInner == prefabField->end() || *prefabInner != inner.value())
                                        {
                                            addOverride(path, cpath, "scriptData." + inner.key(), inner.value());
                                        }
                                    }
                                    continue;
                                }
                                if (prefabField == prefabData.end() || *prefabField != field.value())
                                {
                                    addOverride(path, cpath, field.key(), field.value());
                                }
                            }
                        }
                    }
                    for (const auto &[componentId, entry] : theirComponents)
                    {
                        if (!seen.contains(componentId))
                        {
                            if (const auto cpath = componentPath(theirs, id, *entry))
                            {
                                removedComponents.push_back(*cpath);
                            }
                        }
                    }
                    // Children the instance added under this object
                    if (json *children = ChildrenOf(ours))
                    {
                        for (json &child : *children)
                        {
                            const auto childId = CanonKey(child, "uuid");
                            if (!childId || !base.contains(*childId))
                            {
                                addedChildren.push_back({{"source", path}, {"object", cleanCopy(child)}});
                            }
                        }
                    }
                }

                json out = json::object();
                out["uuid"] = linkId;
                out["name"] = NameOf(instance);
                if (const auto positionable = instance.find("positionable"); positionable != instance.end() && positionable->is_object())
                {
                    for (const char *part : {"localPosition", "localRotation"})
                    {
                        if (const auto value = positionable->find(part); value != positionable->end())
                        {
                            out["positionable"][part] = *value;
                        }
                    }
                }
                json link = {{"prefab", prefab}};
                if (!overrides.empty()) link["overrides"] = std::move(overrides);
                if (!removedObjects.empty()) link["removedObjects"] = std::move(removedObjects);
                if (!removedComponents.empty()) link["removedComponents"] = std::move(removedComponents);
                if (!addedComponents.empty()) link["addedComponents"] = std::move(addedComponents);
                if (!addedChildren.empty()) link["addedChildren"] = std::move(addedChildren);
                out["prefabInstance"] = std::move(link);
                instance = std::move(out);
                return true;
            }
        };
    }

    const json *ResourcePrefabSource::LoadRoot(const Math::UUID &prefab)
    {
        const std::string key = prefab.ToString();
        if (const auto it = _roots.find(key); it != _roots.end())
        {
            return it->second;
        }
        const json *root = nullptr;
        if (const auto file = IO::ResourceLoader::Instance().LoadByUUID<PrefabFile>(prefab))
        {
            const json &data = file->GetData();
            if (const auto it = data.find("rootObject"); it != data.end() && it->is_object())
            {
                root = &*it;
            }
            _held[key] = file;
        }
        _roots[key] = root;
        return root;
    }

    Math::UUID DeriveId(const Math::UUID &link, const Math::UUID &source)
    {
        return Math::UUID::GenerateNameBased(link, "n2prefab:" + source.ToString());
    }

    bool ContainsLinks(const json &objectsOrObject)
    {
        std::vector<const json *> pending{&objectsOrObject};
        while (!pending.empty())
        {
            const json &value = *pending.back();
            pending.pop_back();
            if (value.is_array())
            {
                for (const json &item : value)
                {
                    pending.push_back(&item);
                }
                continue;
            }
            if (!value.is_object())
            {
                continue;
            }
            if (value.contains("prefabInstance"))
            {
                return true;
            }
            if (const auto children = value.find("children"); children != value.end() && children->is_array())
            {
                for (const json &child : *children)
                {
                    pending.push_back(&child);
                }
            }
        }
        return false;
    }

    void Expand(json &objectsOrObject, PrefabSource &source, ExpandReport &report)
    {
        Expander expander{source, report, {}};
        if (objectsOrObject.is_array())
        {
            for (json &object : objectsOrObject)
            {
                expander.Tree(object, 1);
            }
        }
        else
        {
            expander.Tree(objectsOrObject, 1);
        }
    }

    void Expand(json &objectsOrObject, const char *what)
    {
        ResourcePrefabSource source;
        ExpandReport report;
        Expand(objectsOrObject, source, report);
        if (report.errors > 0 || report.droppedChanges > 0)
        {
            Logger::Warn(std::format("{}: expanded {} prefab links; {} failed, {} changes skipped (see above)", what,
                                     report.links, report.errors, report.droppedChanges));
        }
    }

    void StripMarks(json &objectJson)
    {
        StripMarksOf(objectJson);
    }

    std::optional<json> InstanceAsPrefab(const json &instance, PrefabSource &source)
    {
        const auto linkId = CanonKey(instance, "uuid");
        const auto prefab = CanonKey(instance, "prefabReference");
        if (!linkId || !prefab || !IsLinkedRoot(instance))
        {
            Logger::Error("InstanceAsPrefab: the object is not the root of a prefab link");
            return std::nullopt;
        }
        const json *fileRoot = source.LoadRoot(*Math::UUID::FromString(*prefab));
        const auto rootFileId = fileRoot ? CanonKey(*fileRoot, "uuid") : std::nullopt;
        if (!rootFileId)
        {
            Logger::Error(std::format("InstanceAsPrefab: prefab {} can't be loaded", *prefab));
            return std::nullopt;
        }

        json copy = JsonLimits::IterativeCopy(instance);
        // The ids this link's objects and components have in the prefab (what their prefabSource says)
        std::unordered_map<std::string, std::string> ids{{*linkId, *rootFileId}};
        ForEachObject(copy, [&](json &object, json *)
        {
            const bool isRoot = &object == &copy;
            const bool ofThisLink = isRoot || CanonKey(object, "prefabLink") == linkId;
            if (!ofThisLink)
            {
                return; // an object of a link inside: collapsed below
            }
            if (!isRoot)
            {
                const auto id = CanonKey(object, "uuid");
                const auto src = CanonKey(object, "prefabSource");
                if (id && src)
                {
                    ids[*id] = *src;
                }
            }
            if (!isRoot && IsLinkedRoot(object))
            {
                return; // a link inside: its components are its own prefab's
            }
            if (json *components = ComponentsOf(object))
            {
                for (json &entry : *components)
                {
                    const auto id = entry.is_object() && entry.contains("data") ? CanonKey(entry["data"], "uuid") : std::nullopt;
                    const auto src = CanonKey(entry, "prefabSource");
                    if (id && src)
                    {
                        ids[*id] = *src;
                    }
                }
            }
        });
        // The root stays an object (not a link to itself); links inside become links
        copy.erase("prefabLinked");
        Collapser collapser{source};
        if (json *children = ChildrenOf(copy))
        {
            for (json &child : *children)
            {
                collapser.Tree(child);
            }
        }
        if (!collapser.ok)
        {
            Logger::Warn("InstanceAsPrefab: a link inside could not be collapsed and is written expanded");
        }
        Renumber(copy, ids);
        StripMarksOf(copy);
        copy.erase("prefabReference");
        return copy;
    }

    bool Collapse(json &objectsOrObject, PrefabSource &source)
    {
        Collapser collapser{source};
        if (objectsOrObject.is_array())
        {
            for (json &object : objectsOrObject)
            {
                collapser.Tree(object);
            }
        }
        else
        {
            collapser.Tree(objectsOrObject);
        }
        return collapser.ok;
    }
}
