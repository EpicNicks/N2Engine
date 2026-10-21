---@meta

-- ===========================================================================================================
-- The Editor API of a Lua editor extension (API version 1), as LuaLS annotations.
--
-- This is a definition file for the Lua Language Server (LuaLS), like engine/src/scripting/bindings/engine-api.lua
-- but for a different world: an editor extension is a file <project>/editor/**/*.lua that the editor HOST runs, in a
-- sandbox state of its own, with the `Editor` namespace below and NO engine bindings (no GameObject, no Vector3, no
-- Application). The two files describe different states; point LuaLS at this one for files under editor/ and at
-- engine-api.lua for scripts under assets/. Nothing loads this file: it exists for completion, hover text and type
-- checking only.
--
-- What each function is: the editor-server/src/ExtensionHost.cpp prelude (EditorPrelude) defines every function below,
-- and EditorServer::ExtensionRequest answers it with the same command a client could send. The tests in
-- tests/editor-server/ExtensionApiAnnotationTests.cpp load an extension that declares every capability, list the
-- functions of its `Editor` table, and fail when this file lacks one, has one the host does not register, or names the
-- parameters of one differently. docs/editor-extensions.html explains the model, the capabilities, the budgets and the
-- roles.
--
-- Values are plain Lua tables, strings and numbers (the JSON of the editor protocol), never engine objects: an object's
-- id is the string of its UUID, a vector is { x, y, z }, a rotation is a quaternion { x, y, z, w }. A JSON null in an
-- answer is a missing key. A command the host refuses (an unknown id, a role that may not, a value a field refuses)
-- raises a Lua error at the call, with the host's message; `pcall` can catch it.
-- ===========================================================================================================

-- ===== CAPABILITIES =====

---What a file may ask the host for. A function of a capability the file did not declare does not exist in its state
---(it is nil, not refused). A hook needs the capability of what it reads.
---@alias EditorCapability
---| "scene.read" # Editor.Scene's readers; also needed by the three scene hooks and by inspector sections
---| "scene.write" # Editor.Scene's writers (each is a command sent as the user who invoked the item)
---| "assets.read" # Editor.Assets.List and ReadText; also needed by OnAssetsChanged
---| "assets.write" # Editor.Assets.WriteText and CreateFolder

---@alias EditorRole
---| "editor" # the default: editors and admins may run it
---| "admin" # only an admin may run it (a viewer never may)

-- ===== VALUES =====

---The UUID of an object, written 8-4-4-4-12 hex digits
---@alias EditorEntityId string

---The UUID of a component
---@alias EditorComponentId string

---A res:// path: res:// then a path inside the project's assets/ folder
---@alias EditorAssetPath string

---@class EditorVector3
---@field x number
---@field y number
---@field z number

---@class EditorQuaternion
---@field x number
---@field y number
---@field z number
---@field w number

---An object's transform relative to its parent
---@class EditorLocalTransform
---@field position EditorVector3
---@field rotation EditorQuaternion
---@field scale EditorVector3

---What Editor.Scene.SetLocalTransform takes: a part left out keeps the object's own value; a part given must be complete
---@class EditorLocalTransformChange
---@field position? EditorVector3
---@field rotation? EditorQuaternion
---@field scale? EditorVector3

---One object of Editor.Scene.GetHierarchy (the protocol's HierarchyNode)
---@class EditorHierarchyNode
---@field id EditorEntityId
---@field parentId string The parent's id, "" for a root of the scene
---@field index integer Its place among its siblings
---@field name string
---@field active boolean The object's own flag
---@field activeInHierarchy boolean
---@field layer integer 0 to 31
---@field tag string
---@field components string[] The type names of its components

---An object's own properties (the protocol's EntityHeader)
---@class EditorEntityHeader
---@field id EditorEntityId
---@field parentId string "" for a root
---@field index integer
---@field name string
---@field active boolean
---@field activeInHierarchy boolean
---@field layer integer
---@field tag string

---A component of Editor.Scene.GetEntity
---@class EditorEntityComponent
---@field type string The component's type name
---@field uuid EditorComponentId
---@field values table<string, any> The component's saved field values

---What Editor.Scene.GetEntity returns (the protocol's EntityDetails)
---@class EditorEntityDetails
---@field header EditorEntityHeader
---@field transform? EditorLocalTransform Absent for an object without a transform
---@field components EditorEntityComponent[]

---One field of a component type or of a form (the protocol's FieldSchema)
---@class EditorFieldSchema
---@field name string
---@field displayName string
---@field kind string
---@field typeName string
---@field hidden boolean
---@field readOnly boolean
---@field enumOptions? string[]
---@field assetType? string
---@field min? number
---@field max? number
---@field tooltip? string
---@field container? string

---One component type of Editor.Scene.GetComponentTypes (the protocol's ComponentSchema)
---@class EditorComponentSchema
---@field typeName string
---@field singleton boolean
---@field fields EditorFieldSchema[]
---@field defaults? table<string, any>

---@class EditorSubAsset
---@field key string
---@field uuid string
---@field type string

---One asset of Editor.Assets.List (the protocol's AssetInfo)
---@class EditorAssetInfo
---@field path EditorAssetPath
---@field uuid string
---@field type string
---@field size number
---@field modified number Unix milliseconds
---@field subAssets? EditorSubAsset[]

---What Editor.Assets.List returns: what is directly in the folder
---@class EditorAssetListing
---@field folders string[] res:// paths of the folders in it
---@field assets EditorAssetInfo[] The indexed assets in it

-- ===== WHAT AN EXTENSION DECLARES =====

---@alias EditorParamType
---| "number" # a float field
---| "int" # a whole number
---| "string"
---| "bool"
---| "Vector3" # { x, y, z }
---| "GameObject" # an object id; has no default
---| "enum" # needs `options`

---One field of a form: the value of a `params` table, keyed by the field's name (letters, digits and _, at most 64 bytes)
---@class EditorParam
---@field type EditorParamType
---@field default? any Of the type, inside min and max; a GameObject has none. When absent: the type's own (0, "", false, a zero vector, the first option), or min if it is above zero, else max if it is below zero
---@field min? number number and int only; a whole number for an int
---@field max? number number and int only; a whole number for an int
---@field tooltip? string At most 200 bytes
---@field options? string[] enum only: 1 to 64 distinct strings of at most 64 bytes
---@field displayName? string At most 64 bytes

---A section's field: the shape of a form field without a default (the inspector shows the component's own value)
---@class EditorSectionField
---@field type EditorParamType
---@field min? number
---@field max? number
---@field tooltip? string
---@field options? string[]
---@field displayName? string

---@class EditorEnabledWhen
---@field selection? "any"|"nonEmpty"|"one"|"none" Checked by the client; "any" when absent
---@field componentType? string

---What a menu item's run function receives as `ctx`
---@class EditorMenuContext
---@field itemId string
---@field selection EditorEntityId[] The objects the client had selected (at most 4096)

---What an inspector button's run function receives as `ctx`
---@class EditorInspectorContext
---@field actionId string
---@field componentId EditorComponentId The component the inspector shows (the host checked it is of the section's type)
---@field entityId EditorEntityId The object that has it

---What a menu item or an inspector button may return (or return nothing): anything else fails the call
---@class EditorResult
---@field message? string Shown to the user; cut to 1024 bytes
---@field select? EditorEntityId[] Objects the client should select (at most 4096)
---@field warnings? string[] Shown to the user (at most 100, each cut to 1024 bytes)

---What OnBeforePlay may return (or nothing): no other key
---@class EditorHookResult
---@field message? string
---@field warnings? string[]

---A menu item: an entry of the editor's Tools menu
---@class EditorMenuItem
---@field id string Letters, digits, '.', '_' and '-', starting with a letter or digit, at most 64 bytes; unique across the whole host
---@field path string Segments separated by '/', at most 6, each 1 to 64 bytes; the last is the title unless `title` is given
---@field title? string At most 100 bytes; also the label of the undo step
---@field minRole? EditorRole "editor" when absent
---@field enabledWhen? EditorEnabledWhen
---@field confirm? string A question the client asks first (at most 200 bytes)
---@field params? table<string, EditorParam> A form: at most 32 fields; the client fills it, the host checks it and clamps it to min and max
---@field run fun(ctx: EditorMenuContext, params: table<string, any>): EditorResult? Runs on a worker as ONE undo step; a Lua error takes every change back

---@class EditorInspectorButton
---@field id string The rules of a menu item's id; unique among the inspector buttons of the whole host
---@field label string At most 100 bytes; also the label of the undo step
---@field minRole? EditorRole
---@field params? table<string, EditorParam>
---@field run fun(ctx: EditorInspectorContext, params: table<string, any>): EditorResult?

---@class EditorInspectorSection
---@field label string At most 100 bytes
---@field fields? table<string, EditorSectionField> Fields of the component the client shows with their values
---@field buttons? EditorInspectorButton[] At most 16; a section needs fields or buttons

---@class EditorInspectorSpec
---@field sections EditorInspectorSection[] At least one

---What a hook receives for OnSceneOpened, OnSceneSaved and OnBeforePlay
---@class EditorSceneInfo
---@field scene string The scene's res:// path ("" for a new scene that has no file yet; the scene asked to play, or the open one, for OnBeforePlay)

---The hooks a file may declare, each called by the host after the work that caused it, read-only, with a smaller budget
---@class EditorHooks
---@field OnSceneOpened? fun(info: EditorSceneInfo) After OpenScene or NewScene made a scene the open one; needs scene.read
---@field OnSceneSaved? fun(info: EditorSceneInfo) After SaveSceneToFile wrote the file; needs scene.read
---@field OnBeforePlay? fun(info: EditorSceneInfo): EditorHookResult? Before the play snapshot is written; its warnings are shown and never block play; needs scene.read
---@field OnAssetsChanged? fun(added: string[], removed: string[], modified: string[]) After assets changed (each list of res:// paths is cut to 1000); needs assets.read

---The table a file gives Editor.Extension and returns. A key this API version does not know fails the file.
---@class EditorExtensionDeclaration
---@field name string 1 to 64 bytes of text with no path characters ('/', '\', ':', '..') and no space or '.' at either end
---@field apiVersion integer Must be 1
---@field capabilities? EditorCapability[] At most 8, none twice
---@field menu? EditorMenuItem[] At most 64
---@field hooks? EditorHooks

-- ===== THE EDITOR NAMESPACE =====

---The only global an extension has besides the libraries. Scene and Assets exist only after the file called
---Editor.Extension with the capabilities that install them.
---@class EditorApi
---@field Log EditorLogApi
---@field Inspector EditorInspectorApi
---@field Scene EditorSceneApi Declared with scene.read and/or scene.write; absent without either
---@field Assets EditorAssetsApi Declared with assets.read and/or assets.write; absent without either
Editor = {}

---Declares the extension. Call it once, at the top level of the file, and return what it returns: the file must return
---the very table Editor.Extension gave it. It checks only that it was given a table; the host checks the shape after the
---file ran, and a wrong shape fails that file alone.
---@param declaration EditorExtensionDeclaration
---@return EditorExtensionDeclaration
function Editor.Extension(declaration) end

-- ===== LOG =====

---@class EditorLogApi
Editor.Log = {}

---Writes a line to the host's log and to the clients' console (an extensionOutput event that only an admin receives).
---The arguments are joined with spaces; a line written while the file loads is dropped. print(...) is the same as Info.
---@param ... any
function Editor.Log.Info(...) end

---@param ... any
function Editor.Log.Warn(...) end

---@param ... any
function Editor.Log.Error(...) end

-- ===== INSPECTOR =====

---@class EditorInspectorApi
Editor.Inspector = {}

---Adds inspector sections for a component type. Can be called only while the file loads (at most 64 times); a call from a
---running item, button or hook is a Lua error. The file must declare scene.read.
---@param componentType string A component type name: letters, digits and '_', not starting with a digit
---@param spec EditorInspectorSpec
function Editor.Inspector.Register(componentType, spec) end

-- ===== SCENE =====

---Reads and changes the open scene. Every function sends a command of the editor host as the user who invoked the item,
---so what that user may not do is refused. The writers are all part of the one undo step of the item or button.
---@class EditorSceneApi
Editor.Scene = {}

---Every object of the scene, parents before children. Needs scene.read.
---@return EditorHierarchyNode[]
function Editor.Scene.GetHierarchy() end

---One object's properties, transform and components. Needs scene.read.
---@param id EditorEntityId
---@return EditorEntityDetails
function Editor.Scene.GetEntity(id) end

---A component's saved field values. Needs scene.read.
---@param entityId EditorEntityId
---@param componentId EditorComponentId
---@return table<string, any>
function Editor.Scene.GetComponent(entityId, componentId) end

---An object's transform relative to its parent; an object without a transform is at the origin with the identity rotation
---and a scale of 1. Needs scene.read.
---@param id EditorEntityId
---@return EditorLocalTransform
function Editor.Scene.GetLocalTransform(id) end

---The ids of every object with exactly this name, in hierarchy order. Needs scene.read.
---@param name string
---@return EditorEntityId[]
function Editor.Scene.FindByName(name) end

---The component types the editor knows, sorted by type name. Needs scene.read.
---@return EditorComponentSchema[]
function Editor.Scene.GetComponentTypes() end

---What CreateEntity takes; every key is optional and another key is an error
---@class EditorCreateEntitySpec
---@field name? string
---@field parent? EditorEntityId A root of the scene when absent
---@field components? (string|{type: string, values?: table<string, any>})[] Type names, or a type with the values to set

---Makes an object (with a transform, last among its siblings) and returns its id. A call that fails half-way takes back
---what it already did. Needs scene.write.
---@param spec? EditorCreateEntitySpec
---@return EditorEntityId
function Editor.Scene.CreateEntity(spec) end

---Destroys an object and everything under it. Needs scene.write.
---@param id EditorEntityId
function Editor.Scene.Destroy(id) end

---Copies an object and everything under it and returns the copy's id. Needs scene.write.
---@param id EditorEntityId
---@return EditorEntityId
function Editor.Scene.Duplicate(id) end

---Moves an object under another (nil: a root of the scene); it keeps its place in the world. Needs scene.write.
---@param id EditorEntityId
---@param parent? EditorEntityId
---@param index? integer Its place among the new siblings; last when nil
function Editor.Scene.SetParent(id, parent, index) end

---@class EditorEntityProperties
---@field name? string
---@field tag? string
---@field layer? integer 0 to 31
---@field active? boolean

---Changes an object's own properties; only the keys given change. Needs scene.write.
---@param id EditorEntityId
---@param properties EditorEntityProperties
function Editor.Scene.SetProperties(id, properties) end

---Changes an object's transform; a part left out keeps the object's value. Numbers go out as 32-bit floats and a value
---that is not finite is refused. Needs scene.write.
---@param id EditorEntityId
---@param transform EditorLocalTransformChange
function Editor.Scene.SetLocalTransform(id, transform) end

---Adds a component and, when values are given, sets them right after; returns the new component's id. All or nothing.
---Needs scene.write.
---@param id EditorEntityId
---@param typeName string
---@param values? table<string, any>
---@return EditorComponentId
function Editor.Scene.AddComponent(id, typeName, values) end

---Removes a component. Needs scene.write.
---@param entityId EditorEntityId
---@param componentId EditorComponentId
function Editor.Scene.RemoveComponent(entityId, componentId) end

---Sets some fields of a component (only the keys given change) and returns its values as stored. Needs scene.write.
---@param entityId EditorEntityId
---@param componentId EditorComponentId
---@param values table<string, any>
---@return table<string, any>
function Editor.Scene.SetFields(entityId, componentId, values) end

-- ===== ASSETS =====

---Reads and writes the project's text assets, through the editor's own asset commands (res:// paths only, the same size
---caps and text rules, the same role and read-only checks). A write is a file, not a scene edit: it is not part of the
---undo step and a failed invocation does not take it back.
---@class EditorAssetsApi
Editor.Assets = {}

---The folders and the indexed assets directly in a folder (not what is below them). A folder with more than 10000
---entries is an error. Needs assets.read.
---@param folder? EditorAssetPath "res://" when nil
---@return EditorAssetListing
function Editor.Assets.List(folder) end

---A text asset's text (at most 4 MiB, valid UTF-8). Needs assets.read.
---@param path EditorAssetPath
---@return string
function Editor.Assets.ReadText(path) end

---Writes a text asset atomically; the host reloads it and tells the clients. Refuses the open scene's own file and a
---folder that does not exist. Not available to a hook. Needs assets.write.
---@param path EditorAssetPath
---@param text string
function Editor.Assets.WriteText(path, text) end

---Makes a folder (missing parents too); one that exists is fine. Not available to a hook. Needs assets.write.
---@param path EditorAssetPath
function Editor.Assets.CreateFolder(path) end
