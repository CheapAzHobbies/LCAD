# Feature audit against a full CAD checklist

On 2026-11-12 LCAD was checked against a long checklist of what a parametric CAD program is
expected to have, using Fusion 360 as the benchmark. This page says, area by area, what LCAD
has, has in part, and lacks, and in what order the gaps are being closed. It is updated as they
are.

**Have** means it works, is tested, and is in [USING.md](USING.md). **Part** means some of it is
there; the note says what is not. **No** means it is not there at all.

The checklist also asked for NURBS surfacing like Rhino and subdivision modeling like Blender.
LCAD's kernel, Open CASCADE, holds exact B-rep and NURBS geometry, so surfaces can be built on
it without falling back to meshes. Subdivision modeling has no support in OCCT and would be a
large project of its own. Both come after the gaps in mechanical modeling below.

## 1. Sketching

| Item | State | Note |
|---|---|---|
| Line, chains of lines | Have | |
| Rectangle | Part | Two corners, or center and corner. No three-point rectangle. |
| Circle | Have | Center and radius, or three points. |
| Arc | Have | Center, start, and end; three points; tangent on from a curve. |
| Ellipse, polygon, slot, spline, point | Have | Polygon is inscribed only; spline is fit-point only. |
| Construction geometry | Have | Any curve can be made construction. |
| Sketch text | Part | Text is a feature (Create > Text), raised, engraved, or a body; not text in a sketch. |
| Fillet | Have | Between two lines. |
| Chamfer | Have | Equal distance, between two lines. |
| Trim, extend, offset, mirror | Have | |
| Break, split | No | |
| Sketch patterns (rectangular, circular) | Have | Each copy keeps the constraints among the originals; the copies are not tied to each other. |
| Move, copy, rotate, scale | Have | Since 2026-11-12, of what is selected. |
| Project edges of a body | Have | |
| Intersect, project to surface | No | |
| Constraints: coincident, horizontal, vertical, parallel, perpendicular, tangent, equal, concentric, midpoint, collinear, symmetric, fix | Have | |
| Curvature (G2), smooth | No | |
| Dimensions: length, horizontal, vertical, aligned, point to line, radius, diameter, angle | Have | |
| Reference (driven) dimensions | Have | Since 2026-11-12: a dimension that would over-constrain goes in as one, in brackets. |
| Fully constrained detection, what can still move, what conflicts | Have | |
| Dragging dimension values | Have | Since 2026-11-12. |
| Expressions in dimensions | Have | |

### Snapping and automatic constraints

| Item | State | Note |
|---|---|---|
| Snap to points, ends, the origin; Coincident added | Have | Drawing and, since 2026-11-12, dragging. |
| Snap to middles of lines and arcs; Midpoint added | Have | |
| Snap onto curves; point-on-curve added | Have | |
| Horizontal and vertical while drawing a line | Have | |
| Snap to circle centers | Have | Centers are points, so they snap as points. |
| Parallel, perpendicular, tangent detected while drawing | Have | Lines, since 2026-11-12: parallel or square to any line, tangent on from an arc. |
| Equal lengths and radii detected | No | |
| Snap to intersections | Have | Since 2026-11-12. |
| Snap to quadrants | Have | Since 2026-11-12. |
| Grid snapping | Have | Steps that follow the zoom. |
| Marker showing the snap before release | Have | |
| Inference can be switched off | Have | |

## 2. Construction geometry

| Item | State | Note |
|---|---|---|
| Origin planes, axes, point | Have | |
| Offset plane, from an origin plane or a flat face | Have | |
| Plane at angle, about an axis or an edge | Have | Since 2026-11-12. |
| Midplane, between parallel planes or halving an angle | Have | Since 2026-11-12. |
| Offset from another construction plane | Have | Since 2026-11-12. |
| Tangent plane, plane through three points, plane along a path | No | |
| Axis: origin, along an edge, through a cylinder, cone, or torus | Have | |
| Axis through two points, two planes, perpendicular to a face | No | |
| Construction points | No | |
| Rename, hide, edit planes and axes | Have | |
| Drag a plane's offset in the 3D view | Have | Since 2026-11-13, with the pull's arrow. |

## 3. Solids

| Item | State | Note |
|---|---|---|
| Pull (extrude): one side, both sides, symmetric, to a face, taper | Have | |
| Pull through all | Have | Since 2026-11-12. |
| Revolve, loft, sweep | Have | Loft and sweep since 2026-11-12. |
| Hole: simple, counterbore, countersink | Have | |
| Thread | Have | Modeled 60° thread, outside or inside, by pitch. No thread tables yet. |
| Coil, pipe | Have | Since 2026-11-12. |
| Emboss and engrave text | Have | Create > Text. |
| Rib, web, emboss of a sketch onto a curved face | No | |
| Primitives: box, cylinder, sphere, cone, torus | Have | |
| Fillet, chamfer, shell | Have | One radius or distance per feature; chamfer by one distance or two. |
| Draft | Have | Since 2026-11-12, about an origin or construction plane. |
| Offset, move, delete face; repair | Have | |
| Replace face | No | |
| Split body along a plane | Have | Since 2026-11-12. |
| Split body by a face or a sketch, split face | No | |
| Combine bodies as a command | Have | Since 2026-11-12. |
| Move, copy bodies | Have | Since 2026-11-12, by distances and a turn about an axis. |
| Scale bodies | Have | In Move/Copy Body, about the body's middle. |
| Mirror, rectangular and circular patterns | Have | |
| Pattern along a path | No | |

## 4. Surfaces, freeform, mesh, sheet metal

| Item | State | Note |
|---|---|---|
| Surface bodies; surface pull, patch, stitch, thicken | Have | Since 2026-11-12. |
| Offset surface | Have | Since 2026-11-12. |
| Surface revolve | Have | Since 2026-11-13. |
| Surface sweep, loft; trim, extend surfaces; boundary surfaces | No | |
| Subdivision (freeform) modeling | No | See the top of this page. |
| Mesh import (STL, OBJ, 3MF) to solids | Part | Pull request #15, not merged. |
| Mesh export (STL, 3MF, OBJ) | Have | |
| Mesh editing | No | |
| Sheet metal | No | |

## 5. Assemblies

| Item | State | Note |
|---|---|---|
| Components, insert, copy, ground, activate | Have | |
| Joints: rigid, revolute, slider, cylindrical, pin-slot, planar, ball; limits; animation | Have | |
| Interference checking, materials, mass properties | Have | |
| Linked parts from other documents | Have | |
| Rigid groups, motion links, contact sets | No | |
| Exploded views | No | |

## 6. Measuring and inspection

| Item | State | Note |
|---|---|---|
| Distance, angle, length, area, volume, center of mass | Have | |
| Section view in the 3D view | Have | Since 2026-11-12, along an origin or construction plane. |
| Curvature, draft, thickness analysis | No | |
| Geometry check | Have | |

## 7. Parametric history

| Item | State | Note |
|---|---|---|
| Timeline: edit, rename, suppress, roll back, reorder | Have | |
| Parameters with expressions | Have | |
| Dependent features update; failures say why | Have | |
| Undo and redo | Have | |
| Folders in the timeline | No | |
| Configurations | No | |
| Direct editing of imported solids | Have | |

## 8. Drawings

| Item | State | Note |
|---|---|---|
| Base, projected, isometric, section, detail views | Have | |
| Dimensions, center marks, title block | Have | |
| PDF and DXF | Have | |
| Auxiliary and broken views | No | |
| GD&T, surface finish, weld symbols | No | |
| Balloons, parts lists | No | |
| DWG | No | |

## 9. Files

| Item | State | Note |
|---|---|---|
| STEP, IGES import; STEP export | Have | |
| STL, OBJ, 3MF export | Have | |
| IGES export, SAT, DWG | No | |
| Native files with crash recovery | Have | |

## 10. The interface

| Item | State | Note |
|---|---|---|
| Command search, shortcuts that can be changed, context menus | Have | |
| Selection filters, box select, Select Other for overlaps | Have | |
| Standard views, both projections, navigation cube | Have | |
| Display styles, grid | Have | |
| Repeat last command | Have | Edit menu, and first in the 3D view's right-click menu. |
| Saved camera views | No | |
| Grid spacing setting | No | |

## Order of work

1. **Construction geometry:** plane at angle and midplane. Done.
2. **Solids:** pull through all, draft, split body. Done.
3. **Sketching:** center rectangle, three-point circle and arc, tangent arc, chamfer, and sketch
   patterns. Done.
4. **Snapping:** parallel, perpendicular, and tangent detected while drawing, and intersections.
   Done.
5. **Solids, second round:** thread, coil, pipe, combine as a command, and moving bodies. Done.
6. **Inspection:** a section view in the 3D view. Done.
7. **Surfaces:** surface pull, patch, stitch, and thicken. Done. Surface trimming, lofting,
   and offsetting next; then the rest.
