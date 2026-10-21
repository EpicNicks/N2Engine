-- AlignTools.lua: an example editor extension for N2Engine.
--
-- It adds three items to the editor's Tools menu (Tools > Align) that work on the LOCAL positions of the selected
-- objects, and one hook. Each item has a small form: the editor shows it, the host checks what the user typed against it,
-- and this file receives the checked values as `p`. Every change goes through Editor.Scene, so one item is one undo step
-- and an error anywhere in it takes every change back.
--
-- This file is OFF by default. It is not an asset (it lives in the project's editor/ folder, beside assets/), no game and
-- no play host ever loads it, and the editor host loads it only when it was started with --editor-extensions (the
-- Electron editor passes that flag for a local project only after the user trusts the project folder). It declares only
-- scene.read and scene.write: it cannot touch the project's assets, and it has no engine bindings.
--
-- How to read it: docs/editor-extensions.html (the model, the capabilities, the budgets, the roles) and
-- editor-server/editor-api.lua (the annotations of every function used here).

-- A call may send at most 10000 commands to the host. Reading an object takes one and moving it takes two, so a selection
-- this large is refused with a warning instead of failing half-way
local MAX_OBJECTS = 2000

local function tooMany(ctx)
  if #ctx.selection > MAX_OBJECTS then
    return {
      warnings = { string.format("Select at most %d objects (this selection has %d).", MAX_OBJECTS, #ctx.selection) },
    }
  end
  return nil
end

-- The selected objects as { id, index, position }, in selection order
local function readPositions(ids)
  local entries = {}
  for i, id in ipairs(ids) do
    entries[i] = { id = id, index = i, position = Editor.Scene.GetLocalTransform(id).position }
  end
  return entries
end

-- Moves one object along one axis; a transform names only what it changes, so the rest of it stays
local function moveAlong(entry, axis, value)
  local p = entry.position
  local moved = { x = p.x, y = p.y, z = p.z }
  moved[axis] = value
  Editor.Scene.SetLocalTransform(entry.id, { position = moved })
end

local AXIS = { type = "enum", options = { "x", "y", "z" }, default = "x", tooltip = "The axis to work along" }

return Editor.Extension {
  name = "Align Tools",
  apiVersion = 1,
  capabilities = { "scene.read", "scene.write" },
  menu = {
    {
      id = "align.align",
      path = "Tools/Align/Align selection",
      enabledWhen = { selection = "nonEmpty" },
      params = {
        axis = AXIS,
        mode = {
          type = "enum",
          options = { "min", "center", "max" },
          default = "center",
          tooltip = "Move every object to the lowest, the middle or the highest position on the axis",
        },
      },
      run = function(ctx, p)
        local refused = tooMany(ctx)
        if refused then
          return refused
        end
        local entries = readPositions(ctx.selection)
        if #entries < 2 then
          return { message = "Select two or more objects to align." }
        end
        local low = entries[1].position[p.axis]
        local high = low
        for _, entry in ipairs(entries) do
          local value = entry.position[p.axis]
          if value < low then
            low = value
          end
          if value > high then
            high = value
          end
        end
        local target = low
        if p.mode == "max" then
          target = high
        elseif p.mode == "center" then
          target = (low + high) / 2
        end
        for _, entry in ipairs(entries) do
          moveAlong(entry, p.axis, target)
        end
        return {
          message = string.format("Aligned %d objects on %s (%s).", #entries, p.axis, p.mode),
          select = ctx.selection,
        }
      end,
    },
    {
      id = "align.distribute",
      path = "Tools/Align/Distribute selection evenly",
      enabledWhen = { selection = "nonEmpty" },
      params = { axis = AXIS },
      run = function(ctx, p)
        local refused = tooMany(ctx)
        if refused then
          return refused
        end
        local entries = readPositions(ctx.selection)
        if #entries < 3 then
          return { message = "Select three or more objects to distribute." }
        end
        -- The two ends stay where they are; the ones between are spread evenly, in their order along the axis (objects at
        -- the same position keep their selection order, so the result does not depend on the sort)
        table.sort(entries, function(a, b)
          local left, right = a.position[p.axis], b.position[p.axis]
          if left ~= right then
            return left < right
          end
          return a.index < b.index
        end)
        local first = entries[1].position[p.axis]
        local last = entries[#entries].position[p.axis]
        local step = (last - first) / (#entries - 1)
        for i = 2, #entries - 1 do
          moveAlong(entries[i], p.axis, first + step * (i - 1))
        end
        return {
          message = string.format("Distributed %d objects on %s.", #entries, p.axis),
          select = ctx.selection,
        }
      end,
    },
    {
      id = "align.snapToGrid",
      path = "Tools/Align/Snap selection to grid",
      enabledWhen = { selection = "nonEmpty" },
      params = {
        grid = { type = "number", default = 1.0, min = 0.01, max = 100, tooltip = "Grid size in metres" },
        snapX = { type = "bool", default = true },
        snapY = { type = "bool", default = false },
        snapZ = { type = "bool", default = true },
      },
      run = function(ctx, p)
        local refused = tooMany(ctx)
        if refused then
          return refused
        end
        local function snap(value)
          return math.floor(value / p.grid + 0.5) * p.grid
        end
        for _, entry in ipairs(readPositions(ctx.selection)) do
          local position = entry.position
          Editor.Scene.SetLocalTransform(entry.id, {
            position = {
              x = p.snapX and snap(position.x) or position.x,
              y = p.snapY and snap(position.y) or position.y,
              z = p.snapZ and snap(position.z) or position.z,
            },
          })
        end
        return {
          message = string.format("Snapped %d objects to a grid of %s.", #ctx.selection, tostring(p.grid)),
          select = ctx.selection,
        }
      end,
    },
  },
  hooks = {
    -- Called by the host after a scene was saved, read-only and with a small budget (it needs scene.read, declared above).
    -- A hook cannot change anything: this one only says so in the log.
    OnSceneSaved = function(info)
      Editor.Log.Info("Align Tools: saved " .. info.scene)
    end,
  },
}
