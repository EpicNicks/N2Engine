# Notes (inspector-pass-list, 2026-10-10)
- The frontend has no vitest and no DOM test setup: only pure helpers are tested (node:test). A DOM test harness (jsdom or similar) would be a separate item; until then the PassListEditor component is checked by review only, and has never run against a live host.
- Dispatch is on typeName "TextPass[]" (engine PR #111). Against an older engine the field stays the raw JSON textarea.
- Orders above 0 are clamped by the engine and not supported yet; the editor shows a hint and clamps nothing.
