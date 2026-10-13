# Where the interface stands against the GUI specification

[GUI_SPEC.md](GUI_SPEC.md) arrived on 2026-10-12, as Phase 1 was being finished. Phase 1 was
built to the [project brief](PROJECT_BRIEF.md), which asks for the same overall layout, so much
of the specification's structure is already there. Its design system, command search, tabs, and
several viewport controls are not. This page lists both, section by section, and says where each
gap is scheduled. It is updated at the end of every phase.

Status as of the end of Phase 1 (2026-10-12). "Phase 1b" is the interface shell phase in the
[roadmap](../ROADMAP.md), which exists because of this specification.

| Section | Have | Missing, and when |
|---|---|---|
| 1. Originality | Own application icon. Layout and controls built from general CAD conventions, with nothing taken from another product. | Own icons for LCAD's tools: the toolbar shows text, and the top bar borrows the desktop theme's icons. A document icon. Phase 1b. The view cube is OCCT's stock one; restyle or replace in Phase 1b. |
| 2. Visual direction | Compact layout; the viewport dominates. | Central design tokens and a typography scale. Colors are currently Qt's defaults plus a few constants in the viewport. The labels in the 3D view use whichever sans-serif and monospace fonts OCCT finds, not the interface's font. Phase 1b. |
| 3. Main window | Header, toolbar, browser left, viewport center, command panel right, history area bottom, status bar. Panels resize, hide, and float. | Layout is not remembered between launches; no reset-layout command. Phase 1b. |
| 4. Header | Menus; new, open, save, undo, redo with tooltips; document name and unsaved state; warning before closing unsaved work; Ctrl+N, O, S, Z, Shift+Z. No sign-in exists. | Document tabs, command search, workspace selector. Phase 1b; the workspace selector when a second workspace exists (Phase 5). |
| 5. Workspaces | One design workflow; nothing needs a mode switch. | Further workspaces arrive with their phases (assembly 5, drawing and manufacture 6). |
| 6. Toolbar | Create group with the primitives; Select group. | Icons with optional labels; the Modify, Construct, and Inspect groups appear with their first tools (Phases 2 and 3); contextual switching with sketch mode (Phase 2). |
| 7. Browser | Document root, bodies; expand, select, multiselect, rename inline, show and hide, context menu, delete. | Isolate, reveal hidden, filter by name, origin planes and axes: Phase 1b. Sketches, features, suppression, failed-feature marks: Phases 2 and 3. Components and drag-and-drop: Phase 5. |
| 8. Viewport | Orbit, pan, zoom, fit, standard and isometric views, both projections, orientation cube; selection of bodies, faces, edges, vertices with hover outline and selection tint; selection filters; rectangle selection. | Reference grid and its visibility, display styles (wireframe, hidden line, transparent), zoom to selection, choosing among overlapping objects, navigation presets other than the default, edge-mounted view controls: Phase 1b. Section analysis: Phase 3 with the inspect tools. |
| 9. Properties panel | A right-side panel for the command in progress: live preview, validated numeric fields, errors in plain words, Enter confirms, Esc cancels, values kept while editing. | Showing the properties of what is selected when no command runs (name, visibility, measurements): Phase 1b. Material: Phase 5. |
| 10. Timeline | The area exists and is empty. Nothing fake is shown in it. | Everything in this section: Phase 3, with the feature history it represents. |
| 11. Command search | | Phase 1b, on top of the command registry. |
| 12. Context menus | Browser: rename, show or hide, delete. | Viewport context menu, driven by the selection: Phase 1b. |
| 13. Sketch interface | | Phase 2. |
| 14. In-canvas numbers | | Phases 2 and 3, with the sketch and feature tools that need them. |
| 15. Tabs | One document per window. | Tabs and a recent-files list: Phase 1b. |
| 16. File dialogs | New, open, save, save as, with the `.lcad` filter. Files go where the user says and nowhere else. | Remembering the last directory: Phase 1b. Import and export: Phase 4. Package project: Phase 5. |
| 17. Notifications | Status bar with units and a measurement of the selection; modal dialogs only for unsaved changes and for errors that stop an action. | Brief "saved" style messages, the active command, and progress in the status bar: Phase 1b. |
| 18. Themes | | Light and dark themes from central tokens, following the system by default, choice remembered: Phase 1b. |
| 19. Layout | Panels resize, hide, and re-dock. | Remembering the layout, reset, collapsing the timeline, a focus-viewport mode, density modes: Phase 1b. |
| 20. Mouse and keyboard | Standard shortcuts, Esc, Enter, Delete, F2, undo and redo, tab order through panel fields. | Configurable shortcuts: Phase 1b. |
| 21. Window sizes | Usable at 1280 by 800, the size it is tested at. | Checked behavior at small and large sizes and at other scale factors: Phase 1b. |
| 22. Accessibility | Qt's standard widgets throughout, so keyboard access to menus, focus indicators, and screen-reader roles come with them; tooltips on icon-only buttons; visibility is a check box, not a color. | Interface scaling setting, reduced motion (the view cube animates): Phase 1b. |
| 23. Animation | Only the view cube's camera turn. Nothing waits for an animation. | |
| 24. Architecture | The shell, browser, viewport, and command panel are separate classes that reach the model only through the document's interface. No geometry in button handlers: a command builds a shape through the kernel and commits it in a transaction. | A command registry. Today each command is a `QAction` wired by hand in the main window, which is exactly what this section rules out for the long run. Phase 1b, before the sketch tools multiply the number of commands. |
| 25. Working prototype | Steps 1 to 3 and 14 to 16: launch, new document, the regions, save, close, reopen, continue editing. | Steps 4 to 13 need the sketcher (Phase 2) and extrude, fillet, and the timeline (Phase 3). The prototype is complete at the end of Phase 3. |
| 26. GUI testing | Automated interface tests for layout order, keyboard handling, selection, undo and redo, unsaved-state indicator, state after cancel, and what reaches the screen. | Tests at several resolutions and scale factors, for both themes, for tabs and context menus, and with a document of many features: with the features they test. |

## What the specification changes in the plan

1. **An interface shell phase before the sketcher (Phase 1b).** The specification says to build
   the shell and design system first and connect commands to it as they become real. The sketcher
   adds some twenty commands; they should land in a command registry with search, themes, and
   icons already in place, not be retrofitted.
2. **The name.** The specification says ForgeCAD throughout. The project is LCAD, and nothing in
   the repository uses the other name.
3. **Originality.** The brief says "Fusion-like"; the specification adds that LCAD must not look
   like a copy. What carries over from Fusion is the workflow and the mouse mapping, which are
   conventions, not artwork. Icons, colors, and the orientation control are to be LCAD's own.
