# LCAD: MASTER GUI, UX, AND DESKTOP INTERFACE SPECIFICATION

*The owner's specification for LCAD's interface, received on 2026-10-12 and kept as written. The
only change is the name: the original used the placeholder "ForgeCAD". Where the interface stands
against it is tracked in [INTERFACE_STATUS.md](INTERFACE_STATUS.md).*

## Mission

Design and implement a polished, professional, Linux-native mechanical CAD desktop interface for LCAD.

LCAD should provide the familiar workflow efficiency expected from established parametric CAD applications while developing its own original interface, visual identity, navigation system, icons, and interaction design.

The interface must feel like serious desktop engineering software. It must not feel like a website, an Electron dashboard, a collection of floating dialogs, or an academic engineering tool assembled from unrelated panels.

The design objective is simple:

**Keep the established CAD workflow. Remove unnecessary interface friction. Make the application feel cohesive, fast, precise, and pleasant to use.**

This is a functional GUI specification. Implement actual working controls and connect them to the application model. Do not stop after creating a visual prototype.

## 1. Originality and intellectual property requirements

Use general mechanical CAD conventions as inspiration, not another application's exact interface as a blueprint.

Do not reproduce Autodesk Fusion's exact screen composition, proprietary icons, logos, illustrations, distinctive artwork, exact color treatment, branded terminology where avoidable, or pixel-for-pixel arrangement of controls.

Create an independent design system with:

- Original LCAD branding.
- Original icons or appropriately licensed icon libraries.
- Independently designed toolbars and menus.
- A consistent and original color palette.
- Original panel spacing and typography rules.
- A distinct contextual interaction system.
- A custom application icon and document icon.
- Independently authored help text and illustrations.

Generic functionality such as a model tree, viewport, feature history, sketch tools, and numerical property fields is part of the normal vocabulary of CAD software. Implement these concepts using original visual and interaction design.

Do not use proprietary application screenshots, extracted interface assets, reverse-engineered code, or unauthorized resources.

## 2. Overall visual direction

The application should look restrained, technical, modern, and professional.

Avoid both extremes: a dated industrial interface packed with tiny buttons and an oversized minimalist interface that hides everything behind multiple clicks.

Use a compact desktop layout with readable text, well-spaced controls, crisp geometry, and clear visual hierarchy.

### Visual design system

Establish reusable design tokens for:

- Application background.
- Panel backgrounds.
- Viewport background.
- Primary text.
- Secondary text.
- Borders and separators.
- Selection highlighting.
- Hover states.
- Active tool states.
- Warning and error states.
- Success indicators.
- Disabled controls.
- Corner radii.
- Standard spacing.
- Toolbar dimensions.
- Icon sizes.
- Typography sizes.

Use one restrained accent color for primary actions and selection. Reserve stronger colors for errors, warnings, and important interactive states.

Use subtle borders and background differences to separate panels. Avoid excessive shadows, gradients, glass effects, decorative cards, and oversized rounded corners.

The viewport must visually dominate the interface.

Use a coherent icon set with consistent line weight, proportions, and optical alignment. Avoid mixing unrelated icon styles.

### Typography

Use a readable system or bundled font suitable for engineering software.

Suggested hierarchy:

- Application and document titles: 13–15 px.
- Standard controls: 12–14 px.
- Panel headings: 12–14 px.
- Secondary metadata: 11–12 px.
- Important dimensions and numerical values: clearly legible and easy to select.

Allow system scaling and high-DPI displays. These sizes are starting points, not rigid limits.

Do not use tiny text simply to fit more controls into a panel.

## 3. Main application window

Create a single cohesive desktop window with a predictable arrangement of functional regions.

The default arrangement should contain:

1. A compact application header.
2. A document and workspace strip.
3. A contextual command toolbar.
4. A hierarchical model browser on the left.
5. A large central 3D viewport.
6. A contextual properties panel on the right.
7. A feature history area at the bottom.
8. Optional status information along the lower edge.

The viewport should receive the majority of available space.

Use resizable splitters so users can adjust the browser, properties panel, and history area. Persist layout preferences between launches.

Do not force every panel to remain permanently expanded.

Provide a command or shortcut to restore the default layout.

## 4. Application header

Create a slim top header with the LCAD identity and essential document controls.

Include:

- Application menu.
- New document.
- Open document.
- Save.
- Undo and redo.
- Document tabs.
- Workspace or task-area selector.
- Command search.
- Document save state.
- Optional update notification.

Do not place every application function in the header.

Group document commands logically and use tooltips for icon-only controls.

Use familiar desktop shortcuts where appropriate, including Ctrl+N, Ctrl+O, Ctrl+S, Ctrl+Z, and Ctrl+Shift+Z.

Document tabs should support multiple open projects without creating unnecessary toolbar clutter.

Show an unsaved-state indicator. Warn users before closing a document with unsaved changes.

The application must remain usable without signing in.

## 5. Workspace navigation

Organize the application's major capabilities into a small number of recognizable areas.

Potential areas include:

- Design.
- Assembly.
- Drawing.
- Manufacture.
- Inspect.

Sketching, solid modeling, direct editing, and construction tools should remain accessible naturally within the design workflow rather than requiring separate application modes for every task.

Changing workspaces should update relevant commands and panels without unexpectedly rearranging the entire application.

Preserve the current model, camera, selection, and document state when changing workspaces whenever technically appropriate.

Use contextual visibility instead of forcing users to choose a specialized environment before they can perform basic operations.

Do not create separate workspaces simply to make the application appear feature-rich.

## 6. Contextual command toolbar

Build a horizontal toolbar beneath the document header.

Use a small number of meaningful groups, such as:

- Create.
- Modify.
- Assemble.
- Construct.
- Inspect.
- Insert.

The actual available groups may adapt to the current task.

Each group should have a clear visual identity, an understandable label, and a predictable place in the toolbar.

Use compact icons with optional text labels. Ensure that users can identify unfamiliar tools through tooltips and command search.

Frequently used tools should be immediately accessible.

Less frequently used commands may appear in expandable groups or searchable menus.

Do not permanently display every feature in the application.

### Contextual behavior

When editing a sketch, emphasize drawing, dimensioning, and constraint tools.

When editing a solid, emphasize extrusion, revolve, fillet, chamfer, shell, patterns, and direct modifications.

When working with an assembly, emphasize components, joints, positioning, and interference checks.

When preparing manufacturing operations, emphasize stock, tooling, machining operations, simulation, and export.

Preserve consistent toolbar organization across contexts. Do not make the same tool jump to a completely different location without a good reason.

## 7. Left-side model browser

The model browser is the user's primary navigation tool for understanding the document.

Design a compact hierarchical tree containing relevant elements such as:

- Document root.
- Components.
- Bodies.
- Sketches.
- Features.
- Construction geometry.
- Reference planes and axes.
- Assembly joints.
- Linked components.
- Origin and coordinate systems.

Show only relevant object types and metadata.

Use indentation, disclosure arrows, icons, and restrained selection highlighting to communicate hierarchy.

### Required interactions

- Expand and collapse branches.
- Select objects.
- Rename objects inline.
- Hide and show objects.
- Isolate selected objects.
- Reveal hidden objects.
- Open contextual menus.
- Suppress supported features.
- Navigate to an object's geometry.
- Edit supported features.
- Reorganize supported component structures through drag and drop.
- Filter or search objects by name.
- Identify failed or suppressed features.

Right-clicking an object should expose actions relevant to that object.

For example, a sketch should offer sketch-editing actions, while a component should offer component-specific actions.

Do not present impossible actions as enabled buttons.

Support multiselection when the underlying operation permits it.

Use meaningful names where possible, and encourage users to rename important features without making it mandatory.

## 8. Central 3D viewport

The viewport is the main working surface. It should occupy most of the screen and remain visually uncluttered.

Render actual model geometry with appropriate depth perception, edge definition, and selection feedback.

### Default appearance

Use a neutral background, subtle reference grid, clear axes, and a well-lit model.

The background may use a restrained solid color or a subtle gradient. Avoid strong decorative textures.

Provide distinct but coordinated light and dark themes.

### Camera navigation

Support:

- Orbit.
- Pan.
- Zoom.
- Fit model to view.
- Zoom to selection.
- Standard orthographic views.
- Isometric view.
- Perspective and orthographic projection.
- View cube or an original equivalent orientation control.

Use configurable mouse navigation presets.

A sensible default is:

- Middle mouse drag for pan.
- Scroll wheel for zoom.
- A consistent mouse gesture or button combination for orbit.

Allow users to change these settings.

Do not make the user learn a different camera system for each workspace.

### Selection

Support selection of:

- Bodies.
- Components.
- Faces.
- Edges.
- Vertices.
- Sketch entities.
- Construction geometry.
- Supported assembly objects.

Highlight hovered geometry before selection, but keep the effect subtle enough to preserve edge visibility.

Use distinct visual states for hovered, selected, preselected, and reference geometry.

Provide selection filters and a way to resolve overlapping selectable objects.

### Viewport overlays

Place navigation and view controls around the viewport edges rather than covering important model geometry.

Support:

- Orientation widget.
- View controls.
- Display style selector.
- Section analysis.
- Grid and axes visibility.
- Selection filters.
- Fit and zoom controls.

Controls should remain small and unobtrusive.

Do not place large permanent tool panels over the model.

### Visual styles

Provide useful display modes such as:

- Shaded.
- Shaded with edges.
- Wireframe.
- Hidden-line display.
- Transparent display where appropriate.

Use consistent line weights and distinguish silhouette edges from internal edges when practical.

Provide a reliable way to reset the viewport appearance.

## 9. Right-side contextual properties panel

Use a right-side panel for the currently active tool, selected object, or editing operation.

The panel should be contextual, compact, and predictable.

It must not permanently display a giant collection of unrelated settings.

### Example: extrusion

When the user launches an extrusion, show only relevant controls:

- Selected profile.
- Start condition.
- Direction.
- Distance or extent.
- Taper.
- Operation type.
- Preview.
- Confirm.
- Cancel.

Allow users to enter numerical values directly and manipulate supported parameters in the viewport.

### Example: fillet

Show:

- Selected edges or faces.
- Radius.
- Relevant continuity options.
- Live preview.
- Confirm and cancel.

### Example: selected body

Show relevant properties such as:

- Name.
- Visibility.
- Material.
- Physical properties.
- Available modeling actions.

### Interaction rules

- Update the panel when the active selection or operation changes.
- Preserve valid values while editing.
- Clearly distinguish editable fields from read-only information.
- Validate numerical input.
- Provide understandable errors.
- Support keyboard navigation between fields.
- Keep confirmation controls consistently positioned.
- Do not close the operation unexpectedly when the user changes a value.
- Allow cancellation without committing invalid geometry.

For frequent operations, use inline controls and viewport manipulation where appropriate instead of opening a large dialog.

## 10. Feature history and timeline

Provide a dedicated feature history area along the bottom of the main window.

It should represent the chronological construction of the current model.

Use compact feature items with a small icon and readable label.

Examples include:

- Sketch.
- Extrude.
- Hole.
- Fillet.
- Pattern.
- Cut.
- Chamfer.

### Required functionality

- Select a feature.
- Open its editing controls.
- Rename a feature.
- Suppress and restore a feature.
- Inspect supported dependencies.
- Move the history marker.
- Navigate to an earlier model state.
- Reorder features where valid.
- Identify failed operations.
- Display relevant warnings.
- Collapse or expand timeline details.

Use clear visual differentiation for selected, suppressed, failed, and active-history items.

Allow horizontal scrolling for long histories.

Support a compact mode for users with smaller displays.

The timeline must be a functional representation of the document history, not a row of decorative buttons.

Do not silently permit a reordering operation that violates dependencies.

## 11. Command search

Implement a fast, application-wide command search.

Provide a configurable shortcut. S may be considered as a default if it does not conflict with active sketching or other established shortcuts.

The search interface should appear near the center or top of the window without permanently consuming screen space.

It should support:

- Partial command names.
- Common aliases.
- Search by operation type.
- Keyboard navigation.
- Enter to launch.
- Escape to close.
- Recent commands.
- Relevant commands based on the current selection.

For example, searching for "round edge" could suggest Fillet, while "measure hole" could suggest diameter measurement.

Search must launch real commands. Do not make it a decorative text field.

Prioritize speed and relevance. Do not require users to remember the exact official name of every operation.

## 12. Context menus and rapid interactions

Provide contextual right-click menus throughout the application.

Menus should contain actions relevant to the clicked object or selected geometry.

Examples include:

- Edit.
- Rename.
- Hide or show.
- Isolate.
- Suppress.
- Delete.
- Measure.
- Properties.
- Create a sketch on a suitable face.
- Modify a suitable face.
- Navigate to a feature.

Only show applicable actions or clearly disable unsupported operations.

Keep menus short enough to scan quickly.

Use separators and submenus only when they improve organization.

Consider an optional compact radial or marking menu for frequently used commands, but do not make it mandatory or imitate a competitor's distinctive menu design.

Every rapid-access interaction must have an ordinary menu or keyboard alternative.

## 13. Sketch editing interface

Entering sketch mode should produce an obvious, controlled transition.

The camera should orient to the sketch plane, while the application highlights the active sketch and presents the relevant drawing tools.

Show sketch-specific controls such as:

- Line.
- Rectangle.
- Circle.
- Arc.
- Spline.
- Offset.
- Trim.
- Mirror.
- Pattern.
- Dimension.
- Constraint tools.

Provide clear visual distinction between ordinary geometry, construction geometry, projected references, and fully constrained geometry.

Dimensions should appear near their relevant geometry and remain editable.

Avoid covering the sketch with permanent side panels that consume most of the working area.

Provide a clear way to finish or cancel sketch editing.

When leaving sketch mode, restore the appropriate modeling tools without losing the current selection or camera state unnecessarily.

## 14. In-canvas numerical editing

Where practical, allow numerical values to be entered directly during geometry creation.

For example, while creating a line, the user should be able to enter its length and angle. While creating an extrusion, the user should be able to type a distance without leaving the viewport.

Use visual feedback to show which value is being edited.

Support units and expressions where appropriate.

When input is invalid, preserve the user's text and explain the problem rather than silently replacing it.

Provide keyboard access to property fields and allow normal text editing.

Direct manipulation and exact numerical entry must work together.

## 15. Document tabs and project switching

Support multiple open documents through a compact tab strip.

Each tab should display a readable document name and an unsaved indicator when necessary.

Support:

- Switching documents.
- Opening recent projects.
- Creating a new document.
- Closing a document.
- Detecting unsaved changes.
- Reopening recent files.

Avoid excessive tabs and unnecessary document-management controls.

The application should remain entirely useful when all documents are stored locally.

Provide a recent-files list with meaningful paths and optional thumbnails.

Do not require a proprietary cloud project browser.

## 16. File dialogs and local project management

Use native or well-integrated Linux file dialogs.

Make common file operations predictable:

- New.
- Open.
- Save.
- Save As.
- Import.
- Export.
- Package project.

Clearly distinguish opening an editable native project from importing geometry.

Use appropriate file filters and remember the user's last relevant directory when possible.

When exporting, show only settings relevant to the selected format.

Provide useful warnings for unit mismatches, unsupported geometry, or potentially lossy conversion.

Do not automatically save projects into hidden proprietary directories.

Do not require cloud storage or an online account.

## 17. Notifications and status information

Use restrained, nonintrusive status messages.

Show brief feedback for events such as:

- Document saved.
- Export completed.
- Geometry operation failed.
- Recalculation in progress.
- Reference file missing.
- Invalid geometry detected.

Avoid modal alerts for routine success messages.

Reserve modal dialogs for decisions that genuinely require user attention.

Provide a lower status area for useful information such as selection type, units, active command, and progress.

Keep technical diagnostic details available without overwhelming ordinary users.

## 18. Light and dark themes

Provide two carefully designed themes:

- Light.
- Dark.

Both themes must maintain sufficient contrast, clear selection states, legible text, and consistent geometry colors.

Do not simply invert every color.

Define theme tokens centrally so that all panels, menus, overlays, dialogs, and tooltips remain visually consistent.

Support the operating system's preferred appearance as an optional default.

Persist the user's choice.

## 19. Customizable layout

Allow users to adjust the interface to their working style.

Support:

- Resizing panels.
- Hiding and showing panels.
- Collapsing the timeline.
- Compact and comfortable density modes.
- Resetting to the default layout.
- Saving layout preferences.
- Restoring the previous layout after restarting.

Avoid arbitrary panel movement that makes the interface difficult to understand.

Docking should be predictable, with clear drop targets and sensible minimum sizes.

Keep the main viewport usable even when multiple panels are open.

Provide a focus-viewport mode for detailed modeling.

Do not make layout customization a prerequisite for normal work.

## 20. Mouse and keyboard interaction

Design the interface for users who spend hours modeling.

Every frequent operation should be accessible through a reasonable combination of mouse, keyboard, and contextual tools.

Support:

- Standard desktop shortcuts.
- Configurable shortcuts.
- Escape to cancel.
- Enter to confirm where appropriate.
- Delete for supported removal operations.
- F2 or another documented shortcut for renaming.
- Undo and redo.
- Keyboard navigation through forms.
- Clear hover and focus states.

Avoid accidental destructive operations.

Use confirmation for consequential actions that cannot be easily recovered, not for every ordinary edit.

Maintain consistent behavior across the application.

## 21. Responsive desktop behavior

This is desktop software, not a responsive website.

Optimize for conventional Linux workstation and laptop screens.

At smaller window sizes:

- Reduce unnecessary toolbar labels.
- Allow toolbar groups to collapse.
- Provide panel visibility controls.
- Preserve readable controls.
- Keep the viewport accessible.
- Allow the timeline to collapse.
- Prevent overlapping dialogs and panels.

At larger sizes, make better use of available space without excessively stretching property controls or text.

Support common high-DPI configurations and multiple monitors.

Do not hide critical functions merely because the window is narrow.

## 22. Accessibility and usability

Ensure:

- Readable contrast.
- Keyboard-accessible menus.
- Visible keyboard focus.
- Tooltips for unfamiliar icons.
- Configurable UI scaling.
- Consistent interaction patterns.
- Screen-reader-friendly controls where practical.
- Reduced-motion support.
- Clear error communication.

Never communicate an object's state using color alone.

Use icons, labels, and other indicators together when practical.

## 23. Animation and performance

Use subtle transitions for panel changes, menu appearance, selection feedback, and viewport overlays.

Animations must be quick and purposeful.

Do not animate the interface merely for decoration.

Never delay a modeling operation to complete an animation.

Keep geometry rendering and user interaction responsive during expensive calculations whenever possible.

Do not let an elaborate interface framework compromise viewport performance.

Use native desktop components where they provide a better experience, and custom-rendered controls where they materially improve CAD interaction.

## 24. Implementation architecture for the GUI

Build the interface as reusable, testable modules.

Suggested separation:

- Main application shell.
- Document and tab manager.
- Workspace controller.
- Command registry.
- Toolbar system.
- Model browser.
- 3D viewport.
- Contextual property editor.
- Feature history panel.
- Command search.
- Context menu system.
- Preferences and themes.
- File dialogs and import/export UI.
- Status and notification system.

The interface must communicate with the underlying document and geometry systems through defined APIs.

Do not put geometry-engine logic directly into button callbacks.

Commands should expose their state, applicable selection types, parameters, validation, execution, and cancellation behavior.

Use a shared command registry so the same operation can be invoked from a toolbar, search result, context menu, or keyboard shortcut without maintaining separate implementations.

Ensure that all controls reflect the actual state of the document.

A failed operation must not leave a misleading success state in the interface.

## 25. Required working prototype

Before implementing every advanced feature, produce a fully usable GUI prototype with actual modeling integration.

The prototype must support this complete interaction sequence:

1. Launch the application.
2. Create a new local document.
3. See the model browser, viewport, contextual toolbar, and timeline.
4. Create a sketch on a reference plane.
5. Draw and constrain a rectangle.
6. Enter exact dimensions.
7. Finish the sketch.
8. Extrude the profile into a solid.
9. Enter an exact extrusion distance.
10. Apply a fillet.
11. Edit the original sketch.
12. Observe the resulting model update.
13. Select a feature in the history.
14. Save the project.
15. Close and reopen it.
16. Continue editing the project.

The interface must respond to actual model state changes throughout the sequence.

## 26. GUI testing requirements

Test the interface at common desktop resolutions and scaling settings.

Verify:

- No overlapping controls.
- No clipped essential buttons.
- No unreadable labels.
- No broken toolbar layouts.
- Correct keyboard focus.
- Predictable mouse behavior.
- Working context menus.
- Correct selection highlighting.
- Consistent panel resizing.
- Reliable dialog positioning.
- Correct document tab behavior.
- Proper unsaved-state indicators.
- Functional undo and redo.
- Correct state after canceled operations.
- Usable light and dark themes.

Test with both a simple part and a complicated document containing many features and components.

Do not declare the interface finished solely because it looks good in a screenshot.

## 27. Final instructions

Start by implementing the overall application shell and design system.

Then build the viewport, model browser, contextual toolbar, properties panel, and feature timeline as functional components.

Connect each component to real document and geometry operations as those operations become available.

Use placeholder geometry only for isolated UI development and clearly separate it from the actual CAD model.

Do not fabricate interactive behavior, feature history, dimensions, selection states, or successful exports.

Do not copy another CAD application's exact interface or branding.

Make sensible design decisions independently and preserve a consistent visual language across every part of the application.

The finished product should feel familiar to experienced CAD users without being a visual clone of any existing commercial application.

**Success means users can open LCAD, immediately understand where to work, create and edit precise mechanical geometry without fighting the interface, and complete everyday modeling tasks with minimal unnecessary clicks.**
