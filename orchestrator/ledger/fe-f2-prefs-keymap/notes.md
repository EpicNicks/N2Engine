Accepted by the user 2026-10-13:
- All shortcut sites migrated (roadmap text said only global ones; the brief said every site); widget-local keys (tab bar, context menu, dialogs, field Enter/Escape, live-game forwarding, fly keys) stay with their widgets
- Fly keys and Shift boost stay in the view's own listener (ViewportKeyboard) and win by preventDefault; only F and Esc are keymap actions
- Modifiers matched exactly: Shift+F, Ctrl+Down, Shift+Delete etc. no longer fire; Mod matches Ctrl or Cmd on every platform as before
- An open dialog now also suppresses hierarchy, viewport, text-editor and import-settings keys; Ctrl+S default is no longer prevented under a dialog
- Layout stored as n2.prefs.{user|project.<id>}.layout[.default] in {version,value}; layout JSON unchanged; old n2.layout.* read as fallback, never written, removed by reset/forget; stack id is an additive field (LayoutVersion stays 1)
- Existing tests edited: layout tests read the new key, one broken-storage test now expects the session keeps a saved default, Layout literal got ids, Window menu list gained Keyboard shortcuts, DOM helper installs the dispatcher
- editShortcutOf and shortcutFor kept in code as the parity reference for the keymap defaults (unused by the page)
- Action ids (text.save, assets.applySettings, hierarchy.extendNext...) and the Window-menu location of the shortcuts list are my choices; Edit menu labels still show default chords
