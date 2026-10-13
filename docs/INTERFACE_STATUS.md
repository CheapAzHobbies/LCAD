# Where the interface stands against the GUI specification

[GUI_SPEC.md](GUI_SPEC.md) arrived on 2026-10-12, as Phase 1 was being finished. This page lists,
section by section, what LCAD has of it and what is missing, with the phase each gap is
scheduled for. It is updated at the end of every phase.

Status as of the end of Phase 1b (2026-10-12), the interface shell phase that this specification
added to the [roadmap](../ROADMAP.md).

| Section | Have | Missing, and when |
|---|---|---|
| 1. Originality | LCAD's own application icon, document icon, and tool icons (line drawings in one style, drawn in the theme's colors). Layout, colors, and controls are LCAD's own, built from general CAD conventions. | The orientation cube is OCCT's stock one, in the theme's colors. An orientation control of LCAD's own design: Phase 3, with the other in-view controls. |
| 2. Visual direction | Design tokens (`Theme.h`) for every surface, text, state, and viewport color, and for radius, spacing, toolbar height, and icon size, in a light and a dark set. The palette, the style sheet, the icons, and the 3D view all take their values from them. One accent color. | Type sizes follow the system's font, with one smaller size for group labels; a fuller scale when dense panels need it. The labels in the 3D view use the fonts OCCT finds, not the interface's. |
| 3. Main window | Header, toolbar, browser left, viewport center, properties right, history strip bottom, status bar. Panels resize, hide, float, and re-dock. The layout is remembered between launches, and Window > Reset Layout restores the default. The viewport has more than half the window at 1280 by 800. | |
| 4. Header | Menus; new, open, save, undo, redo with tooltips; document tabs with an unsaved mark; command search; warning before closing unsaved work; Ctrl+N, O, S, Z, Shift+Z. No sign-in exists. | Workspace selector, when a second workspace exists (Phase 5). Update notification: when there are releases to be notified of. |
| 5. Workspaces | One design workflow; nothing needs a mode switch. | Further workspaces arrive with their phases (assembly 5, drawing and manufacture 6). |
| 6. Toolbar | Create, Select, and View groups, with icons and labels; every tool has a tooltip and is found by the command search. | The Modify, Construct, and Inspect groups appear with their first tools (Phases 2 and 3); contextual switching with sketch mode (Phase 2); collapsing groups at narrow widths, when there are enough tools to need it. |
| 7. Browser | Document root, origin planes and axes, bodies; expand, select, multiselect, rename inline, show and hide, isolate, show all, filter by name, context menu, delete. Actions that do not apply are not offered. | Sketches, features, suppression, failed-feature marks: Phases 2 and 3. Components and drag-and-drop: Phase 5. |
| 8. Viewport | Orbit, pan, zoom, fit, zoom to selection, standard and isometric views, both projections, orientation cube that can be clicked or dragged; two navigation presets and a reversible wheel; selection of bodies, faces, edges, vertices with hover outline and selection tint; selection filters; rectangle selection; choosing among overlapping objects (Select Other); reference grid, axis marker, origin guides, each switchable; display styles: shaded with edges, shaded, wireframe, hidden line, transparent; light and dark colors. | Controls mounted on the viewport's edges: display style, grid, and fit are in the toolbar and the View menu instead. Phase 3, with section analysis. Silhouette edges drawn differently from inner edges. |
| 9. Properties panel | A right-side panel that holds the command in progress (live preview, validated numbers, errors in plain words, Enter confirms, Esc cancels) and otherwise the selection: a body's name and visibility, editable, and its volume, area, size, center of mass, and counts; a face's area, an edge's length, a vertex's position. Its width does not change with its content. | Material and mass: Phase 5. |
| 10. Timeline | The strip exists and is empty. Nothing fake is shown in it. It can be hidden from the Window menu. | Everything else in this section: Phase 3, with the feature history it represents. |
| 11. Command search | S or Ctrl+K. Partial names, aliases, groups, keyboard navigation, Enter, Esc, recent commands. Commands that cannot be started now are listed last and cannot be run. It starts the real commands. | Ranking by the kind of thing selected, when there are enough commands for it to matter (Phase 3). |
| 12. Context menus | Browser: rename, hide or show, isolate, delete. Viewport: Select Other, zoom to selection, hide, isolate, delete, show all, fit, home. Only what applies is listed. | Create sketch on a face, modify a face: with those commands (Phases 2 and 4). A marking menu is not planned for now. |
| 13. Sketch interface | | Phase 2. |
| 14. In-canvas numbers | | Phases 2 and 3, with the sketch and feature tools that need them. |
| 15. Tabs | Several documents in tabs: switch, new, close, unsaved mark, each with its own camera, undo history, and origin guides. A file is never opened twice. Recent files, with their folders. | Thumbnails in the recent list (the files already carry them). |
| 16. File dialogs | New, open, save, save as, with the `.lcad` filter; the last folder is remembered. Files go where the user says and nowhere else. | Import and export: Phase 4. Package project: Phase 5. |
| 17. Notifications | Status bar: what is selected and its measurement, the command in progress, the units, and brief messages for saved and opened. Modal dialogs only for unsaved changes and for errors that stop an action. | A progress display, when there is an operation long enough to need one (Phase 3). |
| 18. Themes | Light and dark, from central tokens, checked for contrast by a test. The choice is remembered. "Follow the System" is the default. | |
| 19. Layout | Resize, hide, re-dock, remember, reset; hide the timeline; Focus on 3D View (F11). | Compact and comfortable density modes: with the first dense panel, in Phase 3. |
| 20. Mouse and keyboard | Standard shortcuts, Esc, Enter, Delete, F2, undo and redo, tab order through panel fields. Window > Keyboard Shortcuts changes any shortcut and refuses one already in use. | |
| 21. Window sizes | Tested at 1024 by 700, 1280 by 800, and 1600 by 1000: nothing overlaps, nothing is clipped. | Collapsing toolbar groups below about 1000 pixels of width. |
| 22. Accessibility | Qt's standard widgets throughout, so keyboard access to menus, focus indicators, and screen-reader roles come with them; tooltips on icon-only buttons; unsaved work, visibility, and command state are shown by marks and check boxes, not by color alone; View > Animate View Changes turns the one animation off. Drawn correctly at twice the display scale. | A scale setting of LCAD's own; today it follows the desktop's. |
| 23. Animation | Only the view cube's camera turn, which can be switched off. Nothing waits for an animation. | |
| 24. Architecture | A command registry: each command is defined once, with its name, aliases, shortcuts, icon, and the conditions under which it applies, and the toolbar, menus, context menus, shortcuts, and search all use that definition. The shell, tabs, browser, viewport, properties, search, themes, and icons are separate classes that reach the model only through the document's interface. | Commands with their own parameters and validation as registry objects (today that lives in the command's panel): Phase 2, when the sketch tools need a common shape for it. |
| 25. Working prototype | Steps 1 to 3 and 14 to 16: launch, new document, the regions, save, close, reopen, continue editing. | Steps 4 to 13 need the sketcher (Phase 2) and extrude, fillet, and the timeline (Phase 3). The prototype is complete at the end of Phase 3. |
| 26. GUI testing | Automated interface tests for all of the above that exists, run at the screen's scale and again at twice that, in both themes and at three window sizes: overlap and clipping, keyboard handling, context menus, selection, tabs, unsaved marks, undo and redo, state after cancel, and what reaches the screen. | A document with many features and components, when there are features and components. |

## What the specification changes in the plan

1. **An interface shell phase before the sketcher (Phase 1b), now done.** The specification says
   to build the shell and design system first and connect commands to it as they become real.
   The sketcher adds some twenty commands; they will land in a command registry with search,
   themes, and icons already in place.
2. **The name.** The specification says ForgeCAD throughout. The project is LCAD, and nothing in
   the repository uses the other name.
3. **Originality.** The brief says "Fusion-like"; the specification adds that LCAD must not look
   like a copy. What carries over from Fusion is the workflow and the mouse mapping, which are
   conventions, not artwork. Icons, colors, and the orientation control are to be LCAD's own.
