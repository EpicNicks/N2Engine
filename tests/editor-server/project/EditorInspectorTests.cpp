#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <math/UUID.hpp>
#include <math/Vector3.hpp>
#include <nlohmann/json.hpp>

#include <editor-server/Commands.hpp>
#include <editor-server/EditorServer.hpp>
#include <editor-server/Protocol.hpp>
#include <engine/GameObjectScene.hpp>
#include <engine/io/Resources.hpp>
#include <engine/rendering/Mesh.hpp>
#include <engine/rendering/Texture.hpp>
#include <engine/sceneManagement/Scene.hpp>
#include <engine/sceneManagement/SceneManager.hpp>
#include <engine/serialization/ComponentRegistry.hpp>
#include <engine/serialization/ComponentSerializer.hpp>

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
using nlohmann::json;

// The component and inspector commands (#77, E5) through EditorServer::ExecuteCommand, on a scene made with NewScene:
// GetComponentTypes, AddComponent, RemoveComponent, SetComponentFields, GetComponent and GetLuaFields. With the
// project and hierarchy tests, a program of its own: these tests load scenes into SceneManager.
namespace
{
    constexpr uint8_t ErrorType = static_cast<uint8_t>(ResponseType::Error);
    constexpr uint8_t OkType = static_cast<uint8_t>(ResponseType::Ok);
    constexpr uint8_t SceneInfoType = static_cast<uint8_t>(ResponseType::SceneInfo);
    constexpr uint8_t EntityCreatedType = static_cast<uint8_t>(ResponseType::EntityCreated);
    constexpr uint8_t EntityDataType = static_cast<uint8_t>(ResponseType::EntityData);
    constexpr uint8_t ComponentTypesType = static_cast<uint8_t>(ResponseType::ComponentTypes);
    constexpr uint8_t ComponentAddedType = static_cast<uint8_t>(ResponseType::ComponentAdded);
    constexpr uint8_t ComponentDataType = static_cast<uint8_t>(ResponseType::ComponentData);
    constexpr uint8_t LuaFieldsType = static_cast<uint8_t>(ResponseType::LuaFields);

    constexpr const char *ThingType = "EditorInspectorTest_Thing";

    struct Frame
    {
        uint8_t type = 0xEE;
        std::vector<uint8_t> payload;

        [[nodiscard]] std::string Text() const { return {payload.begin(), payload.end()}; }
    };

    Frame Execute(EditorServer &server, CommandType type, const std::vector<uint8_t> &payload = {})
    {
        const std::vector<uint8_t> frame = server.ExecuteCommand(static_cast<uint8_t>(type), payload);
        BufferReader r(frame);
        Frame out;
        out.type = r.ReadU8();
        const auto body = r.ReadBytes(r.ReadU32());
        out.payload.assign(body.begin(), body.end());
        EXPECT_FALSE(r.HasData()) << "trailing bytes after the response payload";
        return out;
    }

    std::vector<uint8_t> Strings(std::initializer_list<std::string> values)
    {
        BufferWriter w;
        for (const std::string &value : values)
            w.WriteString(value);
        return w.Release();
    }

    enum class InspectorMode
    {
        Fast,
        Slow
    };

    N2_SERIALIZE_ENUM(InspectorMode, {
                      { InspectorMode::Fast, "Fast" },
                      { InspectorMode::Slow, "Slow" }
                      })

    /// A component with one member of each kind the inspector edits, and a count of its OnEditorFieldsChanged calls
    class InspectorThing final : public SerializableComponent
    {
    public:
        explicit InspectorThing(GameObject &gameObject) : SerializableComponent(gameObject)
        {
            RegisterMember("count", count).Range(0.0, 10.0);
            RegisterMember("ratio", ratio);
            RegisterMember("label", label);
            RegisterMember("flag", flag);
            RegisterMember("mode", mode);
            RegisterMember("offset", offset);
            RegisterMember("tint", tint).AsColor();
            RegisterMember("locked", locked).ReadOnly();
            RegisterGameObjectRef("target", target);
            RegisterGameObjectRefVector("targets", targets);
            RegisterAssetRef("texture", texture);
            RegisterAssetRefList("textures", textures);
        }

        [[nodiscard]] std::string GetTypeName() const override { return ThingType; }

        void OnEditorFieldsChanged(std::span<const std::string> changed) override
        {
            ++changedCalls;
            lastChanged.assign(changed.begin(), changed.end());
        }

        int count = 1;
        float ratio = 0.5f;
        std::string label = "a";
        bool flag = false;
        InspectorMode mode = InspectorMode::Slow;
        Math::Vector3 offset = {0.0f, 0.0f, 0.0f};
        Math::Vector3 tint = {1.0f, 1.0f, 1.0f};
        int locked = 5;
        GameObject *target = nullptr;
        std::vector<GameObject *> targets;
        std::shared_ptr<Rendering::Texture> texture;
        std::vector<std::shared_ptr<Rendering::Texture>> textures;

        int changedCalls = 0;
        std::vector<std::string> lastChanged;
    };

    /// A texture registered at runtime (a random UUID, found by this run), unregistered when it goes
    struct RegisteredTexture
    {
        std::shared_ptr<Rendering::Texture> texture;

        RegisteredTexture()
        {
            texture = Rendering::Texture::Create(1, 1, std::vector<std::uint8_t>{1, 2, 3, 255});
            IO::Resources::Instance().RegisterAsset(texture);
        }
        ~RegisteredTexture() { IO::Resources::Instance().UnregisterAsset(texture->GetUUID()); }
        RegisteredTexture(const RegisteredTexture &) = delete;
        RegisteredTexture &operator=(const RegisteredTexture &) = delete;

        [[nodiscard]] std::string Id() const { return texture->GetUUID().ToString(); }
    };

    /// Every sceneChanged event the server pushed
    std::vector<json> SceneChangedEvents(const EditorServer &server)
    {
        std::vector<json> found;
        for (const json &event : server.GetEvents().Read(0, 4096, 0).events)
        {
            if (event.value("kind", "") == "sceneChanged")
                found.push_back(event);
        }
        return found;
    }

    bool Contains(const json &list, const std::string &value)
    {
        for (const json &item : list)
        {
            if (item == value)
                return true;
        }
        return false;
    }

    class EditorInspectorTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            ComponentRegistry::Instance().Register(
                ThingType,
                [](GameObject &gameObject) -> std::unique_ptr<Component> { return std::make_unique<InspectorThing>(gameObject); });
            const Frame made = Execute(server, CommandType::NewScene, Strings({"", "Inspector Test"}));
            ASSERT_EQ(made.type, SceneInfoType) << made.Text();
        }

        std::string Create(const std::string &name)
        {
            BufferWriter w;
            w.WriteString(name);
            w.WriteString("");
            w.WriteI32(-1);
            w.WriteString("");
            const Frame created = Execute(server, CommandType::CreateEntityEx, w.Release());
            EXPECT_EQ(created.type, EntityCreatedType) << created.Text();
            if (created.type != EntityCreatedType)
                return {};
            BufferReader r(created.payload);
            return r.ReadString();
        }

        Frame Add(const std::string &entityId, const std::string &typeName)
        {
            return Execute(server, CommandType::AddComponent, Strings({entityId, typeName}));
        }

        /// AddComponent's componentId (and the values, when asked for)
        std::string AddOk(const std::string &entityId, const std::string &typeName, json *values = nullptr)
        {
            const Frame added = Add(entityId, typeName);
            EXPECT_EQ(added.type, ComponentAddedType) << added.Text();
            if (added.type != ComponentAddedType)
                return {};
            BufferReader r(added.payload);
            std::string componentId = r.ReadString();
            const json read = ReadJson(r);
            EXPECT_FALSE(r.HasData());
            if (values != nullptr)
                *values = read;
            return componentId;
        }

        Frame Remove(const std::string &entityId, const std::string &componentId)
        {
            return Execute(server, CommandType::RemoveComponent, Strings({entityId, componentId}));
        }

        Frame SetFields(const std::string &entityId, const std::string &componentId, const json &values)
        {
            BufferWriter w;
            w.WriteString(entityId);
            w.WriteString(componentId);
            WriteJson(w, values);
            return Execute(server, CommandType::SetComponentFields, w.Release());
        }

        /// The values of a ComponentData frame
        json Values(const Frame &frame)
        {
            EXPECT_EQ(frame.type, ComponentDataType) << frame.Text();
            if (frame.type != ComponentDataType)
                return json::object();
            BufferReader r(frame.payload);
            const json values = ReadJson(r);
            EXPECT_FALSE(r.HasData());
            return values;
        }

        json Get(const std::string &entityId, const std::string &componentId)
        {
            return Values(Execute(server, CommandType::GetComponent, Strings({entityId, componentId})));
        }

        /// SetComponentFields that has to succeed: the values it answers with
        json Set(const std::string &entityId, const std::string &componentId, const json &values)
        {
            return Values(SetFields(entityId, componentId, values));
        }

        void ExpectError(const Frame &frame, const std::string &text)
        {
            EXPECT_EQ(frame.type, ErrorType) << frame.Text();
            EXPECT_NE(frame.Text().find(text), std::string::npos) << "expected '" << text << "' in: " << frame.Text();
        }

        /// GetComponentTypes' schemas
        json Types()
        {
            const Frame frame = Execute(server, CommandType::GetComponentTypes);
            EXPECT_EQ(frame.type, ComponentTypesType) << frame.Text();
            if (frame.type != ComponentTypesType)
                return json::array();
            BufferReader r(frame.payload);
            const json types = ReadJson(r);
            EXPECT_FALSE(r.HasData());
            return types;
        }

        json TypeNamed(const std::string &typeName)
        {
            for (const json &type : Types())
            {
                if (type.at("typeName") == typeName)
                    return type;
            }
            ADD_FAILURE() << "no component type " << typeName;
            return json::object();
        }

        json FieldOf(const json &schema, const std::string &name)
        {
            for (const json &field : schema.at("fields"))
            {
                if (field.at("name") == name)
                    return field;
            }
            ADD_FAILURE() << "no field " << name;
            return json::object();
        }

        /// GetEntity's components: [{type, uuid, values}]
        json EntityComponents(const std::string &entityId)
        {
            const Frame frame = Execute(server, CommandType::GetEntity, Strings({entityId}));
            EXPECT_EQ(frame.type, EntityDataType) << frame.Text();
            if (frame.type != EntityDataType)
                return json::array();
            BufferReader r(frame.payload);
            return ReadJson(r).at("components");
        }

        uint32_t Revision()
        {
            const Frame frame = Execute(server, CommandType::GetOpenScene);
            EXPECT_EQ(frame.type, SceneInfoType) << frame.Text();
            BufferReader r(frame.payload);
            (void)r.ReadString();
            (void)r.ReadString();
            (void)r.ReadString();
            return r.ReadU32();
        }

        /// The live InspectorThing the id names
        static InspectorThing *Thing(const std::string &entityId)
        {
            const auto uuid = Math::UUID::FromString(entityId);
            EXPECT_TRUE(uuid.has_value());
            if (!uuid.has_value())
                return nullptr;
            const std::shared_ptr<GameObject> entity = SceneManager::GetCurSceneRef().FindGameObjectByUUID(uuid.value());
            return entity != nullptr ? entity->GetComponent<InspectorThing>() : nullptr;
        }

        static std::shared_ptr<GameObject> Entity(const std::string &entityId)
        {
            const auto uuid = Math::UUID::FromString(entityId);
            return uuid.has_value() ? SceneManager::GetCurSceneRef().FindGameObjectByUUID(uuid.value()) : nullptr;
        }

        EditorServer server;
    };
}

// ==================== GetComponentTypes ====================

TEST_F(EditorInspectorTest, EveryRegisteredTypeIsListedWithItsFieldsSortedByName)
{
    const json types = Types();
    ASSERT_TRUE(types.is_array());
    std::vector<std::string> names;
    for (const json &type : types)
    {
        names.push_back(type.at("typeName").get<std::string>());
        EXPECT_TRUE(type.at("fields").is_array()) << names.back();
        EXPECT_TRUE(type.at("defaults").is_object()) << names.back();
        EXPECT_TRUE(type.at("defaults").contains("isActive")) << names.back();
        EXPECT_FALSE(type.at("defaults").contains("uuid")) << names.back();
    }
    EXPECT_TRUE(std::is_sorted(names.begin(), names.end()));
    for (const char *builtin : {"Light", "AudioSource", "Rigidbody", "MeshRenderer", "TextRenderer", "LuaComponent",
                                "Canvas", "Button", ThingType})
    {
        EXPECT_NE(std::find(names.begin(), names.end(), builtin), names.end()) << builtin;
    }
}

TEST_F(EditorInspectorTest, ATypesFieldsCarryTheirKindsAndHints)
{
    const json light = TypeNamed("Light");
    const json color = FieldOf(light, "color");
    EXPECT_EQ(color.at("kind"), "Color");
    EXPECT_EQ(color.at("typeName"), "Vector3"); // {x,y,z} in the JSON, shown as a colour
    const json type = FieldOf(light, "type");
    EXPECT_EQ(type.at("kind"), "Enum");
    EXPECT_EQ(type.at("enumOptions"), (json::array({"Directional", "Point", "Spot"})));
    EXPECT_EQ(light.at("singleton"), false);
    EXPECT_EQ(light.at("defaults").at("type"), "Directional");

    const json audio = TypeNamed("AudioSource");
    const json volume = FieldOf(audio, "_volume");
    EXPECT_EQ(volume.at("kind"), "Float");
    EXPECT_EQ(volume.at("min"), 0.0);
    EXPECT_EQ(volume.at("max"), 1.0);
    EXPECT_EQ(volume.at("displayName"), "Volume");
    EXPECT_EQ(FieldOf(audio, "_clip").at("assetType"), "AudioClip");

    EXPECT_EQ(TypeNamed("Canvas").at("singleton"), true);
    EXPECT_EQ(TypeNamed("AudioListener").at("singleton"), true);

    const json thing = TypeNamed(ThingType);
    EXPECT_EQ(FieldOf(thing, "locked").at("readOnly"), true);
    EXPECT_EQ(FieldOf(thing, "targets").at("kind"), "GameObjectRefList");
    EXPECT_EQ(FieldOf(thing, "textures").at("assetType"), "Texture");
}

TEST_F(EditorInspectorTest, TheTypesDoNotNeedAScene)
{
    EditorServer bare;
    const Frame frame = Execute(bare, CommandType::GetComponentTypes);
    EXPECT_EQ(frame.type, ComponentTypesType) << frame.Text();
}

// ==================== AddComponent ====================

TEST_F(EditorInspectorTest, AddComponentAddsANewOneWithItsDefaults)
{
    const std::string entity = Create("Lamp");
    const uint32_t before = Revision();

    json values;
    const std::string componentId = AddOk(entity, "Light", &values);
    EXPECT_TRUE(Math::UUID::FromString(componentId).has_value());
    EXPECT_EQ(values.at("uuid"), componentId);
    EXPECT_EQ(values.at("isActive"), true);
    EXPECT_EQ(values.at("type"), "Directional");
    EXPECT_EQ(values.at("intensity"), 1.0);

    // The object has it, and GetEntity and GetComponent agree with the answer
    const json components = EntityComponents(entity);
    ASSERT_EQ(components.size(), 1u);
    EXPECT_EQ(components[0].at("type"), "Light");
    EXPECT_EQ(components[0].at("uuid"), componentId);
    EXPECT_EQ(Get(entity, componentId), values);

    EXPECT_EQ(Revision(), before + 1);
    const std::vector<json> events = SceneChangedEvents(server);
    ASSERT_FALSE(events.empty());
    EXPECT_TRUE(Contains(events.back().at("entityIds"), entity));
}

TEST_F(EditorInspectorTest, ComponentsOfAnyTypeAreAddedInOrder)
{
    const std::string entity = Create("Many");
    const std::string first = AddOk(entity, "Light");
    const std::string second = AddOk(entity, "Light"); // not a singleton: a second one is fine
    const std::string third = AddOk(entity, "AudioSource");
    EXPECT_NE(first, second);

    const json components = EntityComponents(entity);
    ASSERT_EQ(components.size(), 3u);
    EXPECT_EQ(components[0].at("uuid"), first);
    EXPECT_EQ(components[1].at("uuid"), second);
    EXPECT_EQ(components[2].at("uuid"), third);
    EXPECT_EQ(components[2].at("type"), "AudioSource");
}

TEST_F(EditorInspectorTest, ASingletonTypeCanBeAddedOnlyOnce)
{
    const std::string entity = Create("Ui");
    AddOk(entity, "Canvas");
    const uint32_t before = Revision();
    ExpectError(Add(entity, "Canvas"), "only be added once");
    EXPECT_EQ(EntityComponents(entity).size(), 1u);
    EXPECT_EQ(Revision(), before);

    AddOk(entity, "AudioListener");
    ExpectError(Add(entity, "AudioListener"), "only be added once");

    // Another object may have its own
    AddOk(Create("Other Ui"), "Canvas");
}

TEST_F(EditorInspectorTest, AddComponentErrors)
{
    const std::string entity = Create("Target");
    const uint32_t before = Revision();
    ExpectError(Add(entity, "NoSuchComponent"), "Unknown component type");
    ExpectError(Add(entity, ""), "Unknown component type");
    ExpectError(Add("00000000-0000-0000-0000-000000000000", "Light"), "not found");
    ExpectError(Add("not a uuid", "Light"), "not found");
    EXPECT_TRUE(EntityComponents(entity).empty());
    EXPECT_EQ(Revision(), before) << "a refused command must not move the revision";

    // A client's text in the message has its control characters taken out
    const Frame odd = Add(entity, std::string("Bad\nType\x01"));
    ExpectError(odd, "Unknown component type");
    EXPECT_EQ(odd.Text().find('\n'), std::string::npos);
}

// ==================== RemoveComponent ====================

TEST_F(EditorInspectorTest, RemoveComponentRemovesExactlyThatOne)
{
    const std::string entity = Create("Two");
    const std::string first = AddOk(entity, "Light");
    const std::string second = AddOk(entity, "Light");
    const uint32_t before = Revision();

    const Frame removed = Remove(entity, second);
    EXPECT_EQ(removed.type, OkType) << removed.Text();

    const json components = EntityComponents(entity);
    ASSERT_EQ(components.size(), 1u);
    EXPECT_EQ(components[0].at("uuid"), first);
    EXPECT_EQ(Revision(), before + 1);
    EXPECT_TRUE(Contains(SceneChangedEvents(server).back().at("entityIds"), entity));

    // It is gone: asking for it again is an error
    ExpectError(Remove(entity, second), "not found");
    ExpectError(Execute(server, CommandType::GetComponent, Strings({entity, second})), "not found");
}

TEST_F(EditorInspectorTest, RemoveComponentErrors)
{
    const std::string entity = Create("Target");
    const std::string other = Create("Other");
    const std::string component = AddOk(other, "Light");
    const uint32_t before = Revision();

    ExpectError(Remove(entity, component), "not found"); // a component of another object
    ExpectError(Remove(entity, "not a uuid"), "not found");
    ExpectError(Remove("00000000-0000-0000-0000-000000000000", component), "not found");
    EXPECT_EQ(EntityComponents(other).size(), 1u);
    EXPECT_EQ(Revision(), before);
}

// ==================== GetComponent ====================

TEST_F(EditorInspectorTest, GetComponentReturnsTheSavedValues)
{
    const std::string entity = Create("Lamp");
    const std::string component = AddOk(entity, "Light");
    const json values = Get(entity, component);
    EXPECT_EQ(values, EntityComponents(entity)[0].at("values"));
    EXPECT_EQ(values.at("color"), (json{{"x", 1.0}, {"y", 1.0}, {"z", 1.0}}));
}

TEST_F(EditorInspectorTest, GetComponentErrors)
{
    const std::string entity = Create("Target");
    ExpectError(Execute(server, CommandType::GetComponent, Strings({entity, Math::UUID::Random().ToString()})), "not found");
    ExpectError(Execute(server, CommandType::GetComponent, Strings({"00000000-0000-0000-0000-000000000000", "x"})), "not found");
}

// ==================== SetComponentFields ====================

TEST_F(EditorInspectorTest, SetComponentFieldsStoresAndAnswersWithWhatWasStored)
{
    const std::string entity = Create("Lamp");
    const std::string component = AddOk(entity, "Light");

    const json values = Set(entity, component,
                            json{{"type", "Point"}, {"intensity", 2.5}, {"color", {{"x", 0.5}, {"y", 0.25}, {"z", 1.0}}}});
    EXPECT_EQ(values.at("type"), "Point");
    EXPECT_EQ(values.at("intensity"), 2.5);
    EXPECT_EQ(values.at("color").at("y"), 0.25);
    EXPECT_EQ(values.at("range"), 10.0); // not mentioned: unchanged
    EXPECT_EQ(Get(entity, component), values);
}

TEST_F(EditorInspectorTest, OnlyThePresentKeysAreTouched)
{
    const std::string entity = Create("Thing");
    const std::string component = AddOk(entity, ThingType);
    const std::string other = Create("Other");
    InspectorThing *thing = Thing(entity);
    ASSERT_NE(thing, nullptr);

    Set(entity, component, json{{"targets", json::array({other, nullptr})}, {"label", "kept"}});
    ASSERT_EQ(thing->targets.size(), 2u);

    // A set that doesn't mention the list leaves it as it is (loading a whole component would have cleared it)
    const json values = Set(entity, component, json{{"count", 3}});
    EXPECT_EQ(thing->targets.size(), 2u);
    EXPECT_EQ(thing->label, "kept");
    EXPECT_EQ(values.at("targets").size(), 2u);
    EXPECT_EQ(values.at("count"), 3);
}

TEST_F(EditorInspectorTest, EveryKindRoundTripsThroughASetAndAGet)
{
    const std::string entity = Create("Thing");
    const std::string component = AddOk(entity, ThingType);
    const std::string other = Create("Other");
    const RegisteredTexture texture;

    const json sent = {{"count", 7},
                       {"ratio", 0.75},
                       {"label", "hello"},
                       {"flag", true},
                       {"mode", "Fast"},
                       {"offset", {{"x", 1.0}, {"y", 2.0}, {"z", 3.0}}},
                       {"tint", {{"x", 0.5}, {"y", 0.25}, {"z", 0.125}}},
                       {"target", other},
                       {"targets", json::array({other})},
                       {"texture", texture.Id()},
                       {"textures", json::array({texture.Id(), nullptr})}};
    const json stored = Set(entity, component, sent);
    for (const auto &[key, value] : sent.items())
    {
        EXPECT_EQ(stored.at(key), value) << key;
    }
    EXPECT_EQ(Get(entity, component), stored);

    const InspectorThing *thing = Thing(entity);
    ASSERT_NE(thing, nullptr);
    EXPECT_EQ(thing->count, 7);
    EXPECT_EQ(thing->mode, InspectorMode::Fast);
    EXPECT_EQ(thing->target, Entity(other).get());
    EXPECT_EQ(thing->texture, texture.texture);
    ASSERT_EQ(thing->textures.size(), 2u);
    EXPECT_EQ(thing->textures[0], texture.texture);
    EXPECT_EQ(thing->textures[1], nullptr);
}

TEST_F(EditorInspectorTest, ReferencesCanBeClearedWithNull)
{
    const std::string entity = Create("Thing");
    const std::string component = AddOk(entity, ThingType);
    const std::string other = Create("Other");
    const RegisteredTexture texture;
    Set(entity, component, json{{"target", other}, {"texture", texture.Id()}});

    const json cleared = Set(entity, component, json{{"target", nullptr}, {"texture", nullptr}, {"targets", json::array()}});
    EXPECT_TRUE(cleared.at("target").is_null());
    EXPECT_TRUE(cleared.at("texture").is_null());
    EXPECT_TRUE(cleared.at("targets").empty());
    EXPECT_EQ(Thing(entity)->target, nullptr);
    EXPECT_EQ(Thing(entity)->texture, nullptr);
}

TEST_F(EditorInspectorTest, ARangeClampsAndTheAnswerShowsIt)
{
    const std::string entity = Create("Thing");
    const std::string component = AddOk(entity, ThingType);
    EXPECT_EQ(Set(entity, component, json{{"count", 99}}).at("count"), 10);
    EXPECT_EQ(Thing(entity)->count, 10);
    EXPECT_EQ(Set(entity, component, json{{"count", -5}}).at("count"), 0);

    const std::string lamp = Create("Lamp");
    const std::string light = AddOk(lamp, "Light");
    EXPECT_EQ(Set(lamp, light, json{{"outerConeAngle", 400.0}}).at("outerConeAngle"), 180.0);
}

TEST_F(EditorInspectorTest, AnIntegerFieldTakesAWholeFloat)
{
    const std::string entity = Create("Thing");
    const std::string component = AddOk(entity, ThingType);
    EXPECT_EQ(Set(entity, component, json{{"count", 4.0}}).at("count"), 4);
    ExpectError(SetFields(entity, component, json{{"count", 4.5}}), "count");
}

TEST_F(EditorInspectorTest, IsActiveIsASettableBaseField)
{
    const std::string entity = Create("Thing");
    const std::string component = AddOk(entity, ThingType);
    const json values = Set(entity, component, json{{"isActive", false}});
    EXPECT_EQ(values.at("isActive"), false);
    EXPECT_FALSE(Thing(entity)->IsActive());
    ExpectError(SetFields(entity, component, json{{"isActive", "no"}}), "isActive");
}

TEST_F(EditorInspectorTest, ARefusedRequestChangesNothing)
{
    const std::string entity = Create("Thing");
    const std::string component = AddOk(entity, ThingType);
    const json before = Get(entity, component);
    const uint32_t revision = Revision();
    const InspectorThing *thing = Thing(entity);

    // A valid field next to a wrong one: neither is applied
    ExpectError(SetFields(entity, component, json{{"count", 5}, {"label", 7}}), "label");
    EXPECT_EQ(thing->count, 1);
    ExpectError(SetFields(entity, component, json{{"label", "changed"}, {"mode", "Warp"}}), "Warp");
    EXPECT_EQ(thing->label, "a");
    ExpectError(SetFields(entity, component, json{{"label", "changed"}, {"nothing", 1}}), "nothing");
    EXPECT_EQ(thing->label, "a");

    EXPECT_EQ(Get(entity, component), before);
    EXPECT_EQ(Revision(), revision);
    EXPECT_EQ(thing->changedCalls, 0);
}

TEST_F(EditorInspectorTest, EachKindRefusesAValueOfTheWrongShape)
{
    const std::string entity = Create("Thing");
    const std::string component = AddOk(entity, ThingType);

    ExpectError(SetFields(entity, component, json{{"count", "three"}}), "count");
    ExpectError(SetFields(entity, component, json{{"ratio", "high"}}), "ratio");
    ExpectError(SetFields(entity, component, json{{"label", 5}}), "label");
    ExpectError(SetFields(entity, component, json{{"flag", 1}}), "flag");
    ExpectError(SetFields(entity, component, json{{"mode", "Warp"}}), "not one of");
    ExpectError(SetFields(entity, component, json{{"mode", 1}}), "mode");
    ExpectError(SetFields(entity, component, json{{"offset", 5}}), "offset");
    ExpectError(SetFields(entity, component, json{{"offset", {{"x", 1.0}}}}), "offset");
    ExpectError(SetFields(entity, component, json{{"tint", {{"r", 1.0}, {"g", 0.0}, {"b", 0.0}}}}), "tint");
    ExpectError(SetFields(entity, component, json{{"target", 12}}), "target");
    ExpectError(SetFields(entity, component, json{{"target", "not a uuid"}}), "not a UUID");
    ExpectError(SetFields(entity, component, json{{"targets", "x"}}), "targets");
    ExpectError(SetFields(entity, component, json{{"texture", false}}), "texture");
    ExpectError(SetFields(entity, component, json{{"textures", json::array({"nope"})}}), "textures");
}

TEST_F(EditorInspectorTest, UnknownAndReadOnlyFieldsAreRefused)
{
    const std::string entity = Create("Thing");
    const std::string component = AddOk(entity, ThingType);
    ExpectError(SetFields(entity, component, json{{"nothing", 1}}), "Unknown field 'nothing'");
    ExpectError(SetFields(entity, component, json{{"uuid", Math::UUID::Random().ToString()}}), "Unknown field 'uuid'");
    ExpectError(SetFields(entity, component, json{{"locked", 6}}), "read-only");
    EXPECT_EQ(Thing(entity)->locked, 5);
}

TEST_F(EditorInspectorTest, AReferenceMustNameSomethingInTheScene)
{
    const std::string entity = Create("Thing");
    const std::string component = AddOk(entity, ThingType);
    const std::string missing = Math::UUID::Random().ToString();

    ExpectError(SetFields(entity, component, json{{"target", missing}}), "isn't in the scene");
    ExpectError(SetFields(entity, component, json{{"targets", json::array({missing})}}), "isn't in the scene");
    EXPECT_EQ(Thing(entity)->target, nullptr);

    // An object destroyed since is no longer one
    const std::string doomed = Create("Doomed");
    EXPECT_EQ(Execute(server, CommandType::DestroyEntity, Strings({doomed})).type, OkType);
    ExpectError(SetFields(entity, component, json{{"target", doomed}}), "isn't in the scene");
}

TEST_F(EditorInspectorTest, AnAssetMustExistAndBeOfTheFieldsType)
{
    const std::string entity = Create("Thing");
    const std::string component = AddOk(entity, ThingType);
    const InspectorThing *thing = Thing(entity);

    ExpectError(SetFields(entity, component, json{{"texture", Math::UUID::Random().ToString()}}), "no Texture asset");
    ExpectError(SetFields(entity, component, json{{"textures", json::array({Math::UUID::Random().ToString()})}}),
                "no Texture asset");

    // A mesh is an asset, but not a Texture
    const std::string cube = Rendering::Mesh::GetBuiltin(Rendering::BuiltinMesh::Cube)->GetUUID().ToString();
    ExpectError(SetFields(entity, component, json{{"texture", cube}}), "Texture");
    EXPECT_EQ(thing->texture, nullptr);

    // The same request that is right for a Mesh field is accepted
    const std::string rendererEntity = Create("Mesh");
    const std::string renderer = AddOk(rendererEntity, "MeshRenderer");
    EXPECT_EQ(Set(rendererEntity, renderer, json{{"_mesh", cube}}).at("_mesh"), cube);
}

TEST_F(EditorInspectorTest, OnEditorFieldsChangedGetsTheKeysThatChanged)
{
    const std::string entity = Create("Thing");
    const std::string component = AddOk(entity, ThingType);
    InspectorThing *thing = Thing(entity);

    Set(entity, component, json{{"count", 4}, {"label", "a"}}); // the label is what it was
    EXPECT_EQ(thing->changedCalls, 1);
    EXPECT_EQ(thing->lastChanged, (std::vector<std::string>{"count"}));

    Set(entity, component, json{{"count", 5}, {"label", "b"}});
    EXPECT_EQ(thing->changedCalls, 2);
    EXPECT_EQ(thing->lastChanged.size(), 2u);
}

TEST_F(EditorInspectorTest, SettingWhatIsAlreadyStoredChangesNothing)
{
    const std::string entity = Create("Thing");
    const std::string component = AddOk(entity, ThingType);
    InspectorThing *thing = Thing(entity);
    const uint32_t revision = Revision();
    const size_t events = SceneChangedEvents(server).size();

    Set(entity, component, json{{"count", 1}, {"label", "a"}});
    Set(entity, component, json::object());
    EXPECT_EQ(Revision(), revision);
    EXPECT_EQ(SceneChangedEvents(server).size(), events);
    EXPECT_EQ(thing->changedCalls, 0);

    // Clamped to what is stored already: also nothing
    Set(entity, component, json{{"count", 99}});
    EXPECT_EQ(Revision(), revision + 1);
    Set(entity, component, json{{"count", 500}});
    EXPECT_EQ(Revision(), revision + 1);
    EXPECT_EQ(thing->changedCalls, 1);
}

TEST_F(EditorInspectorTest, ASetMovesTheRevisionOnceAndNamesTheObject)
{
    const std::string entity = Create("Thing");
    const std::string component = AddOk(entity, ThingType);
    const uint32_t revision = Revision();

    Set(entity, component, json{{"count", 2}, {"label", "x"}, {"flag", true}});
    EXPECT_EQ(Revision(), revision + 1);
    const std::vector<json> events = SceneChangedEvents(server);
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(events.back().at("revision"), revision + 1);
    EXPECT_TRUE(Contains(events.back().at("entityIds"), entity));
}

TEST_F(EditorInspectorTest, SetComponentFieldsErrors)
{
    const std::string entity = Create("Thing");
    const std::string component = AddOk(entity, ThingType);

    ExpectError(SetFields(entity, Math::UUID::Random().ToString(), json{{"count", 1}}), "not found");
    ExpectError(SetFields(entity, "not a uuid", json{{"count", 1}}), "not found");
    ExpectError(SetFields("00000000-0000-0000-0000-000000000000", component, json{{"count", 1}}), "not found");
    ExpectError(SetFields(entity, component, json::array({1, 2})), "JSON object");
    ExpectError(SetFields(entity, component, json(5)), "JSON object");
    ExpectError(SetFields(entity, component, nullptr), "JSON object");

    // Text that isn't JSON: the command fails as a whole
    BufferWriter w;
    w.WriteString(entity);
    w.WriteString(component);
    w.WriteString("{not json");
    EXPECT_EQ(Execute(server, CommandType::SetComponentFields, w.Release()).type, ErrorType);
}

TEST_F(EditorInspectorTest, AComponentTypeThatIsntRegisteredCantBeEdited)
{
    // A component the scene has but the registry can't make a copy of
    class Unregistered final : public Component
    {
    public:
        explicit Unregistered(GameObject &gameObject) : Component(gameObject) {}
        [[nodiscard]] std::string GetTypeName() const override { return "EditorInspectorTest_Unregistered"; }
    };
    const std::string entity = Create("Odd");
    Component *added = Entity(entity)->AddComponent<Unregistered>();
    ASSERT_NE(added, nullptr);
    const std::string component = added->GetUUID().ToString();

    ExpectError(SetFields(entity, component, json{{"isActive", false}}), "isn't registered");
    // It is still visible
    EXPECT_EQ(Get(entity, component).at("isActive"), true);
}

TEST_F(EditorInspectorTest, EditsOfLightsAndRigidbodiesReachTheComponent)
{
    const std::string entity = Create("Body");
    const std::string body = AddOk(entity, "Rigidbody");
    const json values = Set(entity, body, json{{"_bodyType", "Kinematic"}, {"_mass", 3.0}, {"_gravityEnabled", false}});
    EXPECT_EQ(values.at("_bodyType"), "Kinematic");
    EXPECT_EQ(values.at("_mass"), 3.0);
    EXPECT_EQ(values.at("_gravityEnabled"), false);
    ExpectError(SetFields(entity, body, json{{"_bodyType", "Floating"}}), "Static, Dynamic, Kinematic");
}

// ==================== GetLuaFields ====================

TEST_F(EditorInspectorTest, ALuaComponentWithoutAScriptOffersTheScriptField)
{
    const std::string entity = Create("Scripted");
    const std::string component = AddOk(entity, "LuaComponent");

    const Frame frame = Execute(server, CommandType::GetLuaFields, Strings({entity, component}));
    ASSERT_EQ(frame.type, LuaFieldsType) << frame.Text();
    BufferReader r(frame.payload);
    const json schema = ReadJson(r);
    EXPECT_FALSE(r.HasData());

    EXPECT_EQ(schema.at("typeName"), "LuaComponent");
    EXPECT_FALSE(schema.contains("defaults"));
    ASSERT_EQ(schema.at("fields").size(), 1u);
    const json &script = schema.at("fields")[0];
    EXPECT_EQ(script.at("name"), "scriptUUID");
    EXPECT_EQ(script.at("kind"), "AssetRef");
    EXPECT_EQ(script.at("assetType"), "LuaScript");
}

TEST_F(EditorInspectorTest, GetLuaFieldsErrors)
{
    const std::string entity = Create("Lamp");
    const std::string light = AddOk(entity, "Light");
    ExpectError(Execute(server, CommandType::GetLuaFields, Strings({entity, light})), "not a LuaComponent");
    ExpectError(Execute(server, CommandType::GetLuaFields, Strings({entity, Math::UUID::Random().ToString()})), "not found");
    ExpectError(Execute(server, CommandType::GetLuaFields, Strings({"00000000-0000-0000-0000-000000000000", light})),
                "not found");
}

TEST_F(EditorInspectorTest, ALuaComponentsScriptMustBeAScriptTheProjectHas)
{
    const std::string entity = Create("Scripted");
    const std::string component = AddOk(entity, "LuaComponent");
    ExpectError(SetFields(entity, component, json{{"scriptUUID", Math::UUID::Random().ToString()}}), "Script not found");
    ExpectError(SetFields(entity, component, json{{"scriptUUID", nullptr}}), "needs a script");
    ExpectError(SetFields(entity, component, json{{"scriptData", {{"speed", 1}}}}), "scriptData"); // no script declares it
    EXPECT_TRUE(Get(entity, component).at("scriptData").empty());
}
