Accepted by the user 2026-10-13:
- Assets panel has no asset rename or delete, so those plan flows are untested; the prompt flow is covered via + Folder, + Script and the folder menu New items
- Store is a stub with real signals (not a real EditorStore); read-only on hierarchy driven via HierarchyState.setReadOnly, host read-only via stub hostReadOnly (followWriteGuard already covered in read-only.test.tsx)
- Drag and drop tested via hand-made dataTransfer (hierarchy drop, asset dragStart); drop position is always inside because jsdom has no layout
