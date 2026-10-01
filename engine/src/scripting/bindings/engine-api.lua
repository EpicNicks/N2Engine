---@meta

-- ===== MATH CONSTANTS =====

---@class Math
---@field PI number
---@field TWO_PI number
---@field HALF_PI number
---@field DEG_TO_RAD number
---@field RAD_TO_DEG number
---@field EPSILON number
Math = {}

Math.PI = 3.14159265359
Math.TWO_PI = 6.28318530718
Math.HALF_PI = 1.57079632679
Math.DEG_TO_RAD = 0.01745329251
Math.RAD_TO_DEG = 57.2957795131
Math.EPSILON = 0.00001

-- ===== RANDOM =====

---@class Random
Random = {}

---Generate a random float between 0 and 1
---@return number
function Random.Float() end

---Generate a random number in range [min, max]
---@param min number
---@param max number
---@return number
function Random.Range(min, max) end

---Generate a random integer in range [min, max]
---@param min integer
---@param max integer
---@return integer
function Random.Int(min, max) end

---Generate a random point inside the unit circle
---@return Vector2
function Random.InUnitCircle() end

---Generate a random point on the unit circle
---@return Vector2
function Random.OnUnitCircle() end

-- ===== VECTOR2 =====

---@class Vector2
---@field x number
---@field y number
---@field Zero Vector2 Static constant (0, 0)
---@field One Vector2 Static constant (1, 1)
---@field Up Vector2 Static constant (0, 1)
---@field Down Vector2 Static constant (0, -1)
---@field Left Vector2 Static constant (-1, 0)
---@field Right Vector2 Static constant (1, 0)
Vector2 = {}

---Create a new Vector2
---@param x number
---@param y number
---@return Vector2
function Vector2.new(x, y) end

---Dot product
---@param other Vector2
---@return number
function Vector2:Dot(other) end

---Cross product (returns scalar for 2D)
---@param other Vector2
---@return number
function Vector2:Cross(other) end

---Get the length of the vector
---@return number
function Vector2:Length() end

---Get the squared length of the vector
---@return number
function Vector2:LengthSquared() end

---Get a normalized copy of the vector
---@return Vector2
function Vector2:Normalized() end

---Normalize this vector in place
---@return Vector2
function Vector2:Normalize() end

---Distance to another vector
---@param other Vector2
---@return number
function Vector2:Distance(other) end

---Squared distance to another vector
---@param other Vector2
---@return number
function Vector2:DistanceSquared(other) end

---Get the angle of this vector in radians
---@return number
function Vector2:Angle() end

---Get the angle to another vector in radians
---@param other Vector2
---@return number
function Vector2:AngleTo(other) end

---Get a perpendicular vector (counter-clockwise)
---@return Vector2
function Vector2:Perpendicular() end

---Get a perpendicular vector (clockwise)
---@return Vector2
function Vector2:PerpendicularCW() end

---Rotate this vector by angle (radians)
---@param radians number
---@return Vector2
function Vector2:Rotated(radians) end

---Rotate this vector in place
---@param radians number
---@return Vector2
function Vector2:Rotate(radians) end

---Project this vector onto another
---@param onto Vector2
---@return Vector2
function Vector2:Project(onto) end

---Reject this vector from another
---@param onto Vector2
---@return Vector2
function Vector2:Reject(onto) end

---Reflect this vector across a normal
---@param normal Vector2
---@return Vector2
function Vector2:Reflect(normal) end

---Component-wise multiplication
---@param other Vector2
---@return Vector2
function Vector2:Scale(other) end

---Linear interpolation
---@param other Vector2
---@param t number
---@return Vector2
function Vector2:Lerp(other, t) end

---Spherical interpolation
---@param other Vector2
---@param t number
---@return Vector2
function Vector2:Slerp(other, t) end

---Move towards a target
---@param target Vector2
---@param maxDelta number
---@return Vector2
function Vector2:MoveTowards(target, maxDelta) end

---Clamp magnitude to maxLength
---@param maxLength number
---@return Vector2
function Vector2:ClampMagnitude(maxLength) end

---Component-wise min
---@param other Vector2
---@return Vector2
function Vector2:Min(other) end

---Component-wise max
---@param other Vector2
---@return Vector2
function Vector2:Max(other) end

---Clamp components between min and max
---@param min Vector2
---@param max Vector2
---@return Vector2
function Vector2:Clamp(min, max) end

---Floor all components
---@return Vector2
function Vector2:Floor() end

---Ceil all components
---@return Vector2
function Vector2:Ceil() end

---Round all components
---@return Vector2
function Vector2:Round() end

---Absolute value of all components
---@return Vector2
function Vector2:Abs() end

---Sign of all components
---@return Vector2
function Vector2:Sign() end

---Check if vector is zero
---@param tolerance number|nil
---@return boolean
function Vector2:IsZero(tolerance) end

---Check if vector is normalized
---@param tolerance number|nil
---@return boolean
function Vector2:IsNormalized(tolerance) end

---Check if parallel to another vector
---@param other Vector2
---@param tolerance number|nil
---@return boolean
function Vector2:IsParallel(other, tolerance) end

---Check if perpendicular to another vector
---@param other Vector2
---@param tolerance number|nil
---@return boolean
function Vector2:IsPerpendicular(other, tolerance) end

---Create a vector from an angle
---@param radians number
---@return Vector2
function Vector2.FromAngle(radians) end

-- ===== VECTOR3 =====

---@class Vector3
---@field x number
---@field y number
---@field z number
---@field Zero Vector3 Static constant (0, 0, 0)
---@field One Vector3 Static constant (1, 1, 1)
---@field Up Vector3 Static constant (0, 1, 0)
---@field Down Vector3 Static constant (0, -1, 0)
---@field Left Vector3 Static constant (-1, 0, 0)
---@field Right Vector3 Static constant (1, 0, 0)
---@field Forward Vector3 Static constant (0, 0, 1)
---@field Back Vector3 Static constant (0, 0, -1)
Vector3 = {}

---Create a new Vector3
---@param x number
---@param y number
---@param z number
---@return Vector3
function Vector3.new(x, y, z) end

---Dot product
---@param other Vector3
---@return number
function Vector3:Dot(other) end

---Cross product
---@param other Vector3
---@return Vector3
function Vector3:Cross(other) end

---Get the length of the vector
---@return number
function Vector3:Length() end

---Get the squared length of the vector
---@return number
function Vector3:LengthSquared() end

---Get a normalized copy of the vector
---@return Vector3
function Vector3:Normalized() end

---Normalize this vector in place
---@return Vector3
function Vector3:Normalize() end

---Distance to another vector
---@param other Vector3
---@return number
function Vector3:Distance(other) end

---Squared distance to another vector
---@param other Vector3
---@return number
function Vector3:DistanceSquared(other) end

---Get the angle to another vector in radians
---@param other Vector3
---@return number
function Vector3:AngleTo(other) end

---Project this vector onto another
---@param onto Vector3
---@return Vector3
function Vector3:Project(onto) end

---Project this vector onto a plane
---@param planeNormal Vector3
---@return Vector3
function Vector3:ProjectOnPlane(planeNormal) end

---Reject this vector from another
---@param onto Vector3
---@return Vector3
function Vector3:Reject(onto) end

---Reflect this vector across a normal
---@param normal Vector3
---@return Vector3
function Vector3:Reflect(normal) end

---Component-wise multiplication
---@param other Vector3
---@return Vector3
function Vector3:Scale(other) end

---Linear interpolation
---@param other Vector3
---@param t number
---@return Vector3
function Vector3:Lerp(other, t) end

---Spherical interpolation
---@param other Vector3
---@param t number
---@return Vector3
function Vector3:Slerp(other, t) end

---Move towards a target
---@param target Vector3
---@param maxDelta number
---@return Vector3
function Vector3:MoveTowards(target, maxDelta) end

---Clamp magnitude to maxLength
---@param maxLength number
---@return Vector3
function Vector3:ClampMagnitude(maxLength) end

---Get an orthogonal vector
---@return Vector3
function Vector3:GetOrthogonal() end

---Component-wise min
---@param other Vector3
---@return Vector3
function Vector3:Min(other) end

---Component-wise max
---@param other Vector3
---@return Vector3
function Vector3:Max(other) end

---Clamp components between min and max
---@param min Vector3
---@param max Vector3
---@return Vector3
function Vector3:Clamp(min, max) end

---Floor all components
---@return Vector3
function Vector3:Floor() end

---Ceil all components
---@return Vector3
function Vector3:Ceil() end

---Round all components
---@return Vector3
function Vector3:Round() end

---Absolute value of all components
---@return Vector3
function Vector3:Abs() end

---Sign of all components
---@return Vector3
function Vector3:Sign() end

---Check if vector is zero
---@param tolerance number|nil
---@return boolean
function Vector3:IsZero(tolerance) end

---Check if vector is normalized
---@param tolerance number|nil
---@return boolean
function Vector3:IsNormalized(tolerance) end

---Check if parallel to another vector
---@param other Vector3
---@param tolerance number|nil
---@return boolean
function Vector3:IsParallel(other, tolerance) end

---Check if perpendicular to another vector
---@param other Vector3
---@param tolerance number|nil
---@return boolean
function Vector3:IsPerpendicular(other, tolerance) end

-- ===== VECTOR4 =====

---@class Vector4
---@field w number
---@field x number
---@field y number
---@field z number
---@field Zero Vector4 Static constant (0, 0, 0, 0)
---@field One Vector4 Static constant (1, 1, 1, 1)
Vector4 = {}

---Create a new Vector4
---@param w number
---@param x number
---@param y number
---@param z number
---@return Vector4
function Vector4.new(w, x, y, z) end

---Dot product
---@param other Vector4
---@return number
function Vector4:Dot(other) end

---Get the length of the vector
---@return number
function Vector4:Length() end

---Get the squared length of the vector
---@return number
function Vector4:LengthSquared() end

---Get a normalized copy of the vector
---@return Vector4
function Vector4:Normalized() end

---Normalize this vector in place
---@return Vector4
function Vector4:Normalize() end

---Distance to another vector
---@param other Vector4
---@return number
function Vector4:Distance(other) end

---Squared distance to another vector
---@param other Vector4
---@return number
function Vector4:DistanceSquared(other) end

-- ===== QUATERNION =====

---@class Quaternion
---@field w number
---@field x number
---@field y number
---@field z number
---@field Identity Quaternion Static constant identity quaternion
Quaternion = {}

---Create a new Quaternion
---@param w number
---@param x number
---@param y number
---@param z number
---@return Quaternion
function Quaternion.new(w, x, y, z) end

---Create a quaternion from axis and angle
---@param axis Vector3
---@param angle number Angle in radians
---@return Quaternion
function Quaternion.FromAxisAngle(axis, angle) end

---Create a quaternion from Euler angles
---@param pitch number
---@param yaw number
---@param roll number
---@return Quaternion
function Quaternion.FromEulerAngles(pitch, yaw, roll) end

---Create a look rotation
---@param forward Vector3
---@param up Vector3
---@return Quaternion
function Quaternion.LookRotation(forward, up) end

---Spherical linear interpolation
---@param a Quaternion
---@param b Quaternion
---@param t number
---@return Quaternion
function Quaternion.Slerp(a, b, t) end

---Linear interpolation
---@param a Quaternion
---@param b Quaternion
---@param t number
---@return Quaternion
function Quaternion.Lerp(a, b, t) end

---Get the length of the quaternion
---@return number
function Quaternion:Length() end

---Get the squared length of the quaternion
---@return number
function Quaternion:LengthSquared() end

---Get a normalized copy of the quaternion
---@return Quaternion
function Quaternion:Normalized() end

---Normalize this quaternion in place
---@return Quaternion
function Quaternion:Normalize() end

---Get the conjugate
---@return Quaternion
function Quaternion:Conjugate() end

---Get the inverse
---@return Quaternion
function Quaternion:Inverse() end

---Dot product
---@param other Quaternion
---@return number
function Quaternion:Dot(other) end

---Get angle to another quaternion
---@param other Quaternion
---@return number
function Quaternion:Angle(other) end

---Rotate a vector
---@param vector Vector3
---@return Vector3
function Quaternion:Rotate(vector) end

---Convert to Euler angles
---@return Vector3
function Quaternion:ToEulerAngles() end

---Convert to 4x4 matrix
---@return Matrix4
function Quaternion:ToMatrix() end

---Check if normalized
---@param tolerance number|nil
---@return boolean
function Quaternion:IsNormalized(tolerance) end

---Check if identity
---@param tolerance number|nil
---@return boolean
function Quaternion:IsIdentity(tolerance) end

-- ===== MATRIX4 =====

---@class Matrix4
Matrix4 = {}

---Create an identity matrix
---@return Matrix4
function Matrix4.Identity() end

---Create a translation matrix
---@param translation Vector3
---@return Matrix4
function Matrix4.Translation(translation) end

---Create a scale matrix
---@param sx number
---@param sy number
---@param sz number
---@return Matrix4
function Matrix4.Scale(sx, sy, sz) end

---Create a rotation matrix around X axis
---@param angle number Angle in radians
---@return Matrix4
function Matrix4.RotationX(angle) end

---Create a rotation matrix around Y axis
---@param angle number Angle in radians
---@return Matrix4
function Matrix4.RotationY(angle) end

---Create a rotation matrix around Z axis
---@param angle number Angle in radians
---@return Matrix4
function Matrix4.RotationZ(angle) end

---Transform a point
---@param point Vector3
---@return Vector3
function Matrix4:TransformPoint(point) end

---Get the transpose
---@return Matrix4
function Matrix4:transpose() end

---Get the inverse
---@return Matrix4
function Matrix4:inverse() end

---Get the determinant
---@return number
function Matrix4:determinant() end

-- ===== COLOR =====

---@class Color
---@field r number Red component (0-1)
---@field g number Green component (0-1)
---@field b number Blue component (0-1)
---@field a number Alpha component (0-1)
---@field White Color Static constant
---@field Black Color Static constant
---@field Red Color Static constant
---@field Green Color Static constant
---@field Blue Color Static constant
---@field Cyan Color Static constant
---@field Yellow Color Static constant
---@field Magenta Color Static constant
---@field Transparent Color Static constant
Color = {}

---Create a new color
---@param r number
---@param g number
---@param b number
---@param a number
---@return Color
function Color.new(r, g, b, a) end

---Create a color from a hex value
---@param hexValue integer
---@return Color
function Color.FromHex(hexValue) end

---Convert color to hex value
---@return integer
function Color:ToHex() end

-- ===== CORE OBJECTS =====

-- GameObjects, components and Positionables are references a script may keep (in self, a global or a
-- closure). They keep working throughout a teardown (OnDisable/OnDestroy of a destroy, a scene switch or a
-- removed component, including references to other objects torn down at the same time). Once it's over,
-- calling a method on one raises "attempt to use a destroyed ..." instead of crashing; check IsValid() when
-- a kept reference may have outlived its object. Two references to the same object compare equal with ==.
-- A reference doesn't keep its object alive, except the one a script created it with (GameObject.Create)
-- or detached it with (RemoveChild, RemoveRootGameObject), until it's added to a parent or scene again.
-- RemoveChild only takes ownership when the detached object isn't then a root of its scene (the scene owns it).
-- Other references to the same object stay weak. Don't keep that owning reference in the object's own script
-- (use self.gameObject there): while the object is in no scene, it would keep itself alive forever.
-- Scenes, ActionMaps and InputActions are references too, valid until the engine frees them (see each).

---@class Positionable
Positionable = {}

---False once its GameObject is destroyed; the other methods then raise an error
---@return boolean
function Positionable:IsValid() end

---Get world position
---@return Vector3
function Positionable:GetPosition() end

---Set world position
---@param pos Vector3
function Positionable:SetPosition(pos) end

---Get world rotation
---@return Quaternion
function Positionable:GetRotation() end

---Set world rotation
---@param rot Quaternion
function Positionable:SetRotation(rot) end

---Get world scale
---@return Vector3
function Positionable:GetScale() end

---Set world scale
---@param scale Vector3
function Positionable:SetScale(scale) end

---Get forward direction
---@return Vector3
function Positionable:GetForward() end

---Get right direction
---@return Vector3
function Positionable:GetRight() end

---Get up direction
---@return Vector3
function Positionable:GetUp() end

---@class GameObject
---@overload fun(name?: string): GameObject
GameObject = {}

---Create a new GameObject (same as calling GameObject(name)). Add it to a scene with Scene:AddRootGameObject.
---@param name? string Defaults to "GameObject"
---@return GameObject
function GameObject.Create(name) end

---False once this GameObject is destroyed (after the end-of-frame teardown following Destroy); the other
---methods then raise an error
---@return boolean
function GameObject:IsValid() end

---Get the name of this GameObject
---@return string
function GameObject:GetName() end

---Set the name of this GameObject
---@param name string
function GameObject:SetName(name) end

---The tag of this GameObject ("Untagged" unless set)
---@return string
function GameObject:GetTag() end

---Set the tag of this GameObject (any string; saved with the scene)
---@param tag string
function GameObject:SetTag(tag) end

---Whether this GameObject's tag is exactly tag (case-sensitive)
---@param tag string
---@return boolean
function GameObject:CompareTag(tag) end

---The layer of this GameObject, 0..31 (0, Default, unless set; see Layers)
---@return integer
function GameObject:GetLayer() end

---Move this GameObject to a layer (saved with the scene). Its colliders change layer straight away;
---children keep their own layer. Values outside 0..31 are clamped, with a warning.
---@param layer integer
function GameObject:SetLayer(layer) end

---Set the layer of this GameObject and all its descendants
---@param layer integer
function GameObject:SetLayerRecursive(layer) end

---Check if this GameObject is active
---@return boolean
function GameObject:IsActive() end

---Set whether this GameObject is active
---@param active boolean
function GameObject:SetActive(active) end

---Get the positionable component
---@return Positionable|nil
function GameObject:GetPositionable() end

---Check if this GameObject has a positionable
---@return boolean
function GameObject:HasPositionable() end

---Add a child GameObject
---@param child GameObject
function GameObject:AddChild(child) end

---Remove a child GameObject
---@param child GameObject
function GameObject:RemoveChild(child) end

---Get the parent GameObject
---@return GameObject|nil
function GameObject:GetParent() end

---Find a child by name
---@param name string
---@return GameObject|nil
function GameObject:FindChild(name) end

---Find a child recursively by name
---@param name string
---@return GameObject|nil
function GameObject:FindChildRecursive(name) end

---Destroy this GameObject (does nothing if it's already destroyed)
function GameObject:Destroy() end

---Add a component by type name and return it.
---Types: "Rigidbody", "BoxCollider", "SphereCollider", "CapsuleCollider", "CubeRenderer",
---"SphereRenderer", "AudioSource", "AudioListener", "LuaComponent". Unknown names raise an error.
---@param typeName string
---@return Component
function GameObject:AddComponent(typeName) end

---Get a component by type name (same names as AddComponent)
---@param typeName string
---@return Component|nil nil when the object has no component of that type
function GameObject:GetComponent(typeName) end

---@class CubeRenderer : Component
CubeRenderer = {}

---@param color Color
function CubeRenderer:SetColor(color) end

---@return Color
function CubeRenderer:GetColor() end

---@param size Vector3
function CubeRenderer:SetSize(size) end

---@return Vector3
function CubeRenderer:GetSize() end

---@class SphereRenderer : Component
SphereRenderer = {}

---@param color Color
function SphereRenderer:SetColor(color) end

---@return Color
function SphereRenderer:GetColor() end

---@param radius number
function SphereRenderer:SetRadius(radius) end

---@return number
function SphereRenderer:GetRadius() end

---@param latitude integer
---@param longitude integer
function SphereRenderer:SetSubdivision(latitude, longitude) end

---Runs a behaviour script: a file returning a table whose __index is itself, with optional
---OnAttach, OnUpdate, OnFixedUpdate, OnLateUpdate, OnDestroy, OnEnable, OnDisable and collision methods.
---Inside those methods, self.gameObject and self.component are set.
---@class LuaComponent : Component
LuaComponent = {}

---@param path string e.g. "res://scripts/CameraController.lua"
function LuaComponent:SetScript(path) end

---@return string
function LuaComponent:GetScriptPath() end

---True when the script is missing, failed to load, or didn't return a table
---@return boolean
function LuaComponent:HasMissingScript() end

---@class Component
Component = {}

---False once this component is destroyed (removed, its GameObject destroyed, or the scene unloaded);
---the other methods then raise an error
---@return boolean
function Component:IsValid() end

---True once this component is destroyed (the opposite of IsValid)
---@return boolean
function Component:IsDestroyed() end

---The GameObject this component is attached to
---@return GameObject
function Component:GetGameObject() end

---Check if this component is active
---@return boolean
function Component:IsActive() end

---Set whether this component is active
---@param active boolean
function Component:SetActive(active) end

---@class Scene
---@field sceneName string The name of this scene
Scene = {}

---False once this scene is unloaded (after another scene was loaded; it still works in the OnDestroy calls of
---the unload); the other methods then raise an error
---@return boolean
function Scene:IsValid() end

---Find a GameObject by name
---@param name string
---@return GameObject|nil
function Scene:FindGameObject(name) end

---Find the first GameObject (active or not) whose tag is exactly tag
---@param tag string
---@return GameObject|nil
function Scene:FindGameObjectWithTag(tag) end

---Find all GameObjects (active or not) whose tag is exactly tag
---@param tag string
---@return GameObject[]
function Scene:FindGameObjectsWithTag(tag) end

---The same as FindGameObjectsWithTag (the older name)
---@param tag string
---@return GameObject[]
function Scene:FindGameObjectsByTag(tag) end

---Get all GameObjects in the scene
---@return GameObject[]
function Scene:GetAllGameObjects() end

---Get all root GameObjects
---@return GameObject[]
function Scene:GetRootGameObjects() end

---Add a root GameObject
---@param gameObject GameObject
function Scene:AddRootGameObject(gameObject) end

---Remove a root GameObject (false if it isn't one, or for nil)
---@param gameObject GameObject
---@return boolean
function Scene:RemoveRootGameObject(gameObject) end

---Destroy a GameObject (false if it's already destroyed, or for nil)
---@param gameObject GameObject
---@return boolean
function Scene:DestroyGameObject(gameObject) end

---@class SceneManager
SceneManager = {}

---Get the current scene. A kept reference stops working once another scene is loaded.
---@return Scene|nil nil when no scene is loaded
function SceneManager.GetCurrentScene() end

---Get the current scene index
---@return integer
function SceneManager.GetCurrentSceneIndex() end

---Load a scene by index
---@param sceneIndex integer
function SceneManager.LoadScene(sceneIndex) end

-- ===== LAYERS =====

---The project's 32 layers (as in Unity) and which of them collide. A layer mask has bit (1 << layer)
---set for each layer it includes.
---@class Layers
---@field Default integer 0
---@field TransparentFX integer 1
---@field IgnoreRaycast integer 2 ("Ignore Raycast"): left out of queries by default
---@field Water integer 4
---@field UI integer 5
---@field DefaultRaycastMask integer Every layer except Ignore Raycast
Layers = {}

---The layer with this name, or -1
---@param name string
---@return integer
function Layers.NameToLayer(name) end

---The layer's name, or "" for an unnamed or out-of-range layer
---@param layer integer
---@return string
function Layers.LayerToName(layer) end

---Name a user layer (3, 6..31); an empty name clears it. False for a built-in layer, an
---out-of-range one, or a name another layer already has.
---@param layer integer
---@param name string
---@return boolean
function Layers.SetName(layer, name) end

---The mask of the named layers; unknown names are skipped
---@param ... string
---@return integer
function Layers.GetMask(...) end

---Whether colliders on layers a and b interact (contacts and triggers). Symmetric. Pairs that are
---touching when collision is turned off end with an Exit.
---@param a integer
---@param b integer
---@param collide boolean
function Layers.SetCollision(a, b, collide) end

---@param a integer
---@param b integer
---@return boolean
function Layers.GetCollision(a, b) end

-- ===== PHYSICS =====

---@enum BodyType
BodyType = {
    Static = 0,
    Dynamic = 1,
    Kinematic = 2,
}

---The argument of OnCollisionEnter/Stay/Exit. Safe to keep after the callback: its objects are
---references like any other (see CORE OBJECTS).
---@class Collision
---@field gameObject GameObject The object receiving the callback
---@field otherGameObject GameObject|nil The other object
---@field rigidbody Rigidbody|nil The receiver's Rigidbody (nil for a static collider)
---@field otherRigidbody Rigidbody|nil The other object's Rigidbody (nil for a static collider)
---@field relativeVelocity Vector3
---@field impulse Vector3
---@field contactCount integer
Collision = {}

---A contact point, 1-based
---@param index integer
---@return ContactPoint|nil
function Collision:GetContact(index) end

---@return Vector3
function Collision:GetAverageContactPoint() end

---@class ContactPoint
---@field point Vector3
---@field normal Vector3
---@field separation number
---@field normalImpulse number
ContactPoint = {}

---The argument of OnTriggerEnter/Stay/Exit; safe to keep, like Collision
---@class Trigger
---@field gameObject GameObject
---@field otherGameObject GameObject|nil
---@field rigidbody Rigidbody|nil
---@field otherRigidbody Rigidbody|nil
Trigger = {}

---@class PhysicsMaterial
---@field staticFriction number Friction when not moving (0-1)
---@field dynamicFriction number Friction when sliding (0-1)
---@field restitution number Bounciness (0-1)
PhysicsMaterial = {}

---Create a new physics material
---@return PhysicsMaterial
function PhysicsMaterial.new() end

---Get the default material
---@return PhysicsMaterial
function PhysicsMaterial.Default() end

---Get an ice material
---@return PhysicsMaterial
function PhysicsMaterial.Ice() end

---Get a rubber material
---@return PhysicsMaterial
function PhysicsMaterial.Rubber() end

---Get a bouncy material
---@return PhysicsMaterial
function PhysicsMaterial.Bouncy() end

---Get a metal material
---@return PhysicsMaterial
function PhysicsMaterial.Metal() end

---@class Rigidbody : Component
Rigidbody = {}

---Set the body type
---@param bodyType BodyType
function Rigidbody:SetBodyType(bodyType) end

---Get the body type
---@return BodyType
function Rigidbody:GetBodyType() end

---Set the mass
---@param mass number
function Rigidbody:SetMass(mass) end

---Get the mass
---@return number
function Rigidbody:GetMass() end

---Set whether gravity is enabled
---@param enabled boolean
function Rigidbody:SetGravityEnabled(enabled) end

---Check if gravity is enabled
---@return boolean
function Rigidbody:IsGravityEnabled() end

---Add a force
---@param force Vector3
function Rigidbody:AddForce(force) end

---Add an impulse
---@param impulse Vector3
function Rigidbody:AddImpulse(impulse) end

---Set the velocity
---@param velocity Vector3
function Rigidbody:SetVelocity(velocity) end

---Set the angular velocity
---@param velocity Vector3
function Rigidbody:SetAngularVelocity(velocity) end

---Get the velocity
---@return Vector3
function Rigidbody:GetVelocity() end

---Get the angular velocity
---@return Vector3
function Rigidbody:GetAngularVelocity() end

---@class BoxCollider : Component
BoxCollider = {}

---Set the size
---@param size Vector3
function BoxCollider:SetSize(size) end

---Get the size
---@return Vector3
function BoxCollider:GetSize() end

---Set the half extents
---@param halfExtents Vector3
function BoxCollider:SetHalfExtents(halfExtents) end

---Get the half extents
---@return Vector3
function BoxCollider:GetHalfExtents() end

---Set whether this is a trigger
---@param isTrigger boolean
function BoxCollider:SetIsTrigger(isTrigger) end

---Check if this is a trigger
---@return boolean
function BoxCollider:IsTrigger() end

---Set the physics material
---@param material PhysicsMaterial
function BoxCollider:SetMaterial(material) end

---Get the physics material
---@return PhysicsMaterial
function BoxCollider:GetMaterial() end

---Set the offset
---@param offset Vector3
function BoxCollider:SetOffset(offset) end

---Get the offset
---@return Vector3
function BoxCollider:GetOffset() end

---@class SphereCollider : Component
SphereCollider = {}

---Set the radius
---@param radius number
function SphereCollider:SetRadius(radius) end

---Get the radius
---@return number
function SphereCollider:GetRadius() end

---Set whether this is a trigger
---@param isTrigger boolean
function SphereCollider:SetIsTrigger(isTrigger) end

---Check if this is a trigger
---@return boolean
function SphereCollider:IsTrigger() end

---Set the physics material
---@param material PhysicsMaterial
function SphereCollider:SetMaterial(material) end

---Get the physics material
---@return PhysicsMaterial
function SphereCollider:GetMaterial() end

---Set the offset
---@param offset Vector3
function SphereCollider:SetOffset(offset) end

---Get the offset
---@return Vector3
function SphereCollider:GetOffset() end

---@class CapsuleCollider : Component
CapsuleCollider = {}

---Set the radius
---@param radius number
function CapsuleCollider:SetRadius(radius) end

---Get the radius
---@return number
function CapsuleCollider:GetRadius() end

---Set the height
---@param height number
function CapsuleCollider:SetHeight(height) end

---Get the height
---@return number
function CapsuleCollider:GetHeight() end

---Set whether this is a trigger
---@param isTrigger boolean
function CapsuleCollider:SetIsTrigger(isTrigger) end

---Check if this is a trigger
---@return boolean
function CapsuleCollider:IsTrigger() end

---Set the physics material
---@param material PhysicsMaterial
function CapsuleCollider:SetMaterial(material) end

---Get the physics material
---@return PhysicsMaterial
function CapsuleCollider:GetMaterial() end

---Set the offset
---@param offset Vector3
function CapsuleCollider:SetOffset(offset) end

---Get the offset
---@return Vector3
function CapsuleCollider:GetOffset() end

-- ===== AUDIO =====

---@class AudioSource : Component
AudioSource = {}

---Play the audio clip from the beginning (restarts it if it's already playing or paused)
function AudioSource:Play() end

---Pause the audio clip
function AudioSource:Pause() end

---Resume a paused clip from where it was paused
function AudioSource:UnPause() end

---Stop the audio clip
function AudioSource:Stop() end

---Check if currently playing
---@return boolean
function AudioSource:IsPlaying() end

---Check if currently paused
---@return boolean
function AudioSource:IsPaused() end

---Set the volume
---@param volume number Volume (0-1)
function AudioSource:SetVolume(volume) end

---Get the volume
---@return number
function AudioSource:GetVolume() end

---Set the pitch
---@param pitch number Pitch multiplier
function AudioSource:SetPitch(pitch) end

---Get the pitch
---@return number
function AudioSource:GetPitch() end

---Set whether to loop
---@param loop boolean
function AudioSource:SetLoop(loop) end

---Check if looping
---@return boolean
function AudioSource:GetLoop() end

---Set whether spatial audio is enabled
---@param spatial boolean
function AudioSource:SetSpatial(spatial) end

---Check if spatial audio is enabled
---@return boolean
function AudioSource:GetSpatial() end

---Set the mixer group
---@param group string
function AudioSource:SetMixerGroup(group) end

---Get the mixer group
---@return string
function AudioSource:GetMixerGroup() end

---Set the minimum distance for 3D audio
---@param distance number
function AudioSource:SetMinDistance(distance) end

---Set the maximum distance for 3D audio
---@param distance number
function AudioSource:SetMaxDistance(distance) end

---Set the rolloff factor for 3D audio
---@param factor number
function AudioSource:SetRolloffFactor(factor) end

---@class AudioListener : Component
AudioListener = {}

---@class Audio
Audio = {}

---Set the master volume
---@param volume number Volume (0-1)
function Audio.SetMasterVolume(volume) end

---Get the master volume
---@return number
function Audio.GetMasterVolume() end

---Set the listener position
---@param x number
---@param y number
---@param z number
function Audio.SetListenerPosition(x, y, z) end

---Set the listener orientation
---@param fx number Forward X
---@param fy number Forward Y
---@param fz number Forward Z
---@param ux number Up X
---@param uy number Up Y
---@param uz number Up Z
function Audio.SetListenerOrientation(fx, fy, fz, ux, uy, uz) end

-- ===== INPUT =====

---@enum ActionPhase
ActionPhase = {
    Waiting = 0,
    Started = 1,
    Performed = 2,
    Cancelled = 3,
}

---@class InputValue
InputValue = {}

---Get the value as a boolean
---@return boolean
function InputValue:GetBool() end

---Get the value as a float
---@return number
function InputValue:GetFloat() end

---Get the value as a Vector2
---@return Vector2
function InputValue:GetVector2() end

---@class InputAction
InputAction = {}

---False once this action is freed: replaced or removed from its map, or its map replaced (e.g. by
---Input.CreateActionMap with the same name). Replaced from inside an input callback, it's freed when that
---frame's input update ends. The other methods then raise an error.
---@return boolean
function InputAction:IsValid() end

---Get the current phase
---@return ActionPhase
function InputAction:GetPhase() end

---Check if the action was started this frame
---@return boolean
function InputAction:WasStarted() end

---Check if the action was performed this frame
---@return boolean
function InputAction:WasPerformed() end

---Check if the action was cancelled this frame
---@return boolean
function InputAction:WasCancelled() end

---Check if the action is currently active
---@return boolean
function InputAction:IsActive() end

---Get the value as a Vector2
---@return Vector2
function InputAction:GetVector2Value() end

---Get the value as a boolean
---@return boolean
function InputAction:GetBoolValue() end

---Get the value as a float
---@return number
function InputAction:GetFloatValue() end

---Check if the action is disabled
---@return boolean
function InputAction:GetDisabled() end

---Set whether the action is disabled
---@param disabled boolean
function InputAction:SetDisabled(disabled) end

---Get the action name
---@return string
function InputAction:GetName() end

---Subscribe to state changes.
---Subscribed from a LuaComponent script, the callback stops firing once that component is destroyed.
---@param callback fun(action: InputAction)
---@return integer subscriptionId Use this to unsubscribe later
function InputAction:Subscribe(callback) end

---Unsubscribe from state changes
---@param id integer Subscription ID returned from Subscribe
function InputAction:Unsubscribe(id) end

---Get the event for manual subscription: the action itself, which has the event's Subscribe/Unsubscribe
---(the event lives on the action and goes when it's freed)
---@return InputAction
function InputAction:OnStateChanged() end

---@class ActionMap
---@field disabled boolean Whether this action map is disabled
---@field name string The name of this action map
ActionMap = {}

---False once this map is freed (replaced, e.g. by Input.CreateActionMap with its name; replaced from inside
---an input callback, it's freed when that frame's input update ends). The other methods then raise an error.
---@return boolean
function ActionMap:IsValid() end

---Get an action by name. map["Jump"] works too, except for actions named like an ActionMap member
---(IsValid, Get, name, disabled), which only Get reaches.
---@param actionName string
---@return InputAction|nil
function ActionMap:Get(actionName) end

---@class Input
Input = {}

---Get an action map by name
---@param mapName string
---@return ActionMap|nil
function Input.GetActionMap(mapName) end

---Load an action map by name
---@param mapName string
---@return ActionMap|nil
function Input.LoadActionMap(mapName) end

---Create (or replace) an action map. Each action lists bindings; fields and names follow the action map JSON format:
---  { type = "KeyboardButton", key = "Escape" }
---  { type = "Vector2Composite", up = "W", down = "S", left = "A", right = "D" }
---  { type = "GamepadStick", xAxis = "LeftX", yAxis = "LeftY", deadzone = 0.25, invertX = false, invertY = true }
---  { type = "GamepadButton", button = "South" }, { type = "GamepadAxis", axis = "LeftTrigger" }, { type = "MouseButton", button = "Left" }
---@param mapName string
---@param actions table<string, table[]> e.g. { ["Quit"] = { { type = "KeyboardButton", key = "Escape" } } }
---@return ActionMap|nil nil without a window, or if the map is malformed. References to a map it replaces stop working.
function Input.CreateActionMap(mapName, actions) end

-- ===== EVENTS =====

---@class Event
Event = {}

---Subscribe to this event.
---Subscribed from a LuaComponent script, the callback stops firing once that component is destroyed.
---@param callback fun()
---@return integer subscriptionId Use this to unsubscribe later
function Event:Subscribe(callback) end

---Unsubscribe from this event
---@param id integer Subscription ID returned from Subscribe
function Event:Unsubscribe(id) end

---Invoke this event
function Event:Invoke() end

---Get the number of subscribers
---@return integer
function Event:GetSubscriberCount() end

---@class WindowResizeEvent
WindowResizeEvent = {}

---Subscribe to window resize events.
---Subscribed from a LuaComponent script, the callback stops firing once that component is destroyed.
---@param callback fun(width: integer, height: integer)
---@return integer subscriptionId
function WindowResizeEvent:Subscribe(callback) end

---Unsubscribe from window resize events
---@param id integer
function WindowResizeEvent:Unsubscribe(id) end

---@class GameObjectEvent
GameObjectEvent = {}

---Subscribe to GameObject events.
---Subscribed from a LuaComponent script, the callback stops firing once that component is destroyed.
---@param callback fun(gameObject: GameObject)
---@return integer subscriptionId
function GameObjectEvent:Subscribe(callback) end

---Unsubscribe from GameObject events
---@param id integer
function GameObjectEvent:Unsubscribe(id) end

-- ===== TIME =====

---@class Time
---@field deltaTime number Time since last frame (scaled)
---@field time number Time since game start (scaled)
---@field fixedDeltaTime number Fixed timestep for physics (scaled)
---@field unscaledDeltaTime number Time since last frame (unscaled)
---@field unscaledTime number Time since game start (unscaled)
---@field fixedUnscaledDeltaTime number Fixed timestep for physics (unscaled)
---@field timeScale number Time scale multiplier (1.0 = normal, 0.5 = slow motion, 0.0 = paused)
Time = {}

-- ===== APPLICATION =====

---@class Application
Application = {}

---Quit the application
function Application.Quit() end

---Get the platform name
---@return string
function Application.GetPlatform() end

---Get the main camera
---@return Camera
function Application.GetCamera() end

-- ===== WINDOW =====

---@enum WindowMode
WindowMode = {
    Windowed = 0,
    Fullscreen = 1,
    BorderlessWindowed = 2,
}

---@class Window
Window = {}

---Get the window instance
---@return Window
function Window.Get() end

---Get the window width
---@return integer
function Window.GetWidth() end

---Get the window height
---@return integer
function Window.GetHeight() end

---Set the window title
---@param title string
function Window.SetTitle(title) end

---Set the window mode
---@param mode WindowMode
function Window.SetMode(mode) end

---Set the clear color
---@param color Color
function Window.SetClearColor(color) end

-- ===== CAMERA =====

---@enum OrthographicResizeMode
OrthographicResizeMode = {
    MaintainVertical = 0,
    MaintainHorizontal = 1,
    MaintainLarger = 2,
}

---@class BoundingBox
---@field min Vector3
---@field max Vector3
BoundingBox = {}

---Create a new bounding box
---@param min Vector3
---@param max Vector3
---@return BoundingBox
function BoundingBox.new(min, max) end

---Get the center of the bounding box
---@return Vector3
function BoundingBox:GetCenter() end

---Get the extents (half-sizes) of the bounding box
---@return Vector3
function BoundingBox:GetExtents() end

---Get a corner of the bounding box
---@param index integer Corner index (0-7)
---@return Vector3
function BoundingBox:GetCorner(index) end

---@class Frustum
Frustum = {}

---Check if a bounding box is visible in the frustum
---@param bbox BoundingBox
---@return boolean
function Frustum:IsVisible(bbox) end

---@class Camera
Camera = {}

---Get the main camera (it lives as long as the application, so a kept reference stays valid)
---@return Camera
function Camera.Main() end

---Set the camera position
---@param position Vector3
function Camera:SetPosition(position) end

---Set the camera rotation
---@param rotation Quaternion
function Camera:SetRotation(rotation) end

---Make the camera look at a target
---@param target Vector3
---@param up Vector3|nil Optional up vector (default: Vector3.Up)
function Camera:LookAt(target, up) end

---Get the camera position
---@return Vector3
function Camera:GetPosition() end

---Get the camera rotation
---@return Quaternion
function Camera:GetRotation() end

---Set perspective projection
---@param fov number Field of view in degrees
---@param aspect number Aspect ratio
---@param nearPlane number Near clipping plane
---@param farPlane number Far clipping plane
function Camera:SetPerspective(fov, aspect, nearPlane, farPlane) end

---Set orthographic projection
---@param left number
---@param right number
---@param bottom number
---@param top number
---@param nearPlane number
---@param farPlane number
function Camera:SetOrthographic(left, right, bottom, top, nearPlane, farPlane) end

---Set orthographic resize mode
---@param mode OrthographicResizeMode
function Camera:SetOrthographicResizeMode(mode) end

---Update the aspect ratio
---@param newAspect number
function Camera:UpdateAspectRatio(newAspect) end

---Get the aspect ratio
---@return number
function Camera:GetAspectRatio() end

---Get the view matrix
---@return Matrix4
function Camera:GetViewMatrix() end

---Get the projection matrix
---@return Matrix4
function Camera:GetProjectionMatrix() end

---Get the view-projection matrix
---@return Matrix4
function Camera:GetViewProjectionMatrix() end

---Get the view frustum
---@return Frustum
function Camera:GetViewFrustum() end

---Get the near clipping plane
---@return number
function Camera:GetNearPlane() end

---Get the far clipping plane
---@return number
function Camera:GetFarPlane() end

---Get the field of view
---@return number
function Camera:GetFOV() end

-- ===== DEBUG =====

---@class Debug
Debug = {}

---Log a message
---@param message string
function Debug.Log(message) end

---Log a warning
---@param message string
function Debug.Warn(message) end

---Log an error
---@param message string
function Debug.Error(message) end
