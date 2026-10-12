# LCAD project brief: a local-first, Linux-native parametric CAD application

This is the founding brief for LCAD (LinuxCAD). It defines what the product is, what it must do, and the
order in which it gets built. The schedule lives in [ROADMAP.md](../ROADMAP.md).

## 1. Project vision

Build a professional desktop mechanical CAD application for Linux that captures the best parts of Autodesk Fusion 360's everyday design workflow, while removing unnecessary complexity, cloud dependency, interface clutter, and awkward interactions.

The goal is not to build a browser CAD application, a basic OpenSCAD-style script editor, or a traditional engineering program with dozens of disconnected workbenches.

The goal is to create a cohesive, responsive, intuitive, feature-based 3D CAD environment where a user can sketch, constrain geometry, build solids, edit features, assemble components, inspect designs, prepare manufacturing files, and document parts without constantly changing their mental model of the software.

**Core principle: Fusion-like workflow, local ownership, simpler interface, native Linux performance.**

The application must work offline, keep project files on the user's computer, and remain useful without an account, subscription server, mandatory cloud storage, or internet connection.

Treat this as a serious engineering application. Geometry must be real, dimensionally accurate, editable, and suitable for practical mechanical design. Do not substitute attractive mockups or simulated CAD operations for functional engineering tools.

## 2. Product identity and design philosophy

The project name is LCAD, short for LinuxCAD.

The software should feel like a modern, well-designed desktop engineering tool rather than a web application wrapped in a desktop window.

Design principles:

- Desktop-first and Linux-native.
- Clean, restrained, professional interface.
- Fast access to frequently used tools.
- Context-sensitive commands instead of enormous permanent toolbars.
- Direct manipulation of geometry wherever practical.
- Exact numerical control whenever required.
- A persistent, understandable feature timeline.
- Predictable keyboard shortcuts and mouse behavior.
- Minimal modal dialogs.
- No unnecessary configuration before creating a simple part.
- Advanced capabilities remain available without overwhelming beginners.
- Local files are the primary source of truth.
- No artificial limitations based on subscription tiers.

Take inspiration from Fusion 360's modeling rhythm and visual organization, but do not copy its branding, proprietary source code, protected assets, or exact interface artwork.

## 3. Target users

The application must serve:

1. Hobbyists designing mechanical parts and 3D-printable components.
2. RC car builders designing chassis, suspension parts, gears, brackets, and drivetrain components.
3. Mechanical engineers building precise parametric parts and assemblies.
4. Makers designing enclosures, fixtures, adapters, and mechanisms.
5. CNC users preparing parts for milling, turning, and fabrication.
6. Advanced users working with complex surfaces, imported CAD files, and large assemblies.

A beginner should be able to create a dimensioned sketch and extrude a solid within minutes. An experienced user should be able to build a complex assembly without feeling restricted by a simplified toolset.

## 4. Operating system and application architecture

### Primary platform

Linux is the primary and first-class target.

Support:

- Ubuntu and Ubuntu-based distributions.
- Linux Mint.
- Fedora.
- Arch Linux and derivatives.

Package the application for practical installation using AppImage and Flatpak, with native packages where appropriate.

Follow Linux desktop conventions for file dialogs, keyboard shortcuts, clipboard integration, window management, fonts, high-DPI displays, and accessibility.

Do not make the application dependent on a web browser, remote server, or cloud-based geometry service.

### Suggested technology architecture

Evaluate the following technologies before committing to the implementation:

- C++ for performance-critical geometry, modeling operations, and core application services.
- Open CASCADE Technology (OCCT) as a candidate boundary-representation geometry kernel.
- Qt 6 for the desktop application, docking panels, dialogs, menus, and native interaction.
- A high-performance GPU-accelerated 3D viewport using an appropriate rendering backend.
- A constraint-solving engine for 2D sketches.
- A persistent feature graph and dependency-aware recomputation engine.
- Python scripting as an optional extension layer, not a requirement for ordinary modeling.
- A local project database or structured native document format with robust save and recovery behavior.

These are architectural candidates, not assumptions that every dependency is automatically suitable. Verify licensing, platform support, maintenance status, API compatibility, and redistribution terms before adoption.

Keep the geometry kernel, application interface, document model, rendering layer, and manufacturing systems separate enough that they can be maintained independently.

Never implement core CAD geometry using approximate visual tricks when an exact geometric representation is required.

## 5. The primary user interface

The default screen should feel immediately familiar to someone who has used Fusion 360.

### Main layout

Use five principal regions:

**A. Top application bar**

Contains:

- Application menu.
- New, open, save, and undo/redo.
- Document tabs.
- Workspace selector.
- Search commands.
- Document and project status.
- Optional settings and account-free update controls.

Avoid filling the entire top of the screen with permanent toolbars.

**B. Main modeling toolbar**

Organize the most common tools into a few clear groups:

- Create.
- Modify.
- Assemble.
- Inspect.
- Construct.
- Insert.

The toolbar changes contextually when editing a sketch, a solid, a surface, a mesh, or an assembly.

Frequently used tools must remain one click away. Less frequently used tools can live in searchable menus.

**C. Model browser**

A left-side hierarchical tree showing:

- Components.
- Bodies.
- Sketches.
- Construction geometry.
- Origin planes and axes.
- Features.
- Linked components.
- Suppressed or hidden objects.
- Joints and assembly relationships.

Allow users to rename objects inline, drag objects to reorganize supported hierarchies, hide and show elements, isolate selections, and open contextual actions through right-click menus.

Do not expose every internal implementation detail in the browser. Present a clear, understandable model structure.

**D. Central 3D viewport**

This is the primary working area and should occupy most of the window.

Support:

- Orbit, pan, and zoom.
- Perspective and orthographic projection.
- View cube and standard views.
- Face, edge, vertex, body, and component selection.
- Hover highlighting and selection pre-highlighting.
- Section analysis.
- Measure tools.
- Visual feedback for active operations.
- In-canvas dimensions and manipulation handles.
- Grid and origin visibility controls.
- Multiple display styles.
- Selection filters.
- Context-sensitive marking menus or equivalent fast-access interactions.

**E. Bottom feature timeline**

The timeline is a first-class part of the application, not a hidden panel.

Show the chronological sequence of sketches, extrusions, revolves, cuts, fillets, patterns, shell operations, and other modeling features.

Support:

- Selecting and editing previous features.
- Suppressing and re-enabling features.
- Reordering features where dependencies allow.
- Moving the history marker to inspect an earlier model state.
- Identifying failed operations.
- Displaying feature dependencies.
- Scrubbing the model history.
- Grouping or collapsing repetitive operations.
- Showing useful names instead of opaque feature identifiers.

The timeline must remain readable on normal desktop displays. Offer a compact mode and allow it to resize or collapse when necessary.

## 6. Interaction design: preserve the Fusion workflow

Modeling should follow a natural sequence:

1. Select a plane, face, or reference surface.
2. Create a sketch.
3. Draw geometry and apply constraints.
4. Finish the sketch.
5. Select a profile.
6. Extrude, revolve, sweep, loft, or otherwise create geometry.
7. Modify the resulting solid.
8. Repeat while retaining a useful editable history.

This sequence should feel consistent throughout the application.

For example, when a user clicks a planar face and starts a sketch, the application should automatically orient the camera, establish a sensible sketch coordinate system, and present the appropriate sketch tools.

When a user selects a closed profile and presses Extrude, show a live preview with an editable distance, operation type, direction, and relevant options.

Use direct manipulation for common operations. Numerical fields must remain available for precise dimensions.

Every interactive tool should provide:

- Clear activation and cancellation.
- Live geometry previews.
- Sensible defaults.
- Inline numerical input.
- Explicit confirmation when appropriate.
- Undoable results.
- Helpful errors rather than silent failures.

The Escape key should cancel the current operation or dismiss the active interaction predictably.

Do not force users to navigate through multiple unrelated panels to complete a basic modeling operation.

## 7. Sketching and constraints

Build a robust, fully functional 2D sketcher.

### Drawing tools

Include:

- Line and connected polyline.
- Rectangle, center rectangle, and oriented rectangle.
- Circle and center-diameter circle.
- Three-point circle.
- Arc and three-point arc.
- Ellipse.
- Spline and fit-point spline.
- Polygon.
- Slot.
- Point.
- Construction lines.
- Offset geometry.
- Trim, extend, split, and project geometry.
- Mirror and sketch patterns.
- Text profiles where supported by the geometry kernel.
- Projected references from existing 3D edges and faces.

### Constraints

Include:

- Coincident.
- Horizontal and vertical.
- Parallel and perpendicular.
- Tangent.
- Equal.
- Concentric.
- Midpoint.
- Symmetry.
- Collinear.
- Fix or lock.
- Point-on-object.
- Distance and length.
- Radius and diameter.
- Angle.
- Curvature constraints where supported.

### Sketch solver behavior

The sketcher must show whether a sketch is underconstrained, fully constrained, or overconstrained.

Allow users to drag unconstrained geometry while respecting existing relationships.

Provide automatic geometric inference while drawing, with visual indicators for alignment, tangency, endpoints, centers, and perpendicularity.

Automatic constraints must be useful without being intrusive. Users must be able to disable automatic inference or remove an unwanted constraint immediately.

Dimensions should be editable directly on the sketch whenever practical.

When a sketch fails to solve, identify conflicting or redundant constraints and offer recovery suggestions. Do not arbitrarily move geometry without informing the user.

Support projected geometry with visible reference indicators and sensible behavior when the referenced model changes.

## 8. Parametric solid modeling

Implement a history-based solid modeling system with a dependable feature tree and recomputation engine.

### Core creation features

- Extrude.
- Revolve.
- Sweep.
- Loft.
- Rib.
- Thin feature.
- Hole.
- Thread.
- Coil or helix.
- Primitive solids.
- Pattern-based feature creation.
- Create body and create component.

### Core modification features

- Fillet.
- Chamfer.
- Shell.
- Draft.
- Offset face.
- Move face.
- Press-pull.
- Split body.
- Split face.
- Combine.
- Boolean union, cut, and intersection.
- Scale.
- Mirror.
- Rectangular pattern.
- Circular pattern.
- Face replacement where supported.
- Delete face with healing.
- Direct geometry manipulation for imported solids.

Features must have editable parameters and meaningful previews.

A fillet should store its radius and target edges or faces. An extrusion should store its source profile, distance, direction, and operation type. Editing a sketch should update downstream features where their dependencies remain valid.

### Modeling operations

Support:

- Join or additive operations.
- Cut or subtractive operations.
- Intersect operations.
- New-body operations.
- Symmetric extrusions.
- Two-sided extrusions.
- Up-to-face and up-to-object extents.
- Taper angles.
- Start offsets.
- Multiple supported extrusion directions.

Provide clear operation names and contextual options instead of exposing a large collection of obscure settings by default.

### Feature failure handling

Feature failures are inevitable in parametric CAD, but recovery must be understandable.

When a change breaks downstream geometry:

- Identify the failed feature.
- Explain the immediate failure where possible.
- Highlight invalid references.
- Preserve the last valid model state.
- Allow the user to edit or suppress the feature.
- Provide tools to repair or replace broken references.
- Avoid silently deleting dependent geometry.

Use stable reference strategies wherever possible. Do not rely exclusively on fragile internal edge numbering.

## 9. Direct modeling and hybrid workflows

Parametric modeling should be the default, but imported geometry must not be a dead end.

Support direct edits to suitable imported solids without requiring the user to reconstruct the original feature history.

Provide:

- Face movement and offset.
- Face replacement.
- Solid splitting.
- Hole modification where recognizable.
- Fillet and chamfer editing where supported.
- Boolean operations.
- Body transforms.
- Geometry healing and validation.

Allow parametric and direct-modeling operations to coexist in a document through explicit, understandable feature boundaries.

When editing a STEP file, for example, users should be able to select a planar face, offset it by a specified distance, and continue designing without first recreating the part from sketches.

Preserve the original imported geometry and make destructive changes undoable.

## 10. Surface and freeform modeling

Provide a dedicated contextual surface toolset without forcing users into an entirely different application workflow.

Include:

- Surface extrude.
- Surface revolve.
- Surface sweep.
- Surface loft.
- Boundary surfaces.
- Ruled surfaces.
- Surface offset.
- Trim and untrim.
- Extend.
- Stitch.
- Surface intersection.
- Thicken.
- Knit or join surfaces where supported.
- Surface continuity inspection.
- Conversion of closed surface shells into solids when valid.

Provide subdivision or freeform modeling as a later advanced capability.

Do not promise full T-spline parity until an actual, reliable implementation exists. Complex freeform modeling should be treated as a substantial engineering milestone.

## 11. Mesh modeling

Support practical workflows involving 3D printing, scanning, and downloaded models.

Include:

- Import and export of STL, OBJ, and 3MF where supported.
- Mesh inspection.
- Translation, rotation, and scaling.
- Mesh repair.
- Hole filling.
- Normal correction.
- Mesh simplification.
- Mesh cutting and splitting.
- Basic mesh Boolean operations.
- Section and measurement tools where reliable.
- Mesh-to-solid conversion for suitable geometry.

Clearly distinguish editable B-rep solids from polygon meshes.

Do not automatically convert enormous meshes into impractical collections of solid faces. Warn users when an operation is likely to consume substantial memory or time.

## 12. Components and assemblies

Assemblies are essential to the product, not an optional afterthought.

Support both bottom-up and top-down design.

### Component system

Users should be able to:

- Create independent components.
- Create bodies inside components.
- Reuse existing components.
- Insert external part files as linked components.
- Build subassemblies.
- Rename and reorganize components.
- Ground components.
- Hide, isolate, and suppress components.
- Create copies or linked instances.
- Edit a component in context.
- Design one component using geometry from another.

Distinguish a component, which represents a reusable part or subassembly, from a body, which represents a modeled solid or surface object.

### Assembly constraints and joints

Implement:

- Rigid.
- Revolute.
- Slider.
- Cylindrical.
- Pin-slot.
- Planar.
- Ball.
- Fixed and grounded relationships.
- Joint origins and reference geometry.
- Joint limits.
- Motion preview.

Provide an intuitive joint-creation workflow: select two appropriate faces, axes, or reference points, preview the relationship, adjust alignment, and confirm.

Automatically suggest likely joint types based on the selected geometry, but let users override the suggestion.

### Assembly inspection

Include:

- Component interference checking.
- Clearance measurement.
- Exploded views.
- Assembly motion previews.
- Component visibility control.
- Mass properties.
- Center of mass.
- Assembly hierarchy.
- Bill of materials.

Large assemblies must be able to use lightweight representations and delayed loading where appropriate.

## 13. Parameters and equations

Provide a global parameter table with:

- Named parameters.
- Numeric values.
- Units.
- Expressions.
- Descriptions.
- User-defined constants.
- References to supported model dimensions.

Support expressions such as:

`gear_diameter = tooth_count * module`

`wall_thickness = 2.4 mm`

`shaft_length = housing_length - 12 mm`

Allow parameters to drive sketches, features, and supported assembly dimensions.

Support unit-aware arithmetic and clear error messages for incompatible dimensions or invalid expressions.

A user should be able to change one global parameter and have dependent geometry update correctly.

## 14. Precision, units, and engineering properties

Provide millimeters and inches as first-class units, with support for additional units as appropriate.

Include:

- Document-level units.
- Precision settings.
- Tolerance annotations.
- Distance and angle measurements.
- Bounding boxes.
- Surface area and volume.
- Mass and center of mass.
- Material density.
- Material assignment.
- Principal moments of inertia where supported.
- Physical material libraries.
- User-defined materials.

Geometry must remain internally precise when the display precision changes.

Provide meaningful geometry validity checks, including open shells, invalid solids, self-intersections where detectable, and Boolean failures.

Do not confuse visually approximate tessellation with the underlying exact geometric model.

## 15. Construction and inspection tools

Include construction planes, axes, points, coordinate systems, and reference geometry.

Users should be able to create construction geometry from:

- Existing planes.
- Faces.
- Edges.
- Vertices.
- Angles and offsets.
- Intersections.
- Three points.
- Cylindrical axes.

Inspection tools should include:

- Measure distance.
- Measure angle.
- Radius and diameter.
- Minimum clearance.
- Section analysis.
- Curvature analysis.
- Face and edge information.
- Interference detection.
- Bounding box.
- Model validity.
- Center of mass.

Keep these tools accessible through a single, consistent inspection area.

## 16. Technical drawings

Provide a proper 2D engineering drawing environment linked to the 3D model.

Include:

- Standard orthographic views.
- Isometric views.
- Section views.
- Detail views.
- Auxiliary views.
- Hidden-line display.
- Dimensions.
- Geometric dimensioning and tolerancing where supported.
- Center marks and centerlines.
- Hole callouts.
- Notes and symbols.
- Title blocks.
- Drawing templates.
- Part numbers and revision information.
- Balloons and bills of materials.
- PDF export.
- DXF export where appropriate.

Drawings must update when the underlying model changes, while preserving user annotations when their references remain valid.

Support practical ANSI and ISO drawing standards.

Do not attempt to implement every advanced drafting standard in the first release. Build a reliable core that can be expanded.

## 17. Manufacturing and CAM

Provide a dedicated manufacturing workspace integrated with the same project and model.

The initial CAM system should focus on practical 2.5D and 3-axis milling, with later expansion into turning and advanced machining.

Include:

- Stock definition.
- Work coordinate systems.
- Machine and tool libraries.
- Tool selection.
- Face milling.
- Adaptive clearing where implemented.
- Pocketing.
- Contouring.
- Drilling.
- Chamfering.
- Finishing passes.
- Heights and clearances.
- Feeds and speeds.
- Toolpath simulation.
- Collision and limit checks where supported.
- Estimated machining time.
- Postprocessor support.
- G-code export.

The user should be able to create a part, select manufacturing operations, simulate toolpaths, and export machine code without leaving the application.

Never generate supposedly safe production-ready toolpaths through placeholder logic. Validate actual toolpath geometry, machine settings, and postprocessor behavior.

Start with a limited, dependable collection of operations before attempting advanced five-axis machining.

## 18. 3D printing and fabrication

Support additive manufacturing workflows without turning the application into a complete slicer.

Include:

- STL and 3MF export.
- Mesh export with configurable resolution.
- Units and scale verification.
- Multiple-body export.
- Component separation.
- Orientation tools.
- Bounding-box checks.
- Basic wall-thickness analysis where feasible.
- Printable-solid checks.
- Export presets.

Allow users to send models to external slicers through configurable application integrations.

Keep the CAD model editable and separate from its exported mesh representation.

## 19. Rendering and visualization

Provide a lightweight, useful rendering workspace.

Include:

- Material appearances.
- Colors and roughness.
- Metal and plastic presets.
- Lighting presets.
- Background controls.
- Ground plane.
- Shadows.
- Camera position and focal length.
- Orthographic and perspective views.
- Transparent display where supported.
- Image export.

Prioritize a responsive viewport over photorealistic rendering.

Advanced ray tracing can be optional and should never be required for ordinary modeling.

## 20. Local-first project and file management

The native project format must be designed around local ownership.

Requirements:

- Save to any user-selected local directory.
- Support removable drives and external disks.
- No mandatory login.
- No required cloud account.
- No internet requirement for modeling.
- No cloud-only project references.
- No silent upload of designs or telemetry containing model data.
- Explicit opt-in for any network-based service.
- Reliable autosave.
- Crash recovery.
- Undo and redo.
- Document versioning.
- Backup and restore.
- Clear missing-reference warnings.
- Portable project packaging.
- Local thumbnails and previews.
- Dependency tracking for linked components.
- Safe project migration between application versions.

A project should be portable between Linux computers without relying on an account or a proprietary remote database.

When a project contains linked files, provide a dependency report and a command to package those dependencies into a portable project folder.

The application must distinguish between saving a document, saving a copy, exporting geometry, and packaging a project.

Do not confuse an exported STEP or STL file with the editable native project.

## 21. Import and export interoperability

Prioritize dependable support for:

- STEP and STEP AP242 where feasible.
- IGES.
- STL.
- OBJ.
- 3MF.
- DXF.
- SVG for supported 2D sketch and profile workflows.
- Native editable project files.
- PDF for drawings.
- Common neutral mesh formats.

Investigate additional formats based on real-world demand and licensing feasibility.

For imported B-rep geometry, preserve analytic faces and edges whenever possible.

For unsupported or lossy conversions, warn users before proceeding.

Never claim native feature-history compatibility with another CAD package when only its final geometry can be imported.

## 22. User experience and removal of unnecessary complexity

This is a core requirement.

Avoid the clutter and awkwardness associated with traditional multi-workbench CAD interfaces.

Specifically:

- No giant collections of unrelated tools permanently visible.
- No need to switch workbenches just to perform basic modeling tasks.
- No giant property panel that exposes every parameter at once.
- No modal dialog for every operation.
- No excessive confirmation prompts.
- No duplicated commands scattered across unrelated menus.
- No confusing distinction between common commands unless technically necessary.
- No forced cloud workflow.
- No online-only help requirement.
- No account-based activation for local modeling.
- No unnecessary animation or decorative effects.
- No hidden requirement to configure an extensive project database before creating a part.

Use contextual toolbars and compact property panels.

When a face is selected, show relevant actions such as sketch, offset face, measure, and create construction geometry.

When a sketch is active, show sketching and constraint tools.

When a body is selected, show solid operations and inspection tools.

When an assembly is active, show component and joint tools.

When a manufacturing setup is active, show stock, tools, operations, simulation, and postprocessing.

Keep advanced tools searchable rather than deleting them.

## 23. Command search and keyboard workflow

Implement a fast command search accessible with a memorable shortcut such as S or a configurable alternative.

Users should be able to type commands such as:

- Extrude.
- Fillet.
- Create sketch.
- Measure.
- Offset face.
- Create component.
- Add joint.
- Section analysis.
- Export STL.
- Change parameters.

Search results should appear immediately and support keyboard navigation.

Implement a consistent shortcut system for:

- Save.
- Undo and redo.
- Delete.
- Copy and paste.
- Hide and show.
- Fit view.
- Standard views.
- Sketch tools.
- Extrude.
- Measure.
- Cancel current operation.

Shortcuts must be discoverable and customizable.

Avoid conflicts with common Linux desktop and text-editing shortcuts.

## 24. Performance and responsiveness

Performance is a core product requirement.

The application must remain usable on ordinary modern desktop computers, not just high-end workstations.

Prioritize:

- Responsive selection.
- Smooth orbit and pan.
- Efficient tessellation.
- Background recomputation when safe.
- Incremental feature updates.
- Cached intermediate geometry.
- Dependency-aware invalidation.
- Lazy loading of large assemblies.
- Efficient document serialization.
- Memory-conscious mesh processing.
- Cancellation of long-running operations.
- Progress reporting for expensive tasks.

Do not block the interface during long computations when the operation can safely run in the background.

Avoid recomputing the entire document when only one independent feature has changed.

If recomputation fails, preserve the last valid result and present a clear explanation.

Create benchmarks for simple parts, deeply parameterized parts, complex imported solids, dense meshes, and large assemblies.

## 25. Undo, transactions, and document integrity

Every meaningful modeling operation must be undoable.

Treat operations as transactions:

1. Capture the necessary document state.
2. Validate input.
3. Perform the operation.
4. Validate the resulting geometry.
5. Commit only when successful.
6. Otherwise restore the previous valid state.

Ensure undo and redo behave consistently across sketches, features, assembly relationships, document settings, and supported metadata.

Autosave must not overwrite the only valid copy of a document with a corrupt or incomplete state.

Use atomic save strategies where practical.

Provide a recoverable journal or equivalent crash-recovery mechanism.

## 26. Extensibility and scripting

Provide an optional scripting API for advanced users and developers.

Potential uses include:

- Creating geometry.
- Editing parameters.
- Generating repetitive features.
- Importing and exporting models.
- Reading model properties.
- Automating drawings.
- Running validation checks.
- Creating custom commands.

Provide API documentation, examples, and versioning rules.

A plugin system may support additional tools, exporters, analysis modules, and integrations.

Plugins must run locally and must not receive unrestricted access to user designs without the permissions appropriate to the extension model.

Do not make scripting a prerequisite for ordinary modeling.

## 27. Error messages and help

Error messages should tell users:

- What failed.
- Which feature or operation failed.
- Why it may have failed.
- Whether the previous model remains valid.
- What the user can try next.

Avoid generic messages such as "Operation failed" when more useful information is available.

Provide contextual documentation, tooltips, short tutorials, and searchable help bundled locally where practical.

Make the first-run experience useful without forcing users through a long onboarding wizard.

## 28. Accessibility and desktop quality

Support:

- Adjustable interface scaling.
- High-DPI displays.
- Keyboard navigation.
- Clear focus indicators.
- Readable text.
- Configurable shortcuts.
- Reduced-motion preferences.
- Light and dark themes.
- Consistent iconography.
- Screen-reader-accessible controls where practical.

Do not make the interface depend on tiny icons without labels or discoverable tooltips.

Keep text legible and use a restrained visual hierarchy.

## 29. Security, privacy, and ownership

Treat local design files as valuable user-owned engineering data.

The application must:

- Operate fully offline for its core functionality.
- Avoid mandatory account creation.
- Avoid uploading files without explicit user action.
- Make network access optional and transparent.
- Avoid collecting model geometry as telemetry.
- Store configuration in appropriate user-level directories.
- Respect normal Linux file permissions.
- Provide predictable update behavior.
- Never lock a user's local project because an online service is unavailable.

Document all external dependencies, their licenses, and any network-facing functionality.

## 30. Development strategy and implementation order

Do not attempt to implement the entire product in one enormous coding pass.

Build a functioning application incrementally. Each phase must produce working software rather than an impressive-looking but nonfunctional mockup.

### Phase 1: Desktop foundation

Deliver:

- Linux desktop application.
- Main window and viewport.
- Camera navigation.
- Selection and highlighting.
- Document creation.
- Save and open.
- Basic object tree.
- Undo and redo.
- A simple native project format.

Acceptance test: create a document, construct or import a simple solid, save it, close the application, reopen it, and verify that the geometry is preserved.

### Phase 2: Real sketcher

Deliver:

- Sketch planes.
- Drawing tools.
- Geometric constraints.
- Dimensional constraints.
- Constraint solving.
- Underconstrained and fully constrained states.
- Sketch editing.
- Projected references.

Acceptance test: create a fully constrained rectangular sketch, change its dimensions, and verify that its geometry updates correctly.

### Phase 3: Parametric solid modeling

Deliver:

- Extrude.
- Cut.
- Revolve.
- Fillet.
- Chamfer.
- Shell.
- Patterns.
- Feature editing.
- Feature timeline.
- Dependency tracking.
- Recompute and recovery.

Acceptance test: create a dimensioned bracket, edit the original sketch, and verify that dependent features update or report clearly defined failures.

### Phase 4: Direct modeling and interchange

Deliver:

- STEP import and export.
- IGES import where supported.
- STL and 3MF export.
- Direct face editing.
- Geometry validation.
- Healing and repair tools.

Acceptance test: import a neutral solid, edit a suitable face, save the project, export STEP, and validate the resulting geometry.

### Phase 5: Components and assemblies

Deliver:

- Components and bodies.
- Linked parts.
- Subassemblies.
- Rigid and revolute joints.
- Joint editing.
- Interference checks.
- Assembly transforms.

Acceptance test: assemble two independently editable parts, constrain their relationship, and verify that their placement survives reopening the project.

### Phase 6: Drawings and manufacturing

Deliver:

- Associative engineering drawings.
- PDF and DXF export where appropriate.
- Initial 2.5D and 3-axis CAM.
- Tool libraries.
- Toolpath simulation.
- Configurable postprocessors.
- Additive manufacturing exports.

Acceptance test: generate a dimensioned drawing from a model, create a supported machining operation, simulate it, and export through a validated postprocessor.

### Phase 7: Advanced capabilities

Add:

- Surface modeling.
- Sheet metal.
- Advanced mesh repair.
- Advanced assembly motion.
- Rendering.
- Simulation integrations.
- Advanced CAM.
- Plugin ecosystem.
- Optional external integrations.

Do not delay the core modeling workflow while implementing less frequently used advanced capabilities.

## 31. Testing and engineering quality

Build automated tests for:

- Sketch solver correctness.
- Geometric operations.
- Boolean validity.
- Parametric recomputation.
- Feature dependency updates.
- Undo and redo.
- Save and reopen.
- Import and export round trips.
- Assembly relationships.
- Unit conversion.
- Document recovery.
- Invalid geometry.
- Memory usage.
- Performance regressions.

Create a collection of representative test models with known dimensions and expected outcomes.

Use geometric tolerances appropriate to each operation and unit system. Never assume that a visually correct viewport proves that a CAD operation is correct.

Include regression tests for previously fixed modeling failures.

## 32. Final product acceptance criteria

The product should not be considered a successful first release unless a user can:

1. Create a new local project without signing in.
2. Create and constrain a sketch.
3. Build a solid using parametric features.
4. Edit earlier features through a timeline.
5. Modify global parameters.
6. Import and edit suitable neutral CAD geometry.
7. Create multiple components.
8. Assemble parts using meaningful constraints.
9. Measure geometry accurately.
10. Export usable STEP and STL files.
11. Save and reopen projects without losing model history.
12. Work completely offline.
13. Install and run the application on supported Linux distributions.
14. Perform everyday modeling without navigating a confusing collection of separate workbenches.

The application must be genuinely usable for mechanical design, not merely visually similar to a CAD program.

## 33. Instructions to the coding agent

Begin by inspecting the repository, available build tools, current implementation, and dependency constraints.

If starting from scratch, first produce:

- A proposed architecture.
- A dependency and licensing assessment.
- A clear module breakdown.
- A phased implementation plan.
- A minimal functional prototype target.
- A test strategy.

Then implement the first working vertical slice.

Do not rewrite a working subsystem without a clear technical reason. Do not create fake implementations for features that have not been completed. Do not claim a feature works until it has been exercised through an actual test or demonstrable workflow.

Keep the application buildable throughout development.

Use real geometry operations, real persistence, real user interactions, and real tests.

When a capability is too large for the current phase, establish the appropriate interfaces and document its status instead of presenting a nonfunctional placeholder as complete.

Make sensible engineering decisions independently, document major tradeoffs, and prioritize a dependable sketch-to-solid-to-edit workflow above feature count.

**The ultimate objective is a professional, offline-first Linux CAD application that preserves the speed and familiarity of Fusion 360's core modeling workflow while providing local ownership, a cleaner interface, fewer distractions, and a maintainable open engineering architecture.**
