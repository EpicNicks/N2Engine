#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <math/Vector2.hpp>
#include <renderer/common/RenderTypes.hpp>

#include "engine/common/Color.hpp"
#include "engine/physics/PhysicsHandle.hpp"
#include "engine/rendering/Geometry2D.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/Texture.hpp"
#include "engine/serialization/FieldInfo.hpp"

namespace N2Engine::Physics2D
{
    class Physics2DWorld;

    /// The outline a SoftBody2D is made from, in the object's local x/y
    enum class SoftBodyShape2D : std::uint8_t
    {
        /// nodeCount nodes on a circle of `radius`
        Circle,
        /// nodeCount nodes (at least 4) around a box of `size`, the corners among them
        Box,
        /// The points of `outline`, one node each
        Outline
    };

    N2_SERIALIZE_ENUM(SoftBodyShape2D, {
                      {SoftBodyShape2D::Circle, "Circle"},
                      {SoftBodyShape2D::Box, "Box"},
                      {SoftBodyShape2D::Outline, "Outline"}
                      })

    /**
     * A deformable 2D body on Box2D (issue #445; docs/physics2d.html#soft-bodies), drawn as a filled shape. Its outline is a
     * ring of small circle bodies (the nodes), joined by distance springs to their neighbours and next-but-one neighbours
     * (the edge springs) and to a centre body at the object's position (the shape springs). The nodes collide with other 2D
     * colliders by the object's layer, never with each other; queries, trigger and collision messages do not see them.
     *
     * The body is made in the first FixedUpdate from the object's world position, rotation and scale. After that the
     * object's position follows the centre (its rotation is left alone), moving the object moves the whole body, and a
     * change of a shape or physics setting rebuilds it, at rest, where the object is. Disabling the component or the object
     * takes the body out of the simulation where it is. The object's layer is read when the body is made (Rebuild applies a
     * new one). Do not put a Rigidbody2D or Collider2D on the same object.
     *
     * Drawing: like MeshRenderer2D (unlit, blended, the Transparent queue by sortingOrder), the colour times the texture
     * mapped over the rest outline's box, with the outline pushed out by the node radius so it covers what collides.
     * Without Box2D (or before the body is made, as in the editor's edit mode) the rest shape is drawn.
     *
     * Simulation: part of the 2D world, so rollback saves, restores and hashes it with everything else. A late joiner
     * matches it to its own by net id, as it does a Rigidbody2D.
     */
    class SoftBody2D final : public Rendering::GeometryRenderer2D
    {
    public:
        static constexpr int MinNodes = 3;
        static constexpr int MaxNodes = 64;

        explicit SoftBody2D(GameObject &gameObject);

        [[nodiscard]] std::string GetTypeName() const override { return "SoftBody2D"; }

        // ===== Shape (a change rebuilds the body) =====
        [[nodiscard]] SoftBodyShape2D GetShape() const { return _shape; }
        [[nodiscard]] float GetRadius() const { return _radius; }
        [[nodiscard]] const Math::Vector2 &GetSize() const { return _size; }
        [[nodiscard]] int GetNodeCountSetting() const { return _nodeCount; }
        [[nodiscard]] const std::vector<Math::Vector2> &GetOutline() const { return _outline; }
        void SetCircle(float radius, int nodeCount);
        void SetBox(const Math::Vector2 &size, int nodeCount);
        /// 3 to 64 finite points around the object's origin, either winding; false (and nothing changes) otherwise
        bool SetOutline(const std::vector<Math::Vector2> &points);
        /// The rest outline in local space, counter-clockwise; empty for settings that give none
        [[nodiscard]] std::vector<Math::Vector2> RestOutline() const;

        // ===== Physics settings (a change rebuilds the body) =====
        [[nodiscard]] float GetNodeRadius() const { return _nodeRadius; }
        void SetNodeRadius(float radius);
        [[nodiscard]] float GetDensity() const { return _density; }
        void SetDensity(float density);
        [[nodiscard]] float GetFriction() const { return _friction; }
        void SetFriction(float friction);
        [[nodiscard]] float GetBounciness() const { return _bounciness; }
        void SetBounciness(float bounciness);
        /// Hertz and damping ratio of the outline's springs and of the springs to the centre
        void SetStiffness(float edgeFrequency, float edgeDamping, float shapeFrequency, float shapeDamping);
        [[nodiscard]] float GetEdgeFrequency() const { return _edgeFrequency; }
        [[nodiscard]] float GetEdgeDamping() const { return _edgeDamping; }
        [[nodiscard]] float GetShapeFrequency() const { return _shapeFrequency; }
        [[nodiscard]] float GetShapeDamping() const { return _shapeDamping; }
        [[nodiscard]] float GetGravityScale() const { return _gravityScale; }
        void SetGravityScale(float scale);
        [[nodiscard]] float GetLinearDamping() const { return _linearDamping; }
        void SetLinearDamping(float damping);

        // ===== Appearance =====
        [[nodiscard]] const Common::Color &GetColor() const { return _color; }
        void SetColor(const Common::Color &color) { _color = color; }
        [[nodiscard]] const std::shared_ptr<Rendering::Texture> &GetTexture() const { return _texture; }
        void SetTexture(std::shared_ptr<Rendering::Texture> texture) { _texture = std::move(texture); }

        // ===== Runtime =====
        /// How many nodes the outline has
        [[nodiscard]] std::size_t GetNodeCount() const;
        /// The nodes' world positions, in outline order: the simulated ones, or the rest outline at the object before there
        /// is a body
        [[nodiscard]] std::vector<Math::Vector2> GetNodePositions() const;
        /// The centre body's world position, or the object's before there is a body
        [[nodiscard]] Math::Vector2 GetCentre() const;
        [[nodiscard]] Math::Vector2 GetVelocity() const;
        /// Holds a node where it is (or lets it go). Saved: a rebuilt body pins it at its rest place. An index outside the
        /// outline does nothing
        void SetNodePinned(std::size_t node, bool pinned);
        [[nodiscard]] bool IsNodePinned(std::size_t node) const;
        /// An impulse on the whole body, shared by mass over the unpinned nodes and the centre; nothing before there is a body
        void AddImpulse(const Math::Vector2 &impulse);
        /// Rebuilds the body at rest where the object is, in the next FixedUpdate
        void Rebuild();
        /// Whether there is a simulated body now
        [[nodiscard]] bool HasBody() const;
        [[nodiscard]] Physics::PhysicsBodyHandle GetHandle() const { return _handle; }

        // ===== Lifecycle =====
        void OnAttach() override;
        void OnDestroy() override;
        void OnEnable() override;
        void OnDisable() override;
        void OnFixedUpdate() override;
        void OnEditorFieldsChanged(std::span<const std::string> changed) override;

        [[nodiscard]] std::optional<BoundingBox> GetWorldBounds() const override;
        void RenderInQueue(Renderer::Common::IRenderer *renderer, const Renderer::Common::RenderState &state,
                           RenderQueue queue) override;

        /// The drawn mesh for node positions (world) and a centre: what RenderInQueue builds, public for tests
        [[nodiscard]] Renderer::Common::MeshData BuildMesh(const std::vector<Math::Vector2> &nodes,
                                                           const Math::Vector2 &centre, float z) const;

        // Called by the 2D world and the object's transform
        void OnTransformChanged() const;
        void OnBodyLost() { _handle = Physics::INVALID_PHYSICS_HANDLE; }
        void OnBodyRenamed(const Physics::PhysicsBodyHandle handle) { _handle = handle; }

        static constexpr bool IsSingleton = true;

    protected:
        void OnActiveFlagChanged() override;

    private:
        SoftBodyShape2D _shape = SoftBodyShape2D::Circle;
        float _radius = 1.0f;
        Math::Vector2 _size{2.0f, 2.0f};
        int _nodeCount = 16;
        std::vector<Math::Vector2> _outline;
        std::vector<int> _pinned;
        float _nodeRadius = 0.1f;
        float _density = 1.0f;
        float _friction = 0.4f;
        float _bounciness = 0.0f;
        float _edgeFrequency = 8.0f;
        float _edgeDamping = 0.5f;
        float _shapeFrequency = 3.0f;
        float _shapeDamping = 0.5f;
        float _gravityScale = 1.0f;
        float _linearDamping = 0.0f;
        Common::Color _color{1.0f, 1.0f, 1.0f, 1.0f};
        std::shared_ptr<Rendering::Texture> _texture;

        bool _attached = false;
        bool _warned = false; // a body that could not be made is said once
        std::weak_ptr<Physics2DWorld> _world;
        Physics::PhysicsBodyHandle _handle = Physics::INVALID_PHYSICS_HANDLE;
        // The drawn mesh, and what it was built from
        std::shared_ptr<Rendering::Mesh> _mesh;
        std::vector<Math::Vector2> _meshNodes;
        Math::Vector2 _meshCentre{};
        float _meshZ = 0.0f;
        bool _meshStale = true;

        /// The world, when this component's body is in it
        [[nodiscard]] Physics2DWorld *Live() const;
        /// Makes the body when there is none (or a restore dropped it)
        void Refresh();
        void DestroyBody();
        /// The rest outline placed by the object's transform, counter-clockwise
        [[nodiscard]] std::vector<Math::Vector2> PlacedOutline() const;
        [[nodiscard]] Math::Vector2 Position() const;
    };
}
