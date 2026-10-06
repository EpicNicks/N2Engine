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
---"SphereRenderer", "QuadRenderer", "MeshRenderer", "TextRenderer", "AudioSource", "AudioListener",
---"LuaComponent", "RectTransform", "Canvas", "Image", "UIText", "Button". Unknown names raise an error.
---@param typeName string
---@return Component
function GameObject:AddComponent(typeName) end

---Get a component by type name (same names as AddComponent)
---@param typeName string
---@return Component|nil nil when the object has no component of that type
function GameObject:GetComponent(typeName) end

---A 1 x 1 x 1 cube (the built-in "Cube" mesh), unlit in its colour unless it has a material.
---Opaque: a translucent colour doesn't blend; give it a material with alphaMode "blend" for that.
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

---A .mat file the cube draws with, tinted by its colour; nil goes back to unlit in the colour.
---Raises an error (keeping the current material) if it doesn't load.
---@param path string|nil e.g. "res://materials/crate.mat"
function CubeRenderer:SetMaterial(path) end

---@return string|nil path nil without a material
function CubeRenderer:GetMaterial() end

---A sphere of diameter 1 (the built-in "Sphere" mesh at the default subdivision), unlit in its colour unless it
---has a material
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

---Mesh quality (16 x 32 by default); takes effect on the next draw
---@param latitude integer
---@param longitude integer
function SphereRenderer:SetSubdivision(latitude, longitude) end

---@param path string|nil a .mat file, or nil for unlit in the colour
function SphereRenderer:SetMaterial(path) end

---@return string|nil
function SphereRenderer:GetMaterial() end

---A 1 x 1 quad on the object's x/y plane, facing +Z (the built-in "Quad" mesh), unlit in its colour unless it
---has a material
---@class QuadRenderer : Component
QuadRenderer = {}

---@param color Color
function QuadRenderer:SetColor(color) end

---@return Color
function QuadRenderer:GetColor() end

---@param size Vector3
function QuadRenderer:SetSize(size) end

---@return Vector3
function QuadRenderer:GetSize() end

---@param path string|nil a .mat file, or nil for unlit in the colour
function QuadRenderer:SetMaterial(path) end

---@return string|nil
function QuadRenderer:GetMaterial() end

---Draws a mesh with one material per submesh. An empty slot draws lit white. Blend materials draw in the
---Transparent queue, the rest in the Opaque queue.
---@class MeshRenderer : Component
MeshRenderer = {}

---A built-in mesh by name, or a mesh asset's path; nil clears it. Raises an error (keeping the current mesh) if
---it doesn't load.
---@param nameOrPath string|nil "Cube", "Sphere", "Quad", or a mesh asset's res:// path
function MeshRenderer:SetMesh(nameOrPath) end

---@return string|nil nameOrPath the built-in's name or the asset's path ("" for a mesh made at runtime); nil without a mesh
function MeshRenderer:GetMesh() end

---The number of material slots that draw: the mesh's submesh count (0 without a mesh)
---@return integer
function MeshRenderer:GetMaterialCount() end

---Sets material slot `index` (1-based) to a .mat file, or empties it with nil. Raises an error (keeping the
---slot) if the file doesn't load.
---@param index integer
---@param path string|nil
function MeshRenderer:SetMaterial(index, path) end

---@param index integer 1-based
---@return string|nil path nil for an empty slot
function MeshRenderer:GetMaterial(index) end

---The mesh's bounds in its own space (before the object's transform)
---@return BoundingBox|nil nil without a mesh
function MeshRenderer:GetBounds() end

---World-space text on the object's x/y plane, facing +Z, drawn with an SDF font in the Transparent queue.
---Sizes are in world units. Changing a setting other than the colour lays the text out again on the next draw.
---@alias HorizontalAlign "Left"|"Center"|"Right"
---@alias VerticalAlign "Top"|"Middle"|"Bottom"|"Baseline"
---@class TextRenderer : Component
TextRenderer = {}

---@param text string UTF-8; "\n" breaks lines
function TextRenderer:SetText(text) end

---@return string
function TextRenderer:GetText() end

---@param color Color
function TextRenderer:SetColor(color) end

---@return Color
function TextRenderer:GetColor() end

---@param size number world units per em (default 1)
function TextRenderer:SetFontSize(size) end

---@return number
function TextRenderer:GetFontSize() end

---@param width number wrap width in world units; 0 (the default) never wraps
function TextRenderer:SetMaxWidth(width) end

---@return number
function TextRenderer:GetMaxWidth() end

---@param spacing number multiplies the font's line height (default 1)
function TextRenderer:SetLineSpacing(spacing) end

---@return number
function TextRenderer:GetLineSpacing() end

---@param spacing number extra space between glyphs, in ems (default 0; negative tightens)
function TextRenderer:SetLetterSpacing(spacing) end

---@return number
function TextRenderer:GetLetterSpacing() end

---An outline around the glyphs, drawn under them. Lengths are in ems; effects are limited to 90% of the
---font's SDF spread (1/6 em for the default font: about 0.15 em), with one warning when reduced.
---@param width number outline width in ems; 0 (the default) turns it off
---@param color Color
function TextRenderer:SetOutline(width, color) end

---@return number width
---@return Color color
function TextRenderer:GetOutline() end

---A shadow under the text (with offset 0 and some softness, a glow). Off while the colour's alpha is 0 (the default).
---@param offsetX number ems, + right
---@param offsetY number ems, + up
---@param color Color
---@param softness number|nil how far its edge fades, in ems (default 0; ignored by the software renderer)
function TextRenderer:SetShadow(offsetX, offsetY, color, softness) end

---@return number offsetX
---@return number offsetY
---@return Color color
---@return number softness
function TextRenderer:GetShadow() end

---@param softness number how far the outer edges fade, in ems (default 0 = crisp; ignored by the software renderer)
function TextRenderer:SetSoftness(softness) end

---@return number
function TextRenderer:GetSoftness() end

---Where the block sits relative to the object's origin. Unknown names raise an error and change nothing.
---@param horizontal HorizontalAlign default "Left"
---@param vertical VerticalAlign default "Top"
function TextRenderer:SetAlignment(horizontal, vertical) end

---@return HorizontalAlign horizontal
---@return VerticalAlign vertical
function TextRenderer:GetAlignment() end

---Draw with a font file; nil goes back to the built-in default font. Raises an error, keeping the
---current font, if the file doesn't load.
---@param path string|nil e.g. "res://fonts/Title.ttf"
function TextRenderer:SetFont(path) end

---The laid-out block in the object's local units (all 0 for empty text)
---@return number minX
---@return number minY
---@return number maxX
---@return number maxY
function TextRenderer:GetBounds() end

---Runs a behaviour script: a file returning a table whose __index is itself, with optional
---OnAttach, OnUpdate, OnFixedUpdate, OnLateUpdate, OnDestroy, OnEnable, OnDisable, collision methods and
---the pointer methods OnMouseEnter, OnMouseOver, OnMouseExit, OnMouseDown, OnMouseDrag, OnMouseUp and
---OnMouseUpAsButton (no arguments; sent before OnUpdate to the object under the cursor, left button only).
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

-- ===== UI =====

-- UI (docs/ui.html). On a screen-space overlay canvas, canvas space is pixels in window units with the origin
-- at the bottom-left and y up (as in Unity), unlike Input.GetMousePosition, which is top-left and y down. On a
-- world-space canvas it is canvas units in the canvas's own rect, placed in the world by its object's transform.

---A rectangle in canvas space: (x, y) is its bottom-left corner
---@class Rect
---@field x number
---@field y number
---@field width number
---@field height number
Rect = {}

---Whether the point (canvas space) is inside, edges included; false for a rect with no area
---@param point Vector2
---@return boolean
function Rect:Contains(point) end

---Where a UI element sits in its parent's rect (Unity's RectTransform). One per object.
---@class RectTransform : Component
RectTransform = {}

---@return Vector2
function RectTransform:GetAnchorMin() end
---@param value Vector2 fractions of the parent rect, (0, 0) its bottom-left
function RectTransform:SetAnchorMin(value) end
---@return Vector2
function RectTransform:GetAnchorMax() end
---@param value Vector2 fractions of the parent rect, (1, 1) its top-right
function RectTransform:SetAnchorMax(value) end
---@return Vector2
function RectTransform:GetPivot() end
---@param value Vector2 fractions of the element's own size
function RectTransform:SetPivot(value) end
---@return Vector2
function RectTransform:GetAnchoredPosition() end
---@param value Vector2 the pivot's offset from the reference point inside the anchor box, in pixels
function RectTransform:SetAnchoredPosition(value) end
---@return Vector2
function RectTransform:GetSizeDelta() end
---@param value Vector2 the size minus the anchor box's size (with equal anchors, the size)
function RectTransform:SetSizeDelta(value) end
---@return Vector2
function RectTransform:GetOffsetMin() end
---Moves the bottom-left corner, keeping the top-right
---@param value Vector2
function RectTransform:SetOffsetMin(value) end
---@return Vector2
function RectTransform:GetOffsetMax() end
---Moves the top-right corner, keeping the bottom-left
---@param value Vector2
function RectTransform:SetOffsetMax(value) end
---Anchors (0, 0)-(1, 1) with zero offsets: covers the parent exactly
function RectTransform:StretchToParent() end
---The rect the last layout resolved (each UI pass and each pointer update); zeros before any
---@return Rect
function RectTransform:GetRect() end

---The root of a UI tree. A screen-space overlay canvas (the default) is drawn over the scene, and higher sort
---orders draw on top and are hit first. A world-space canvas is a rectangle in the world, drawn with the scene's
---transparent objects and hit by the camera's ray.
---@class Canvas : Component
Canvas = {}

---@return integer
function Canvas:GetSortOrder() end
---@param sortOrder integer only used by overlay canvases
function Canvas:SetSortOrder(sortOrder) end
---"WorldSpace" adds a transform (at the origin, scale 1) and a RectTransform if the object has none. An unknown
---name raises an error and changes nothing.
---@param mode "ScreenSpaceOverlay"|"WorldSpace"
function Canvas:SetRenderMode(mode) end
---@return "ScreenSpaceOverlay"|"WorldSpace"
function Canvas:GetRenderMode() end
---@return boolean
function Canvas:IsWorldSpace() end
---A world canvas's size in canvas units: its RectTransform's sizeDelta (100 x 100 without one). Its transform's
---scale is the size of a canvas unit in world units (0.01 from UI.CreateCanvas).
---@return Vector2
function Canvas:GetSize() end
---Sets the RectTransform's sizeDelta, adding a RectTransform if the object has none
---@param size Vector2
function Canvas:SetSize(size) end

---A coloured rectangle over its object's rect, optionally showing a sprite (an image file). A raycast target
---(the default) takes the pointer: its object gets the OnMouse* callbacks and Input.IsPointerOverUI() is true.
---@class Image : Component
Image = {}

---@return Color
function Image:GetColor() end
---@param color Color
function Image:SetColor(color) end
---@return boolean
function Image:GetRaycastTarget() end
---@param raycastTarget boolean false lets the pointer through to what is underneath
function Image:SetRaycastTarget(raycastTarget) end
---Show an image file (PNG, JPEG, TGA or BMP) in the rect, multiplied by the colour; nil clears it. Raises an
---error, keeping the current sprite, if the file doesn't load. Saved with the scene.
---@param path string|nil e.g. "res://ui/icon.png"
function Image:SetSprite(path) end
---The sprite's path ("res://..." for a project asset), or nil without a sprite
---@return string|nil
function Image:GetSprite() end

---Text drawn inside its object's rect, with an SDF font. Sizes are canvas pixels. Unlike Image it is not a
---raycast target by default, so a label lets the pointer through to what is underneath.
---Changing a setting other than the colour, or the rect's width with wrap on, lays the text out again.
---@class UIText : Component
UIText = {}

---@param text string UTF-8; "\n" breaks lines
function UIText:SetText(text) end
---@return string
function UIText:GetText() end
---@return Color
function UIText:GetColor() end
---@param color Color
function UIText:SetColor(color) end
---@return boolean
function UIText:GetRaycastTarget() end
---@param raycastTarget boolean true makes the text take the pointer (default false)
function UIText:SetRaycastTarget(raycastTarget) end
---@param size number canvas pixels per em (default 24)
function UIText:SetFontSize(size) end
---@return number
function UIText:GetFontSize() end
---@param wrap boolean whether lines wrap at the rect's width (default true)
function UIText:SetWrap(wrap) end
---@return boolean
function UIText:GetWrap() end
---@param spacing number multiplies the font's line height (default 1)
function UIText:SetLineSpacing(spacing) end
---@return number
function UIText:GetLineSpacing() end
---@param spacing number extra space between glyphs, in ems (default 0; negative tightens)
function UIText:SetLetterSpacing(spacing) end
---@return number
function UIText:GetLetterSpacing() end
---An outline around the glyphs, drawn under them. Lengths are in ems (fontSize pixels each); effects are
---limited to 90% of the font's SDF spread (about 0.15 em for the default font), with one warning when reduced.
---@param width number outline width in ems; 0 (the default) turns it off
---@param color Color
function UIText:SetOutline(width, color) end
---@return number width
---@return Color color
function UIText:GetOutline() end
---A shadow under the text (with offset 0 and some softness, a glow). Off while the colour's alpha is 0 (the default).
---@param offsetX number ems, + right
---@param offsetY number ems, + up
---@param color Color
---@param softness number|nil how far its edge fades, in ems (default 0; ignored by the software renderer)
function UIText:SetShadow(offsetX, offsetY, color, softness) end
---@return number offsetX
---@return number offsetY
---@return Color color
---@return number softness
function UIText:GetShadow() end
---@param softness number how far the outer edges fade, in ems (default 0 = crisp; ignored by the software renderer)
function UIText:SetSoftness(softness) end
---@return number
function UIText:GetSoftness() end

---Where the block sits inside the rect: Left/Center/Right against its left edge, centre and right edge;
---Top/Middle/Bottom against its top, centre and bottom; Baseline puts the first baseline on the rect's
---vertical centre. Unknown names raise an error and change nothing.
---@param horizontal HorizontalAlign default "Left"
---@param vertical VerticalAlign default "Top"
function UIText:SetAlignment(horizontal, vertical) end

---@return HorizontalAlign horizontal
---@return VerticalAlign vertical
function UIText:GetAlignment() end

---Draw with a font file; nil goes back to the built-in default font. Raises an error, keeping the
---current font, if the file doesn't load.
---@param path string|nil e.g. "res://fonts/Title.ttf"
function UIText:SetFont(path) end

---The laid-out block in canvas space, in the rect of the last layout (all 0 for empty text)
---@return number minX
---@return number minY
---@return number maxX
---@return number maxY
function UIText:GetBounds() end

---A clickable UI element: put it on an object with a raycast-target graphic (an Image). It tints its target
---graphic for its state (the graphic's own colour times the state's colour times the multiplier, so the
---graphic's colour itself never changes) and calls its OnClick listeners when the left button is pressed
---and released over it, if it is interactable and enabled. Listeners are not saved with the scene.
---@class Button : Component
Button = {}

---@return boolean
function Button:IsInteractable() end
---@param interactable boolean false shows the disabled colour and ignores clicks
function Button:SetInteractable(interactable) end
---@return Color
function Button:GetNormalColor() end
---@param color Color default white
function Button:SetNormalColor(color) end
---@return Color
function Button:GetHighlightedColor() end
---@param color Color shown while the pointer is over it (default 245/255 grey)
function Button:SetHighlightedColor(color) end
---@return Color
function Button:GetPressedColor() end
---@param color Color shown while pressed with the pointer over it (default 200/255 grey)
function Button:SetPressedColor(color) end
---@return Color
function Button:GetDisabledColor() end
---@param color Color shown while not interactable (default 200/255 grey at half alpha)
function Button:SetDisabledColor(color) end
---@return number
function Button:GetColorMultiplier() end
---@param multiplier number multiplies every state's colour (default 1)
function Button:SetColorMultiplier(multiplier) end
---@return number
function Button:GetFadeDuration() end
---@param seconds number unscaled seconds a tint change takes (default 0.1; 0 is instant)
function Button:SetFadeDuration(seconds) end
---The tint on the target graphic now (between two states' colours during a fade)
---@return Color
function Button:GetCurrentTint() end
---@return "Normal"|"Highlighted"|"Pressed"|"Disabled"
function Button:GetState() end
---The graphic tinted now, or nil
---@return Image|UIText|nil
function Button:GetTargetGraphic() end
---@param graphic Image|UIText|nil nil goes back to the first graphic on the button's object
function Button:SetTargetGraphic(graphic) end
---Calls fn on every click. An error in fn is logged and the other listeners still run. A listener a
---component script added stops once that component is destroyed.
---@param fn fun()
---@return integer id for RemoveOnClick
function Button:AddOnClick(fn) end
---@param id integer what AddOnClick returned (safe from inside a listener)
function Button:RemoveOnClick(id) end
function Button:ClearOnClick() end
---@return integer
function Button:GetOnClickListenerCount() end
---Calls the listeners as a click does, if interactable and enabled
---@return boolean whether it did
function Button:Click() end

---@class UI
UI = {}

---A new root GameObject on the UI layer with a Canvas (add it to the scene with AddRootGameObject). With mode
---"WorldSpace" it also gets a transform scaled to 0.01 and a 100 x 100 RectTransform (1 x 1 world units).
---An unknown mode raises an error.
---@param name? string defaults to "Canvas"
---@param mode? "ScreenSpaceOverlay"|"WorldSpace" defaults to "ScreenSpaceOverlay"
---@return GameObject
function UI.CreateCanvas(name, mode) end

---A new GameObject on the UI layer with a RectTransform (a 100x100 box at its parent's centre);
---parent it under a canvas or another element
---@param name? string defaults to "UIElement"
---@return GameObject
function UI.CreateElement(name) end

---UI.CreateElement with a UIText showing `text` (24 px, white, top-left, wrapped to the rect)
---@param name? string defaults to "Text"
---@param text? string defaults to ""
---@return GameObject
function UI.CreateText(name, text) end

---UI.CreateElement sized 160x40 with a white Image, a Button, and a child "Label" UIText stretched over it
---and centred (20 px, dark grey) showing `label`
---@param name? string defaults to "Button"
---@param label? string defaults to "Button"
---@return GameObject
function UI.CreateButton(name, label) end

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
---@field AllLayers integer Every layer (0xFFFFFFFF)
---@field DefaultRaycastMask integer Every layer except Ignore Raycast
Layers = {}

---The mask holding just this layer (1 << layer), or 0 for an out-of-range layer
---@param layer integer
---@return integer
function Layers.MaskOf(layer) end

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

---A raycast hit. Safe to keep: its objects are references like any other (see CORE OBJECTS).
---@class RaycastHit
---@field point Vector3
---@field normal Vector3
---@field distance number
---@field gameObject GameObject|nil The hit body's object (its Rigidbody's object for a compound body)
---@field collider Component|nil The collider that was hit, as its own type (BoxCollider, ...)
---@field rigidbody Rigidbody|nil nil for a static collider
RaycastHit = {}

---@class Physics
Physics = {}

---The closest hit along a ray, or nil. Without physics (no backend, or built without PhysX) nothing is hit.
---A zero direction or a negative distance hits nothing.
---@param origin Vector3
---@param direction Vector3 normalised internally
---@param maxDistance number|nil default and maximum (longer ones, math.huge included, are clamped): 1e9
---@param layerMask integer|nil default: Layers.DefaultRaycastMask (every layer except Ignore Raycast); Layers.AllLayers or -1 for every layer
---@return RaycastHit|nil
function Physics.Raycast(origin, direction, maxDistance, layerMask) end

---Every hit along a ray, nearest first (an empty table for none). Arguments as Physics.Raycast.
---@param origin Vector3
---@param direction Vector3
---@param maxDistance number|nil
---@param layerMask integer|nil
---@return RaycastHit[]
function Physics.RaycastAll(origin, direction, maxDistance, layerMask) end

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

---The cursor and mouse buttons, sampled once at the start of each frame. Buttons are 0..7: 0 left, 1 right,
---2 middle (out-of-range buttons read false).
---@class Mouse
Mouse = {}

---The mouse, or nil without a window
---@return Mouse|nil
function Mouse.Get() end

---Cursor position in window coordinates: origin at the top-left, y down
---@return Vector2
function Mouse:GetPosition() end

---@return Vector2
function Mouse:GetPositionDelta() end

---@return Vector2
function Mouse:GetScrollDelta() end

---@param button integer
---@return boolean
function Mouse:GetButton(button) end

---True only in the frame the button went down
---@param button integer
---@return boolean
function Mouse:GetButtonDown(button) end

---True only in the frame the button was released
---@param button integer
---@return boolean
function Mouse:GetButtonUp(button) end

---@class Input
Input = {}

---Cursor position in window coordinates (top-left origin, y down); (0, 0) without a window
---@return Vector2
function Input.GetMousePosition() end

---Whether a mouse button is held (0 left, 1 right, 2 middle); false without a window
---@param button integer
---@return boolean
function Input.GetMouseButton(button) end

---True only in the frame the button went down
---@param button integer
---@return boolean
function Input.GetMouseButtonDown(button) end

---True only in the frame the button was released
---@param button integer
---@return boolean
function Input.GetMouseButtonUp(button) end

---Whether the pointer was over a UI element (a raycast-target graphic under a canvas) in the last pointer update
---@return boolean
function Input.IsPointerOverUI() end

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

---@class Ray
---@field origin Vector3
---@field direction Vector3
Ray = {}

---@param origin Vector3|nil
---@param direction Vector3|nil
---@return Ray
function Ray.new(origin, direction) end

---The point distance units along the ray
---@param distance number
---@return Vector3
function Ray:GetPoint(distance) end

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

---Turn the camera to view a target. The camera looks down its local -Z (objects face +Z), so this points
---the camera's -Z at the target. A target at the camera's position keeps the rotation; an up vector that is zero
---or parallel to the view direction falls back to Vector3.Up, or, within about 8 degrees of vertical, to the
---world Z axis on the side that keeps the screen's up continuous.
---@param target Vector3
---@param up Vector3|nil Optional up vector (default: Vector3.Up)
function Camera:LookAt(target, up) end

---The world direction the camera views: its rotation applied to -Z (unlike Positionable:GetForward, which is +Z)
---@return Vector3
function Camera:GetForward() end

---The world direction of the screen's up: its rotation applied to +Y
---@return Vector3
function Camera:GetUp() end

---The world direction of the screen's right: its rotation applied to +X
---@return Vector3
function Camera:GetRight() end

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

---The ray through a screen point, from the near plane away from the camera (unit direction). Takes window
---coordinates (top-left origin, y down), as Input.GetMousePosition returns. The viewport defaults to the
---window's size; pass both width and height to use another (only one of them is an error).
---  local mouse = Input.GetMousePosition()
---  local ray = Camera.Main():ScreenPointToRay(mouse.x, mouse.y)
---  local hit = Physics.Raycast(ray.origin, ray.direction, 100)
---@param x number
---@param y number
---@param width integer|nil
---@param height integer|nil
---@return Ray
function Camera:ScreenPointToRay(x, y, width, height) end

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
