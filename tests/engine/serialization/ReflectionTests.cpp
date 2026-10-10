#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <math/UUID.hpp>
#include <math/Vector3.hpp>
#include <nlohmann/json.hpp>

#include "engine/Component.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/audio/AudioClip.hpp"
#include "engine/physics/Rigidbody.hpp"
#include "engine/rendering/Light.hpp"
#include "engine/rendering/Material.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/Model.hpp"
#include "engine/rendering/Texture.hpp"
#include "engine/rendering/TextRenderer.hpp"
#include "engine/sceneManagement/SceneFile.hpp"
#include "engine/scripting/LuaScript.hpp"
#include "engine/serialization/ComponentRegistry.hpp"
#include "engine/serialization/ComponentSerializer.hpp"
#include "engine/serialization/FieldInfo.hpp"
#include "engine/serialization/ReferenceResolver.hpp"
#include "engine/text/Font.hpp"
#include "engine/ui/UIText.hpp"

// The reflection API (#77, E5): the FieldInfo each RegisterMember builds from the member's type, the opt-in
// FieldBuilder calls, N2_SERIALIZE_ENUM, the checks and clamping the inspector applies, SetEditorFields (which sets
// only the members whose keys are present) and ComponentRegistry::Describe

using namespace N2Engine;
using json = nlohmann::json;

namespace
{
    enum class ReflectMode
    {
        Fast,
        Slow,
        Off
    };

    N2_SERIALIZE_ENUM(ReflectMode, {
                      { ReflectMode::Fast, "Fast" },
                      { ReflectMode::Slow, "Slow" },
                      { ReflectMode::Off, "Off" }
                      })

    /// An enum registered the old way: it saves by name but lists no options, so an editor shows it as JSON
    enum class PlainMode
    {
        One,
        Two
    };

    NLOHMANN_JSON_SERIALIZE_ENUM(PlainMode, {
                                 { PlainMode::One, "One" },
                                 { PlainMode::Two, "Two" }
                                 })

    class ReflectedThing final : public SerializableComponent
    {
    public:
        explicit ReflectedThing(GameObject &gameObject) : SerializableComponent(gameObject)
        {
            // A builder taken first stays good through any number of registrations after it
            FieldBuilder first = RegisterMember("first", firstValue);
            for (std::size_t i = 0; i < filler.size(); ++i)
            {
                RegisterMember("filler" + std::to_string(i), filler[i]).Hidden();
            }
            first.Range(1.0, 2.0).Tooltip("Registered first");

            RegisterMember("_playOnAwake", playOnAwake);
            RegisterMember("count", count).Range(0.0, 10.0).Tooltip("How many");
            RegisterMember("ratio", ratio).DisplayName("The Ratio");
            RegisterMember("label", label);
            RegisterMember("offset", offset);
            RegisterMember("tint", tint).AsColor();
            RegisterMember("mode", mode);
            RegisterMember("plain", plain);
            RegisterMember("locked", locked).ReadOnly();
            RegisterMember("flagged", flagged).AsColor(); // not a vector: the hint is ignored
            RegisterMember("unsignedCount", unsignedCount);
            RegisterMember("wide", wide);
            RegisterGameObjectRef("target", target);
            RegisterGameObjectRefVector("targets", targets);
            RegisterAssetRef("texture", texture);
            RegisterAssetRefList("textures", textures);
        }

        [[nodiscard]] std::string GetTypeName() const override { return "ReflectionTest_Thing"; }

        int firstValue = 1;
        std::array<int, 24> filler{};
        bool playOnAwake = true;
        int count = 1;
        float ratio = 0.5f;
        std::string label = "a";
        Math::Vector3 offset = {0.0f, 0.0f, 0.0f};
        Math::Vector3 tint = {1.0f, 1.0f, 1.0f};
        ReflectMode mode = ReflectMode::Slow;
        PlainMode plain = PlainMode::One;
        int locked = 5;
        bool flagged = false;
        std::uint32_t unsignedCount = 0;
        double wide = 0.0;
        GameObject *target = nullptr;
        std::vector<GameObject *> targets;
        std::shared_ptr<Rendering::Texture> texture;
        std::vector<std::shared_ptr<Rendering::Texture>> textures;
    };

    /// A component with no members at all
    class BareComponent final : public Component
    {
    public:
        explicit BareComponent(GameObject &gameObject) : Component(gameObject) {}
        [[nodiscard]] std::string GetTypeName() const override { return "ReflectionTest_Bare"; }
    };

    /// A type that can be on an object only once
    class LonelyComponent final : public Component
    {
    public:
        explicit LonelyComponent(GameObject &gameObject) : Component(gameObject) {}
        [[nodiscard]] std::string GetTypeName() const override { return "ReflectionTest_Lonely"; }
        static constexpr bool IsSingleton = true;
    };

    /// Points at other components, which must not dangle when they are removed
    class RefHolder final : public SerializableComponent
    {
    public:
        explicit RefHolder(GameObject &gameObject) : SerializableComponent(gameObject)
        {
            RegisterComponentRef("thing", thing);
            RegisterComponentRefVector("things", things);
        }
        [[nodiscard]] std::string GetTypeName() const override { return "ReflectionTest_RefHolder"; }

        ReflectedThing *thing = nullptr;
        std::vector<ReflectedThing *> things;
    };

    /// Counts the OnDisable and OnDestroy calls it gets
    class CallbackProbe final : public Component
    {
    public:
        explicit CallbackProbe(GameObject &gameObject) : Component(gameObject) {}
        [[nodiscard]] std::string GetTypeName() const override { return "ReflectionTest_Probe"; }
        void OnDisable() override { ++disables; }
        void OnDestroy() override { ++destroys; }
        static inline int disables = 0;
        static inline int destroys = 0;
    };

    const FieldInfo *Find(const std::vector<FieldInfo> &fields, const std::string &name)
    {
        const auto found = std::ranges::find(fields, name, &FieldInfo::name);
        return found == fields.end() ? nullptr : &*found;
    }

    FieldInfo Field(FieldKind kind, std::string name = "field")
    {
        FieldInfo info;
        info.name = std::move(name);
        info.kind = kind;
        return info;
    }
}

// ==================== FieldTraits and N2_SERIALIZE_ENUM ====================

TEST(ReflectionTest, FieldTraitsMapTypesToKinds)
{
    EXPECT_EQ(FieldTraits<bool>::kind, FieldKind::Bool);
    EXPECT_EQ(FieldTraits<int>::kind, FieldKind::Int);
    EXPECT_EQ(FieldTraits<std::uint32_t>::kind, FieldKind::Int);
    EXPECT_EQ(FieldTraits<float>::kind, FieldKind::Float);
    EXPECT_EQ(FieldTraits<double>::kind, FieldKind::Float);
    EXPECT_EQ(FieldTraits<std::string>::kind, FieldKind::String);
    EXPECT_EQ(FieldTraits<Math::Vector2>::kind, FieldKind::Vector2);
    EXPECT_EQ(FieldTraits<Math::Vector3>::kind, FieldKind::Vector3);
    EXPECT_EQ(FieldTraits<Math::Vector4>::kind, FieldKind::Vector4);
    EXPECT_EQ(FieldTraits<Math::Quaternion>::kind, FieldKind::Quaternion);
    EXPECT_EQ(FieldTraits<Common::Color>::kind, FieldKind::Color);
    EXPECT_EQ(FieldTraits<std::vector<int>>::kind, FieldKind::Json);

    EXPECT_EQ(FieldTraits<bool>::TypeName(), "bool");
    EXPECT_EQ(FieldTraits<int>::TypeName(), "int");
    EXPECT_EQ(FieldTraits<float>::TypeName(), "float");
    EXPECT_EQ(FieldTraits<Math::Vector3>::TypeName(), "Vector3");
    EXPECT_EQ(FieldTraits<Common::Color>::TypeName(), "Color");
}

TEST(ReflectionTest, AnEnumRegisteredWithN2SerializeEnumListsItsOptionsInOrder)
{
    EXPECT_EQ(FieldTraits<ReflectMode>::kind, FieldKind::Enum);
    EXPECT_EQ(FieldTraits<ReflectMode>::TypeName(), "ReflectMode");
    const std::vector<std::string> expected = {"Fast", "Slow", "Off"};
    EXPECT_EQ(FieldTraits<ReflectMode>::EnumOptions(), expected);

    // It still saves and loads by name, like NLOHMANN_JSON_SERIALIZE_ENUM alone
    EXPECT_EQ(json(ReflectMode::Off), "Off");
    EXPECT_EQ(json("Slow").get<ReflectMode>(), ReflectMode::Slow);
}

TEST(ReflectionTest, AnEnumWithoutOptionsIsJson)
{
    EXPECT_EQ(FieldTraits<PlainMode>::kind, FieldKind::Json);
    EXPECT_TRUE(FieldTraits<PlainMode>::EnumOptions().empty());
}

TEST(ReflectionTest, TheComponentEnumsListTheirOptions)
{
    const std::vector<std::string> lights = {"Directional", "Point", "Spot"};
    EXPECT_EQ(FieldTraits<Rendering::LightType>::EnumOptions(), lights);
    EXPECT_EQ(FieldTraits<Rendering::LightType>::TypeName(), "LightType");

    const std::vector<std::string> bodies = {"Static", "Dynamic", "Kinematic"};
    EXPECT_EQ(FieldTraits<Physics::BodyType>::EnumOptions(), bodies);
}

TEST(ReflectionTest, DisplayNamesComeFromTheKey)
{
    EXPECT_EQ(DefaultDisplayName("_playOnAwake"), "Play On Awake");
    EXPECT_EQ(DefaultDisplayName("sortOrder"), "Sort Order");
    EXPECT_EQ(DefaultDisplayName("UIText"), "UI Text");
    EXPECT_EQ(DefaultDisplayName("color"), "Color");
    EXPECT_EQ(DefaultDisplayName("_x"), "X");
    EXPECT_EQ(DefaultDisplayName("snake_case_key"), "Snake Case Key");
    EXPECT_EQ(DefaultDisplayName(""), "");
    EXPECT_EQ(DefaultDisplayName("___"), "");
}

// ==================== RegisterMember and FieldBuilder ====================

TEST(ReflectionTest, RegisteredMembersDescribeThemselvesFromTheirTypes)
{
    const auto go = GameObject::Create("Thing");
    const auto *thing = go->AddComponent<ReflectedThing>();
    const std::vector<FieldInfo> fields = thing->DescribeFields();

    const auto kindOf = [&fields](const std::string &name)
    {
        const FieldInfo *field = Find(fields, name);
        return field != nullptr ? std::optional<FieldKind>(field->kind) : std::nullopt;
    };
    EXPECT_EQ(kindOf("_playOnAwake"), FieldKind::Bool);
    EXPECT_EQ(kindOf("count"), FieldKind::Int);
    EXPECT_EQ(kindOf("ratio"), FieldKind::Float);
    EXPECT_EQ(kindOf("label"), FieldKind::String);
    EXPECT_EQ(kindOf("offset"), FieldKind::Vector3);
    EXPECT_EQ(kindOf("mode"), FieldKind::Enum);
    EXPECT_EQ(kindOf("plain"), FieldKind::Json);
    EXPECT_EQ(kindOf("target"), FieldKind::GameObjectRef);
    EXPECT_EQ(kindOf("targets"), FieldKind::GameObjectRefList);
    EXPECT_EQ(kindOf("texture"), FieldKind::AssetRef);
    EXPECT_EQ(kindOf("textures"), FieldKind::AssetRefList);

    const FieldInfo *mode = Find(fields, "mode");
    ASSERT_NE(mode, nullptr);
    EXPECT_EQ(mode->typeName, "ReflectMode");
    EXPECT_EQ(mode->enumOptions, (std::vector<std::string>{"Fast", "Slow", "Off"}));

    const FieldInfo *texture = Find(fields, "texture");
    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(texture->assetType, "Texture");
    EXPECT_EQ(Find(fields, "textures")->assetType, "Texture");
}

TEST(ReflectionTest, FieldsAreListedInRegistrationOrder)
{
    const auto go = GameObject::Create("Thing");
    const std::vector<FieldInfo> fields = go->AddComponent<ReflectedThing>()->DescribeFields();
    ASSERT_GE(fields.size(), 3u);
    EXPECT_EQ(fields.front().name, "first");
    EXPECT_EQ(fields.back().name, "textures");
}

TEST(ReflectionTest, TheOptInCallsRefineTheField)
{
    const auto go = GameObject::Create("Thing");
    const std::vector<FieldInfo> fields = go->AddComponent<ReflectedThing>()->DescribeFields();

    const FieldInfo *count = Find(fields, "count");
    ASSERT_NE(count, nullptr);
    ASSERT_TRUE(count->range.has_value());
    EXPECT_DOUBLE_EQ(count->range->first, 0.0);
    EXPECT_DOUBLE_EQ(count->range->second, 10.0);
    EXPECT_EQ(count->tooltip, "How many");

    EXPECT_EQ(Find(fields, "ratio")->displayName, "The Ratio");
    EXPECT_EQ(Find(fields, "_playOnAwake")->displayName, "Play On Awake");
    EXPECT_TRUE(Find(fields, "locked")->readOnly);
    EXPECT_FALSE(Find(fields, "count")->readOnly);
    EXPECT_TRUE(Find(fields, "filler3")->hidden);
}

TEST(ReflectionTest, ABuilderStaysValidAfterMoreMembersAreRegistered)
{
    // "first" was registered before 24 others (enough to move the member list), and configured after them
    const auto go = GameObject::Create("Thing");
    const std::vector<FieldInfo> fields = go->AddComponent<ReflectedThing>()->DescribeFields();
    const FieldInfo *first = Find(fields, "first");
    ASSERT_NE(first, nullptr);
    ASSERT_TRUE(first->range.has_value());
    EXPECT_DOUBLE_EQ(first->range->first, 1.0);
    EXPECT_DOUBLE_EQ(first->range->second, 2.0);
    EXPECT_EQ(first->tooltip, "Registered first");
    // and the member it names, not a neighbour, was configured
    EXPECT_FALSE(Find(fields, "filler0")->range.has_value());
}

TEST(ReflectionTest, AsColorMarksAVectorAndKeepsItsJson)
{
    const auto go = GameObject::Create("Thing");
    auto *thing = go->AddComponent<ReflectedThing>();
    const std::vector<FieldInfo> fields = thing->DescribeFields();

    const FieldInfo *tint = Find(fields, "tint");
    ASSERT_NE(tint, nullptr);
    EXPECT_EQ(tint->kind, FieldKind::Color);
    EXPECT_EQ(tint->typeName, "Vector3");
    // The JSON is still {x,y,z}: a scene saved before the hint loads as it did
    const json saved = thing->Serialize();
    EXPECT_TRUE(saved.at("tint").contains("x"));
    EXPECT_FALSE(saved.at("tint").contains("r"));

    // An offset that isn't marked is still a vector, and a bool can't be a colour
    EXPECT_EQ(Find(fields, "offset")->kind, FieldKind::Vector3);
    EXPECT_EQ(Find(fields, "flagged")->kind, FieldKind::Bool);
}

TEST(ReflectionTest, TheFieldsOfAComponentWithoutMembersAreNone)
{
    const auto go = GameObject::Create("Bare");
    EXPECT_TRUE(go->AddComponent<BareComponent>()->DescribeFields().empty());
}

TEST(ReflectionTest, FieldInfoJsonLeavesOutWhatIsEmpty)
{
    FieldInfo plain = Field(FieldKind::Float, "speed");
    plain.displayName = "Speed";
    plain.typeName = "float";
    const json j = plain.ToJson();
    EXPECT_EQ(j.at("name"), "speed");
    EXPECT_EQ(j.at("kind"), "Float");
    EXPECT_EQ(j.at("hidden"), false);
    EXPECT_FALSE(j.contains("enumOptions"));
    EXPECT_FALSE(j.contains("assetType"));
    EXPECT_FALSE(j.contains("min"));
    EXPECT_FALSE(j.contains("tooltip"));
    EXPECT_FALSE(j.contains("container"));

    plain.range = std::make_pair(0.0, 1.0);
    plain.tooltip = "tip";
    plain.container = "scriptData";
    const json full = plain.ToJson();
    EXPECT_EQ(full.at("min"), 0.0);
    EXPECT_EQ(full.at("max"), 1.0);
    EXPECT_EQ(full.at("tooltip"), "tip");
    EXPECT_EQ(full.at("container"), "scriptData");
}

// ==================== ValidateFieldValue and ClampFieldValue ====================

TEST(ReflectionTest, ScalarKindsAcceptOnlyTheirJsonType)
{
    EXPECT_FALSE(ValidateFieldValue(Field(FieldKind::Bool), true).has_value());
    EXPECT_TRUE(ValidateFieldValue(Field(FieldKind::Bool), "true").has_value());
    EXPECT_TRUE(ValidateFieldValue(Field(FieldKind::Bool), 1).has_value());

    EXPECT_FALSE(ValidateFieldValue(Field(FieldKind::Int), 3).has_value());
    EXPECT_FALSE(ValidateFieldValue(Field(FieldKind::Int), 3.0).has_value()); // a client may write 3 as 3.0
    EXPECT_TRUE(ValidateFieldValue(Field(FieldKind::Int), 3.5).has_value());
    EXPECT_TRUE(ValidateFieldValue(Field(FieldKind::Int), "3").has_value());
    EXPECT_TRUE(ValidateFieldValue(Field(FieldKind::Int), nullptr).has_value());

    EXPECT_FALSE(ValidateFieldValue(Field(FieldKind::Float), 3).has_value());
    EXPECT_FALSE(ValidateFieldValue(Field(FieldKind::Float), 0.25).has_value());
    EXPECT_TRUE(ValidateFieldValue(Field(FieldKind::Float), "0.25").has_value());

    EXPECT_FALSE(ValidateFieldValue(Field(FieldKind::String), "x").has_value());
    EXPECT_TRUE(ValidateFieldValue(Field(FieldKind::String), 5).has_value());
}

TEST(ReflectionTest, TheMessageNamesTheField)
{
    const auto problem = ValidateFieldValue(Field(FieldKind::Float, "volume"), "loud");
    ASSERT_TRUE(problem.has_value());
    EXPECT_NE(problem->find("volume"), std::string::npos) << *problem;
    EXPECT_NE(problem->find("number"), std::string::npos) << *problem;
}

TEST(ReflectionTest, VectorKindsNeedEveryAxisAsANumber)
{
    EXPECT_FALSE(ValidateFieldValue(Field(FieldKind::Vector2), json{{"x", 1}, {"y", 2}}).has_value());
    EXPECT_TRUE(ValidateFieldValue(Field(FieldKind::Vector2), json{{"x", 1}}).has_value());
    EXPECT_FALSE(ValidateFieldValue(Field(FieldKind::Vector3), json{{"x", 1}, {"y", 2}, {"z", 3}}).has_value());
    EXPECT_TRUE(ValidateFieldValue(Field(FieldKind::Vector3), json{{"x", 1}, {"y", 2}, {"z", "3"}}).has_value());
    EXPECT_TRUE(ValidateFieldValue(Field(FieldKind::Vector3), 5).has_value());
    EXPECT_TRUE(ValidateFieldValue(Field(FieldKind::Vector3), nullptr).has_value());
    EXPECT_FALSE(ValidateFieldValue(Field(FieldKind::Vector4), json{{"w", 1}, {"x", 1}, {"y", 2}, {"z", 3}}).has_value());
    EXPECT_TRUE(ValidateFieldValue(Field(FieldKind::Vector4), json{{"x", 1}, {"y", 2}, {"z", 3}}).has_value());
    EXPECT_FALSE(ValidateFieldValue(Field(FieldKind::Quaternion), json{{"w", 1}, {"x", 0}, {"y", 0}, {"z", 0}}).has_value());
    EXPECT_TRUE(ValidateFieldValue(Field(FieldKind::Quaternion), json{{"x", 0}, {"y", 0}, {"z", 0}}).has_value());
}

TEST(ReflectionTest, ColorShapesFollowTheTypeName)
{
    FieldInfo color = Field(FieldKind::Color);
    color.typeName = "Color";
    EXPECT_FALSE(ValidateFieldValue(color, json{{"r", 1}, {"g", 0}, {"b", 0}, {"a", 1}}).has_value());
    EXPECT_FALSE(ValidateFieldValue(color, json{{"r", 1}, {"g", 0}, {"b", 0}}).has_value()); // alpha is optional
    EXPECT_TRUE(ValidateFieldValue(color, json{{"r", 1}, {"g", 0}}).has_value());
    EXPECT_TRUE(ValidateFieldValue(color, json{{"r", 1}, {"g", 0}, {"b", 0}, {"a", "x"}}).has_value());
    EXPECT_TRUE(ValidateFieldValue(color, json{{"x", 1}, {"y", 0}, {"z", 0}}).has_value());

    FieldInfo vector3Color = Field(FieldKind::Color);
    vector3Color.typeName = "Vector3";
    EXPECT_FALSE(ValidateFieldValue(vector3Color, json{{"x", 1}, {"y", 0}, {"z", 0}}).has_value());
    EXPECT_TRUE(ValidateFieldValue(vector3Color, json{{"r", 1}, {"g", 0}, {"b", 0}}).has_value());
}

TEST(ReflectionTest, AnEnumValueMustBeOneOfTheOptions)
{
    FieldInfo mode = Field(FieldKind::Enum, "mode");
    mode.enumOptions = {"Fast", "Slow"};
    EXPECT_FALSE(ValidateFieldValue(mode, "Fast").has_value());
    const auto unknown = ValidateFieldValue(mode, "Warp");
    ASSERT_TRUE(unknown.has_value());
    EXPECT_NE(unknown->find("Warp"), std::string::npos) << *unknown;
    EXPECT_NE(unknown->find("Fast, Slow"), std::string::npos) << *unknown;
    EXPECT_TRUE(ValidateFieldValue(mode, "fast").has_value()); // case matters
    EXPECT_TRUE(ValidateFieldValue(mode, 0).has_value());
}

TEST(ReflectionTest, ReferencesAreUuidStringsOrNull)
{
    const std::string id = Math::UUID::Random().ToString();
    for (const FieldKind kind : {FieldKind::AssetRef, FieldKind::GameObjectRef, FieldKind::ComponentRef})
    {
        EXPECT_FALSE(ValidateFieldValue(Field(kind), id).has_value());
        EXPECT_FALSE(ValidateFieldValue(Field(kind), nullptr).has_value());
        EXPECT_TRUE(ValidateFieldValue(Field(kind), "not a uuid").has_value());
        EXPECT_TRUE(ValidateFieldValue(Field(kind), 12).has_value());
        EXPECT_TRUE(ValidateFieldValue(Field(kind), json::array()).has_value());
    }
    for (const FieldKind kind : {FieldKind::AssetRefList, FieldKind::GameObjectRefList, FieldKind::ComponentRefList})
    {
        EXPECT_FALSE(ValidateFieldValue(Field(kind), json::array()).has_value());
        EXPECT_FALSE(ValidateFieldValue(Field(kind), json::array({id, nullptr})).has_value());
        EXPECT_TRUE(ValidateFieldValue(Field(kind), json::array({id, "nope"})).has_value());
        EXPECT_TRUE(ValidateFieldValue(Field(kind), id).has_value()); // not an array
    }
}

TEST(ReflectionTest, AReferenceInAContainerIsWrapped)
{
    FieldInfo target = Field(FieldKind::GameObjectRef, "target");
    target.container = "scriptData";
    const std::string id = Math::UUID::Random().ToString();
    EXPECT_FALSE(ValidateFieldValue(target, json{{"$ref", id}}).has_value());
    EXPECT_FALSE(ValidateFieldValue(target, json{{"$ref", nullptr}}).has_value());
    EXPECT_TRUE(ValidateFieldValue(target, id).has_value()); // the bare form is for top-level fields
    EXPECT_TRUE(ValidateFieldValue(target, json{{"$ref", "nope"}}).has_value());
}

TEST(ReflectionTest, JsonFieldsAcceptAnything)
{
    for (const json &value : {json(1), json("x"), json(nullptr), json::array(), json::object()})
    {
        EXPECT_FALSE(ValidateFieldValue(Field(FieldKind::Json), value).has_value()) << value.dump();
    }
}

TEST(ReflectionTest, ClampingFollowsTheRange)
{
    FieldInfo ratio = Field(FieldKind::Float);
    ratio.range = std::make_pair(0.0, 1.0);
    EXPECT_EQ(ClampFieldValue(ratio, 5), json(1.0));
    EXPECT_EQ(ClampFieldValue(ratio, -2.5), json(0.0));
    EXPECT_EQ(ClampFieldValue(ratio, 0.25), json(0.25));

    FieldInfo count = Field(FieldKind::Int);
    count.range = std::make_pair(0.0, 10.0);
    const json clamped = ClampFieldValue(count, 99);
    EXPECT_TRUE(clamped.is_number_integer()) << clamped.dump();
    EXPECT_EQ(clamped, 10);
    EXPECT_EQ(ClampFieldValue(count, -4), 0);

    // No range, or not a number: unchanged
    EXPECT_EQ(ClampFieldValue(Field(FieldKind::Float), 99), 99);
    EXPECT_EQ(ClampFieldValue(ratio, "text"), "text");
}

TEST(ReflectionTest, ValueObjectsAreCheckedKeyByKey)
{
    const auto go = GameObject::Create("Thing");
    const std::vector<FieldInfo> fields = go->AddComponent<ReflectedThing>()->DescribeFields();

    EXPECT_FALSE(ValidateFieldValues(fields, json{{"count", 3}, {"label", "x"}, {"isActive", false}}).has_value());
    EXPECT_FALSE(ValidateFieldValues(fields, json::object()).has_value());

    const auto unknown = ValidateFieldValues(fields, json{{"count", 3}, {"nothing", 1}});
    ASSERT_TRUE(unknown.has_value());
    EXPECT_NE(unknown->find("nothing"), std::string::npos) << *unknown;

    const auto wrong = ValidateFieldValues(fields, json{{"count", "three"}});
    ASSERT_TRUE(wrong.has_value());
    EXPECT_NE(wrong->find("count"), std::string::npos) << *wrong;

    const auto locked = ValidateFieldValues(fields, json{{"locked", 6}});
    ASSERT_TRUE(locked.has_value());
    EXPECT_NE(locked->find("read-only"), std::string::npos) << *locked;

    EXPECT_TRUE(ValidateFieldValues(fields, json{{"isActive", 1}}).has_value());
    EXPECT_TRUE(ValidateFieldValues(fields, json::array()).has_value());
    EXPECT_TRUE(ValidateFieldValues(fields, json{{"uuid", Math::UUID::Random().ToString()}}).has_value());
}

TEST(ReflectionTest, ContainerFieldsAreCheckedInsideTheirObject)
{
    FieldInfo speed = Field(FieldKind::Float, "speed");
    speed.container = "scriptData";
    const std::vector<FieldInfo> fields = {speed};

    EXPECT_FALSE(ValidateFieldValues(fields, json{{"scriptData", {{"speed", 3}}}}).has_value());
    EXPECT_TRUE(ValidateFieldValues(fields, json{{"scriptData", {{"speed", "fast"}}}}).has_value());
    EXPECT_TRUE(ValidateFieldValues(fields, json{{"scriptData", {{"other", 1}}}}).has_value());
    EXPECT_TRUE(ValidateFieldValues(fields, json{{"scriptData", 5}}).has_value());
    EXPECT_TRUE(ValidateFieldValues(fields, json{{"speed", 3}}).has_value()); // it isn't a top-level key
}

// ==================== SetEditorFields ====================

TEST(ReflectionTest, SetEditorFieldsSetsOnlyThePresentKeys)
{
    const auto go = GameObject::Create("Thing");
    auto *thing = go->AddComponent<ReflectedThing>();
    thing->label = "keep";

    thing->SetEditorFields(json{{"count", 4}, {"ratio", 0.75}}, nullptr);
    EXPECT_EQ(thing->count, 4);
    EXPECT_FLOAT_EQ(thing->ratio, 0.75f);
    EXPECT_EQ(thing->label, "keep");
    EXPECT_EQ(thing->mode, ReflectMode::Slow);
}

TEST(ReflectionTest, SetEditorFieldsKeepsAListWhoseKeyIsAbsent)
{
    const auto go = GameObject::Create("Thing");
    const auto other = GameObject::Create("Other");
    auto *thing = go->AddComponent<ReflectedThing>();
    thing->targets = {other.get(), nullptr, other.get()};

    thing->SetEditorFields(json{{"count", 2}}, nullptr);
    EXPECT_EQ(thing->targets.size(), 3u);

    // What loading a whole component does with the same input, and why the editor can't use it for a partial set
    thing->Deserialize(json{{"count", 2}}, nullptr);
    EXPECT_TRUE(thing->targets.empty());
}

TEST(ReflectionTest, SetEditorFieldsClampsAFieldWithARange)
{
    const auto go = GameObject::Create("Thing");
    auto *thing = go->AddComponent<ReflectedThing>();
    thing->SetEditorFields(json{{"count", 500}}, nullptr);
    EXPECT_EQ(thing->count, 10);
    thing->SetEditorFields(json{{"count", -3}}, nullptr);
    EXPECT_EQ(thing->count, 0);
    // Loading doesn't clamp: a scene is trusted to hold what it saved
    thing->Deserialize(json{{"count", 500}}, nullptr);
    EXPECT_EQ(thing->count, 500);
}

TEST(ReflectionTest, SetEditorFieldsReadsEveryKindThroughTheMembersOwnJson)
{
    const auto go = GameObject::Create("Thing");
    auto *thing = go->AddComponent<ReflectedThing>();
    thing->SetEditorFields(json{{"_playOnAwake", false},
                                {"label", "hello"},
                                {"offset", {{"x", 1.0}, {"y", 2.0}, {"z", 3.0}}},
                                {"tint", {{"x", 0.5}, {"y", 0.25}, {"z", 0.125}}},
                                {"mode", "Off"}},
                           nullptr);
    EXPECT_FALSE(thing->playOnAwake);
    EXPECT_EQ(thing->label, "hello");
    EXPECT_FLOAT_EQ(thing->offset.y, 2.0f);
    EXPECT_FLOAT_EQ(thing->tint.z, 0.125f);
    EXPECT_EQ(thing->mode, ReflectMode::Off);
}

TEST(ReflectionTest, SetEditorFieldsResolvesReferencesThroughTheResolver)
{
    const auto go = GameObject::Create("Thing");
    const auto other = GameObject::Create("Other");
    auto *thing = go->AddComponent<ReflectedThing>();

    ReferenceResolver resolver;
    resolver.RegisterGameObject(other->GetUUID(), other.get());
    thing->SetEditorFields(json{{"target", other->GetUUID().ToString()},
                                {"targets", json::array({other->GetUUID().ToString(), nullptr})}},
                           &resolver);
    resolver.ResolveAll();
    EXPECT_EQ(thing->target, other.get());
    ASSERT_EQ(thing->targets.size(), 2u);
    EXPECT_EQ(thing->targets[0], other.get());
    EXPECT_EQ(thing->targets[1], nullptr);

    thing->SetEditorFields(json{{"target", nullptr}}, &resolver);
    resolver.ResolveAll();
    EXPECT_EQ(thing->target, nullptr);
    EXPECT_EQ(thing->targets.size(), 2u); // the list's key was absent
}

TEST(ReflectionTest, SetEditorFieldsThrowsForAWrongType)
{
    const auto go = GameObject::Create("Thing");
    auto *thing = go->AddComponent<ReflectedThing>();
    EXPECT_THROW(thing->SetEditorFields(json{{"count", "many"}}, nullptr), json::exception);
    EXPECT_THROW(thing->SetEditorFields(json{{"offset", 7}}, nullptr), json::exception);
}

TEST(ReflectionTest, ABaseComponentSetsNothing)
{
    const auto go = GameObject::Create("Bare");
    auto *bare = go->AddComponent<BareComponent>();
    bare->SetEditorFields(json{{"anything", 1}}, nullptr);
    EXPECT_TRUE(bare->IsActive());
}

// ==================== ResourceTypeName ====================

TEST(ReflectionTest, GetResourceTypeIsImplementedFromTheStaticName)
{
    EXPECT_EQ(GameObject::Create("x")->GetResourceType(), GameObject::ResourceTypeName);
    EXPECT_EQ(GameObject::ResourceTypeName, "GameObject");

    const auto go = GameObject::Create("Thing");
    EXPECT_EQ(go->AddComponent<ReflectedThing>()->GetResourceType(), Component::ResourceTypeName);
    EXPECT_EQ(Component::ResourceTypeName, "Component");

    EXPECT_EQ(LuaScript().GetResourceType(), LuaScript::ResourceTypeName);
    EXPECT_EQ(SceneFile().GetResourceType(), SceneFile::ResourceTypeName);

    const auto texture = Rendering::Texture::Create(1, 1, std::vector<std::uint8_t>{0, 0, 0, 255});
    EXPECT_EQ(texture->GetResourceType(), Rendering::Texture::ResourceTypeName);
    EXPECT_EQ(Rendering::Mesh::GetBuiltin(Rendering::BuiltinMesh::Cube)->GetResourceType(),
              Rendering::Mesh::ResourceTypeName);
}

TEST(ReflectionTest, TheStaticNamesAreWhatMetadataAndSceneFilesCall)
{
    EXPECT_EQ(Rendering::Texture::ResourceTypeName, "Texture");
    EXPECT_EQ(Rendering::Mesh::ResourceTypeName, "Mesh");
    EXPECT_EQ(Rendering::Material::ResourceTypeName, "Material");
    EXPECT_EQ(Rendering::Model::ResourceTypeName, "Model");
    EXPECT_EQ(LuaScript::ResourceTypeName, "LuaScript");
    EXPECT_EQ(SceneFile::ResourceTypeName, "Scene");
    EXPECT_EQ(Scene::ResourceTypeName, "Scene");
    EXPECT_EQ(Text::Font::ResourceTypeName, "Font");
    EXPECT_EQ(Audio::AudioClip::ResourceTypeName, "AudioClip");
}

// ==================== ComponentRegistry::Describe ====================

TEST(ReflectionRegistryTest, ATypeDescribesItselfWithoutAnInstanceInAScene)
{
    ComponentRegistry::Instance().Register("ReflectionTest_Thing", [](GameObject &gameObject) -> std::unique_ptr<Component>
    {
        return std::make_unique<ReflectedThing>(gameObject);
    });

    const std::optional<ComponentSchema> schema = ComponentRegistry::Instance().Describe("ReflectionTest_Thing");
    ASSERT_TRUE(schema.has_value());
    EXPECT_EQ(schema->typeName, "ReflectionTest_Thing");
    EXPECT_FALSE(schema->singleton);
    EXPECT_EQ(Find(schema->fields, "mode")->kind, FieldKind::Enum);

    // The defaults are what a new component saves: its values, isActive, and no random uuid
    ASSERT_TRUE(schema->defaults.is_object());
    EXPECT_EQ(schema->defaults.at("count"), 1);
    EXPECT_EQ(schema->defaults.at("mode"), "Slow");
    EXPECT_EQ(schema->defaults.at("isActive"), true);
    EXPECT_FALSE(schema->defaults.contains("uuid"));
}

TEST(ReflectionRegistryTest, AnUnregisteredNameHasNoSchema)
{
    EXPECT_FALSE(ComponentRegistry::Instance().Describe("ReflectionTest_NoSuchType").has_value());
}

TEST(ReflectionRegistryTest, TheSchemaIsCachedAndRegisteringAgainReplacesIt)
{
    auto &registry = ComponentRegistry::Instance();
    registry.Register("ReflectionTest_Replaced", [](GameObject &gameObject) -> std::unique_ptr<Component>
    {
        return std::make_unique<ReflectedThing>(gameObject);
    });
    const std::optional<ComponentSchema> first = registry.Describe("ReflectionTest_Replaced");
    const std::optional<ComponentSchema> again = registry.Describe("ReflectionTest_Replaced");
    ASSERT_TRUE(first.has_value() && again.has_value());
    EXPECT_EQ(first->ToJson(), again->ToJson());
    EXPECT_FALSE(registry.IsSingleton("ReflectionTest_Replaced"));

    // A different type under the same name: the old schema is dropped, and the singleton flag follows
    registry.Register("ReflectionTest_Replaced", [](GameObject &gameObject) -> std::unique_ptr<Component>
    {
        return std::make_unique<LonelyComponent>(gameObject);
    }, true);
    const std::optional<ComponentSchema> replaced = registry.Describe("ReflectionTest_Replaced");
    ASSERT_TRUE(replaced.has_value());
    EXPECT_TRUE(replaced->fields.empty());
    EXPECT_TRUE(replaced->singleton);
    EXPECT_TRUE(registry.IsSingleton("ReflectionTest_Replaced"));
}

TEST(ReflectionRegistryTest, EveryBuiltinTypeIsDescribableAndItsFieldsAreItsSavedKeys)
{
    const std::vector<std::string> builtins = {
        "Rigidbody", "BoxCollider", "SphereCollider", "CapsuleCollider", "Light", "AudioSource", "AudioListener",
        "CubeRenderer", "SphereRenderer", "QuadRenderer", "MeshRenderer", "TextRenderer", "LuaComponent", "Canvas",
        "RectTransform", "Image", "UIText", "Button"};
    auto &registry = ComponentRegistry::Instance();

    for (const std::string &name : builtins)
    {
        ASSERT_TRUE(registry.IsRegistered(name)) << name;
        const std::optional<ComponentSchema> schema = registry.Describe(name);
        ASSERT_TRUE(schema.has_value()) << name;
        EXPECT_EQ(schema->typeName, name);
        ASSERT_TRUE(schema->defaults.is_object()) << name;
        EXPECT_TRUE(schema->defaults.contains("isActive")) << name;

        if (name == "LuaComponent")
        {
            continue; // its fields are the script's, which a throwaway instance doesn't have
        }

        // The fields are exactly what the component saves besides the base keys
        std::vector<std::string> saved;
        for (const auto &[key, value] : schema->defaults.items())
        {
            if (key != "isActive")
            {
                saved.push_back(key);
            }
        }
        std::vector<std::string> described;
        for (const FieldInfo &field : schema->fields)
        {
            described.push_back(field.name);
        }
        std::ranges::sort(saved);
        std::ranges::sort(described);
        EXPECT_EQ(described, saved) << name;
    }
}

TEST(ReflectionRegistryTest, KnownFieldsHaveTheirKindsAndHints)
{
    auto &registry = ComponentRegistry::Instance();

    const ComponentSchema light = registry.Describe("Light").value();
    const FieldInfo *color = Find(light.fields, "color");
    ASSERT_NE(color, nullptr);
    EXPECT_EQ(color->kind, FieldKind::Color); // Light::color is a Vector3 marked AsColor
    EXPECT_EQ(color->typeName, "Vector3");
    const FieldInfo *type = Find(light.fields, "type");
    ASSERT_NE(type, nullptr);
    EXPECT_EQ(type->kind, FieldKind::Enum);
    EXPECT_EQ(type->enumOptions, (std::vector<std::string>{"Directional", "Point", "Spot"}));
    EXPECT_EQ(light.defaults.at("color").at("x"), 1.0);

    const ComponentSchema audio = registry.Describe("AudioSource").value();
    const FieldInfo *volume = Find(audio.fields, "_volume");
    ASSERT_NE(volume, nullptr);
    EXPECT_EQ(volume->kind, FieldKind::Float);
    ASSERT_TRUE(volume->range.has_value());
    EXPECT_DOUBLE_EQ(volume->range->second, 1.0);
    EXPECT_EQ(volume->displayName, "Volume");
    EXPECT_EQ(Find(audio.fields, "_clip")->kind, FieldKind::AssetRef);
    EXPECT_EQ(Find(audio.fields, "_clip")->assetType, "AudioClip");
    EXPECT_EQ(Find(audio.fields, "_playOnAwake")->kind, FieldKind::Bool);

    const ComponentSchema text = registry.Describe("TextRenderer").value();
    EXPECT_EQ(Find(text.fields, "_font")->kind, FieldKind::AssetRef);
    EXPECT_EQ(Find(text.fields, "_font")->assetType, "Font");

    const ComponentSchema meshRenderer = registry.Describe("MeshRenderer").value();
    EXPECT_EQ(Find(meshRenderer.fields, "_mesh")->assetType, "Mesh");
    EXPECT_EQ(Find(meshRenderer.fields, "_materials")->kind, FieldKind::AssetRefList);
    EXPECT_EQ(Find(meshRenderer.fields, "_materials")->assetType, "Material");

    const ComponentSchema body = registry.Describe("Rigidbody").value();
    EXPECT_EQ(Find(body.fields, "_bodyType")->kind, FieldKind::Enum);
    EXPECT_EQ(Find(body.fields, "_bodyType")->typeName, "BodyType");
}

TEST(ReflectionRegistryTest, SingletonTypesAreMarked)
{
    auto &registry = ComponentRegistry::Instance();
    for (const char *name : {"AudioListener", "Canvas", "RectTransform"})
    {
        EXPECT_TRUE(registry.Describe(name).value().singleton) << name;
        EXPECT_TRUE(registry.IsSingleton(name)) << name;
    }
    for (const char *name : {"Light", "AudioSource", "Rigidbody", "MeshRenderer"})
    {
        EXPECT_FALSE(registry.Describe(name).value().singleton) << name;
    }
    EXPECT_FALSE(registry.IsSingleton("ReflectionTest_NoSuchType"));
}

TEST(ReflectionRegistryTest, DescribingLeavesNoObjectBehind)
{
    // The throwaway object is in no scene, and the component made on it is gone with it
    const auto go = GameObject::Create("Holder");
    const std::optional<ComponentSchema> schema = ComponentRegistry::Instance().Describe("Light");
    ASSERT_TRUE(schema.has_value());
    EXPECT_EQ(go->GetComponentCount(), 0u);
    EXPECT_EQ(go->GetScene(), nullptr);
}

// ==================== GameObject::AddComponent(unique_ptr) ====================

TEST(ReflectionRegistryTest, AComponentMadeByNameCanBeAddedToAnObject)
{
    const auto go = GameObject::Create("Lit");
    Component *added = go->AddComponent(ComponentRegistry::Instance().Create("Light", *go));
    ASSERT_NE(added, nullptr);
    EXPECT_EQ(added->GetTypeName(), "Light");
    EXPECT_EQ(go->GetComponentCount(), 1u);
    EXPECT_EQ(go->GetComponent<Rendering::Light>(), added);

    // A second one is added too (only the editor server refuses a second singleton), and the first stays the one found
    Component *second = go->AddComponent(ComponentRegistry::Instance().Create("Light", *go));
    ASSERT_NE(second, nullptr);
    EXPECT_NE(second, added);
    EXPECT_EQ(go->GetComponentCount(), 2u);
    EXPECT_EQ(go->GetComponent<Rendering::Light>(), added);

    EXPECT_TRUE(go->RemoveComponent(added));
    EXPECT_EQ(go->GetComponentCount(), 1u);
}

TEST(ReflectionRegistryTest, AddingANullComponentDoesNothing)
{
    const auto go = GameObject::Create("Empty");
    EXPECT_EQ(go->AddComponent(nullptr), nullptr);
    EXPECT_EQ(go->GetComponentCount(), 0u);
}

// ==================== Numbers a member can't hold ====================

TEST(ReflectionTest, ANumberOutsideTheMembersTypeIsRefused)
{
    const auto go = GameObject::Create("Thing");
    const std::vector<FieldInfo> fields = go->AddComponent<ReflectedThing>()->DescribeFields();

    // int: 32 bits; unsigned: not negative; float: not more than a float holds (it would become infinity, which JSON
    // can't save, so the scene would no longer load); double: anything finite
    EXPECT_FALSE(ValidateFieldValue(*Find(fields, "count"), 2147483647).has_value());
    EXPECT_TRUE(ValidateFieldValue(*Find(fields, "count"), 3000000000LL).has_value());
    EXPECT_TRUE(ValidateFieldValue(*Find(fields, "count"), -3000000000LL).has_value());
    EXPECT_TRUE(ValidateFieldValue(*Find(fields, "unsignedCount"), -1).has_value());
    EXPECT_FALSE(ValidateFieldValue(*Find(fields, "unsignedCount"), 4000000000ULL).has_value());
    EXPECT_TRUE(ValidateFieldValue(*Find(fields, "unsignedCount"), 5000000000ULL).has_value());
    EXPECT_FALSE(ValidateFieldValue(*Find(fields, "ratio"), 1.0e38).has_value());
    EXPECT_TRUE(ValidateFieldValue(*Find(fields, "ratio"), 1.0e300).has_value());
    EXPECT_TRUE(ValidateFieldValue(*Find(fields, "ratio"), -1.0e300).has_value());
    EXPECT_FALSE(ValidateFieldValue(*Find(fields, "wide"), 1.0e300).has_value());
    EXPECT_TRUE(ValidateFieldValue(*Find(fields, "offset"), json{{"x", 1.0e300}, {"y", 0}, {"z", 0}}).has_value());
    EXPECT_TRUE(ValidateFieldValue(*Find(fields, "tint"), json{{"x", 0}, {"y", 0}, {"z", -1.0e40}}).has_value());

    FieldInfo color = Field(FieldKind::Color);
    color.typeName = "Color";
    EXPECT_TRUE(ValidateFieldValue(color, json{{"r", 1}, {"g", 0}, {"b", 0}, {"a", 1.0e300}}).has_value());

    const auto problem = ValidateFieldValue(*Find(fields, "count"), 3000000000LL);
    ASSERT_TRUE(problem.has_value());
    EXPECT_NE(problem->find("count"), std::string::npos) << *problem;
}

TEST(ReflectionTest, AWholeFloatIsStoredAsAnIntegerForAnIntField)
{
    const json three = ClampFieldValue(Field(FieldKind::Int), 3.0);
    EXPECT_TRUE(three.is_number_integer()) << three.dump();
    EXPECT_EQ(three, 3);
    EXPECT_TRUE(ClampFieldValue(Field(FieldKind::Float), 3).is_number_integer()); // a float field's number is left alone
}

TEST(ReflectionTest, ASceneStillSavesAndLoadsAfterTheLargestAcceptedNumbers)
{
    ComponentRegistry::Instance().Register("ReflectionTest_Thing", [](GameObject &gameObject) -> std::unique_ptr<Component>
    {
        return std::make_unique<ReflectedThing>(gameObject);
    });
    const auto go = GameObject::Create("Thing");
    auto *thing = go->AddComponent<ReflectedThing>();
    thing->SetEditorFields(json{{"ratio", 3.0e38}, {"locked", 2147483647}, {"unsignedCount", 4294967295ULL},
                                {"offset", {{"x", -3.0e38}, {"y", 0}, {"z", 3.0e38}}}, {"wide", 1.0e300}},
                           nullptr);

    const auto scene = Scene::Create("Limits");
    scene->AddRootGameObject(go);
    const std::unique_ptr<Scene> loaded = Scene::FromJSON(scene->Serialize());
    ASSERT_NE(loaded, nullptr);
    const auto *copy = loaded->FindGameObject("Thing")->GetComponent<ReflectedThing>();
    ASSERT_NE(copy, nullptr);
    EXPECT_EQ(copy->locked, 2147483647);
    EXPECT_EQ(copy->unsignedCount, 4294967295u);
    EXPECT_FLOAT_EQ(copy->ratio, 3.0e38f);
    EXPECT_DOUBLE_EQ(copy->wide, 1.0e300);
}

// ==================== Removing components in a scene opened for editing ====================

TEST(ReflectionRegistryTest, RemovingAComponentInAnEditSceneRunsNoCallbacks)
{
    CallbackProbe::disables = 0;
    CallbackProbe::destroys = 0;
    const auto scene = Scene::Create("EditRemoval");
    scene->SetEditMode(true);
    const auto go = GameObject::Create("Object");
    scene->AddRootGameObject(go);

    Component *probe = go->AddComponent<CallbackProbe>();
    EXPECT_TRUE(go->RemoveComponent(probe));
    EXPECT_EQ(CallbackProbe::disables, 0) << "never attached, so never enabled";
    EXPECT_EQ(CallbackProbe::destroys, 0);
    EXPECT_EQ(go->GetComponentCount(), 0u);
}

TEST(ReflectionRegistryTest, RemovingAComponentInAnOrdinarySceneStillRunsOnDestroy)
{
    CallbackProbe::disables = 0;
    CallbackProbe::destroys = 0;
    const auto scene = Scene::Create("Removal");
    const auto go = GameObject::Create("Object");
    scene->AddRootGameObject(go);

    Component *probe = go->AddComponent<CallbackProbe>();
    EXPECT_TRUE(go->RemoveComponent(probe));
    EXPECT_EQ(CallbackProbe::destroys, 1);
}

TEST(ReflectionRegistryTest, RemovingAComponentClearsTheReferencesToItInAnEditScene)
{
    const auto scene = Scene::Create("RefRemoval");
    scene->SetEditMode(true);
    const auto target = GameObject::Create("Target");
    const auto holderObject = GameObject::Create("Holder");
    scene->AddRootGameObject(target);
    scene->AddRootGameObject(holderObject);

    auto *first = target->AddComponent<ReflectedThing>();
    auto *second = target->AddComponent<ReflectedThing>();
    auto *holder = holderObject->AddComponent<RefHolder>();
    holder->thing = first;
    holder->things = {first, second, first};

    EXPECT_TRUE(target->RemoveComponent(static_cast<Component *>(first)));
    EXPECT_EQ(holder->thing, nullptr);
    ASSERT_EQ(holder->things.size(), 3u);
    EXPECT_EQ(holder->things[0], nullptr);
    EXPECT_EQ(holder->things[1], second); // another component: kept
    EXPECT_EQ(holder->things[2], nullptr);
}

TEST(ReflectionRegistryTest, AComponentMadeForAnotherObjectIsNotAdded)
{
    const auto go = GameObject::Create("Mine");
    const auto other = GameObject::Create("Theirs");
    EXPECT_EQ(go->AddComponent(ComponentRegistry::Instance().Create("Light", *other)), nullptr);
    EXPECT_EQ(go->GetComponentCount(), 0u);
}

TEST(ReflectionTest, TheTextEffectPassesReportTheTextPassListTypeName)
{
    const auto go = GameObject::Create("Texts");
    auto *renderer = go->AddComponent<Rendering::TextRenderer>();
    auto *label = go->AddComponent<N2Engine::UI::UIText>();
    ASSERT_NE(renderer, nullptr);
    ASSERT_NE(label, nullptr);

    const std::vector<FieldInfo> rendererFields = renderer->DescribeFields();
    const std::vector<FieldInfo> labelFields = label->DescribeFields();
    const FieldInfo *rendererPasses = Find(rendererFields, "_effectPasses");
    const FieldInfo *labelPasses = Find(labelFields, "effectPasses");
    ASSERT_NE(rendererPasses, nullptr);
    ASSERT_NE(labelPasses, nullptr);
    EXPECT_EQ(rendererPasses->kind, FieldKind::Json);
    EXPECT_EQ(rendererPasses->typeName, "TextPass[]");
    EXPECT_EQ(labelPasses->kind, FieldKind::Json);
    EXPECT_EQ(labelPasses->typeName, "TextPass[]");
    EXPECT_EQ(rendererPasses->ToJson()["typeName"], "TextPass[]");
    EXPECT_EQ(rendererPasses->ToJson()["kind"], "Json");
}

TEST(ReflectionTest, TheTextEffectPassesStillRoundTripAsJson)
{
    const auto go = GameObject::Create("Texts");
    auto *label = go->AddComponent<N2Engine::UI::UIText>();
    ASSERT_NE(label, nullptr);

    const json passes = json::array(
        {json{{"color", {{"r", 1.0}, {"g", 0.0}, {"b", 0.0}, {"a", 1.0}}},
              {"offset", {{"x", 0.25}, {"y", -0.5}}},
              {"width", 0.125},
              {"softness", 0.0},
              {"order", -150}}});
    label->Deserialize(json{{"effectPasses", passes}}, nullptr);
    ASSERT_EQ(label->GetEffects().passes.size(), 1u);
    EXPECT_EQ(label->GetEffects().passes[0].order, -150);
    EXPECT_FLOAT_EQ(label->GetEffects().passes[0].width, 0.125f);

    const json saved = label->Serialize();
    ASSERT_TRUE(saved.contains("effectPasses"));
    ASSERT_TRUE(saved["effectPasses"].is_array());
    ASSERT_EQ(saved["effectPasses"].size(), 1u);
    EXPECT_EQ(saved["effectPasses"][0]["order"], -150);
    EXPECT_FLOAT_EQ(saved["effectPasses"][0]["width"].get<float>(), 0.125f);
    EXPECT_FLOAT_EQ(saved["effectPasses"][0]["offset"]["x"].get<float>(), 0.25f);
}
