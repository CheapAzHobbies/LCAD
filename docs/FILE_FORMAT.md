# Native file format

The specification of format version 1, the format LCAD reads and writes. The design was decided
on 2026-10-12 and implemented in Phase 1; `src/document/src/File.cpp` is the implementation.

## Decision

An LCAD document is one file with the extension `.lcad`. It is a zip archive that holds the model
as JSON and its geometry as binary B-rep.

```
bracket.lcad
├── manifest.json        format version, the LCAD version that wrote the file, units
├── document.json        the model: components, bodies, sketches, parameters, feature history
├── geometry/
│   └── <id>.brep        one exact shape per file, in OCCT's binary B-rep format
└── thumbnail.png        preview for file managers and the open dialog
```

The archive is an ordinary zip file: `unzip -l part.lcad` lists it, and the two JSON files are
indented text.

### manifest.json

```json
{
  "format": "lcad",
  "formatVersion": 1,
  "writtenBy": {
    "application": "LCAD",
    "version": "0.0.1",
    "kernel": "OCCT 7.6.3"
  },
  "units": {
    "length": "mm"
  }
}
```

| Field | Meaning |
|---|---|
| `format` | Always `"lcad"`. A zip file without it is not an LCAD document. |
| `formatVersion` | The version of this specification the file follows. |
| `writtenBy` | The program that wrote the file, and the geometry kernel it was built with. For diagnosis only; nothing is decided from it. |
| `units.length` | The unit lengths are shown in. Version 1 knows only `"mm"`, and a reader refuses any other value. Geometry is stored in millimeters whatever this says. |

### document.json

```json
{
  "nextId": 3,
  "bodies": [
    {
      "id": 1,
      "name": "Box",
      "visible": true,
      "geometry": {
        "file": "geometry/1.brep",
        "role": "source"
      }
    }
  ]
}
```

| Field | Meaning |
|---|---|
| `nextId` | The id the next new object gets. Larger than every id in the file. Ids are never reused, so this can be larger than the number of objects. |
| `bodies` | The bodies, in the order they were created. |
| `id` | A whole number greater than zero, unique in the document. |
| `name` | Text, not empty. Names need not be unique. |
| `visible` | Whether the body is shown. |
| `geometry.file` | The archive entry that holds the body's shape. |
| `geometry.role` | What that entry is to the document; see below. Version 1 knows only `"source"`. |

`document.json` may also hold `sketches`, a list. It is optional: a document without sketches
has no such field, and a file written before sketches existed opens as it always did.

```json
{
  "id": 2,
  "name": "Sketch 1",
  "visible": true,
  "plane": { "origin": [0, 0, 0], "normal": [0, 0, 1], "xDirection": [1, 0, 0] },
  "nextEntityId": 5,
  "nextConstraintId": 3,
  "entities": [
    { "id": 1, "kind": "point", "x": 0, "y": 0, "owner": 0, "construction": false, "fixed": true },
    { "id": 2, "kind": "line", "start": 3, "end": 4, "construction": false, "fixed": false },
    { "id": 3, "kind": "point", "x": 0, "y": 0, "owner": 2, "construction": false, "fixed": false },
    { "id": 4, "kind": "point", "x": 40, "y": 0, "owner": 2, "construction": false, "fixed": false }
  ],
  "constraints": [
    { "id": 1, "kind": "Horizontal", "entities": [2, 0, 0], "value": 0, "value2": 0 },
    { "id": 2, "kind": "Distance", "entities": [3, 4, 0], "value": 40, "value2": 0 }
  ]
}
```

| Field | Meaning |
|---|---|
| `id`, `name`, `visible` | As for a body. Sketches and bodies share one range of ids. |
| `plane` | Where the sketch sits in space: a point of the plane, its normal (the side the sketch is looked at from), and the direction of the sketch's own X. The directions are of unit length and square to each other. |
| `entities` | The geometry. Every position is a `point`; a `line` names its `start` and `end`, a `circle` its `center` and has a `radius`, an `arc` names its `center`, `start`, and `end` (it runs counter-clockwise from start to end), an `ellipse` names its `center` and `major` (the end of its major axis) and has a `minorRadius`, and a `spline` lists its `controlPoints` (a clamped B-spline of degree 3, or less with fewer points, with its knots spread evenly). `owner` is the curve a point belongs to, or 0. The first entity is the sketch's origin: a fixed point at (0, 0) that belongs to nothing. |
| `construction` | Geometry that guides the sketch and is left out of its outlines. |
| `fixed` | Reference geometry, which the solver leaves where it is. |
| `constraints` | `kind` is one of `Coincident`, `PointOnLine`, `PointOnCircle`, `Horizontal`, `Vertical`, `Parallel`, `Perpendicular`, `Tangent`, `Equal`, `Concentric`, `Midpoint`, `Symmetric`, `Collinear`, `Fix`, `Distance`, `DistanceX`, `DistanceY`, `PointLineDistance`, `Radius`, `Diameter`, `Angle`. `entities` are the up to three entities it is about, 0 for none. `value` is a dimension's value in millimeters or radians. |
| `nextEntityId`, `nextConstraintId` | The ids the next entity and constraint get, as `nextId` is for objects. |

Numbers are written with every digit, so a sketch comes back exactly as it was saved and is as
constrained on reopening as it was on saving. A sketch that does not fit together (a line whose
end is not a point, a constraint on entities it does not take, no origin) makes the file
damaged, and the file is refused as a whole like any other.

`document.json` may also hold `features`, `timeline`, and `marker`. Like `sketches` they are
optional, and a file without them opens as it always did.

```json
{
  "id": 7, "name": "Pull 1", "kind": "Pull", "suppressed": false,
  "body": 8, "sketch": 2, "regions": [[3, 6, 9, 12]],
  "operation": "NewBody", "distance": 8, "distanceBack": 0, "symmetric": false,
  "axis": 0, "angle": 360, "edges": [], "faces": [], "size": 1
}
```

| Field | Meaning |
|---|---|
| `kind` | `Pull`, `Revolve`, `Fillet`, `Chamfer`, `Shell`, `Mirror`, `Rectangular Pattern`, `Circular Pattern`, `Plane` (a construction plane), `Hole`, `Axis` (a construction axis), `Offset Face`, `Move Face`, `Delete Face`, or `Repair`. |
| `suppressed` | Left out of the model, and kept. |
| `body` | The body it changes, or for a pull or revolve with `operation` `NewBody`, the body it makes. |
| `sketch`, `regions` | A pull's or revolve's sketch, and its closed areas, each by the ids of the curves around it, sorted. An empty list means every area of the sketch. |
| `operation` | `NewBody`, `Join`, `Cut`, or `Intersect`. |
| `distance`, `distanceBack`, `symmetric` | How far a pull goes along the sketch's normal, how far the other way (0 for one side), and whether `distance` is split evenly to both sides. |
| `axis`, `angle` | The sketch line a revolve turns about, and how far, in degrees. A circular pattern's `angle` too. |
| `reference` (revolve) | Written only when a revolve turns about an axis of the model rather than a sketch line: `X`, `Y`, `Z`, or `axis:` and the id of a construction axis. Its `axis` is then 0. |
| `edges`, `faces` | A fillet's or chamfer's edges, a shell's open faces, the faces an Offset Face, Move Face, or Delete Face changes, by name (see ARCHITECTURE.md, section 11). |
| `reference` (move face) | Move Face only: the direction, as for a pattern (`X`, `Y`, `Z`, the name of an edge or face, or `axis:` and an id); its `distance` is how far. An Offset Face's `distance` is how far along the faces' normals. |
| `size` | A fillet's radius, a chamfer's distance, a shell's wall thickness, a hole's diameter. |
| `taper` | A pull only, and only when not zero: how far its sides lean in, in degrees. |
| `extentBody`, `extentFace` | A pull up to a face only: the body, and the name of its flat face, that the pull ends in. |
| `sources`, `reference`, `reference2`, `count`, `count2`, `spacing`, `spacing2` | Mirror and patterns only: the features copied (empty: the whole body), the plane, direction, or axis (`XY`, `XZ`, `YZ`, `X`, `Y`, `Z`, a face or edge name, `plane:` and the id of a construction plane, or `axis:` and the id of a construction axis), how many, and how far apart. |
| `holeKind`, `throughAll`, `tipAngle`, `headDiameter`, `headDepth`, `countersinkAngle`, `flipped` | A hole only: `Simple`, `Counterbore`, or `Countersink`; right through or to `distance` with a point of `tipAngle` degrees; the width and depth of a counterbore or the angle of a countersink; and whether it is drilled toward the side the sketch is seen from. Its `size` is the diameter, and the holes go at the free-standing points of its `sketch`. |
| `reference`, `visible` | A construction plane only: what it is placed against (`XY`, `XZ`, `YZ`, or the name of a flat face of `body`), and whether it is shown. Its `distance` is how far it is from that, along its normal. It makes no body; `body` is 0 for one placed against an origin plane. |
| `reference`, `visible` (axis) | A construction axis only: what it runs along (`X`, `Y`, `Z`, or the name of an edge or a face of `body`: a straight edge, or a round edge or face, whose axis it is), and whether it is shown. It makes no body; `body` is 0 for an origin axis. |

`document.json` may also hold `parameters`, a list of `{"id", "name", "expression", "comment"}`.
A feature may have `expressions`, an object from the name of a value (`distance`, `size`,
`count`, and so on) to the expression it is worked out from, and a sketch `expressions`, from
the id of a dimension to its expression. The values stored next to them are what the
expressions came to when the file was saved; on opening they are worked out again.

A sketch that follows something has `attachedTo`, the id of a construction plane or of a body,
and `attachedFace`, the name of the body's face (empty for a plane). Its `plane` is then where it
was when the file was saved, and it is placed again from what it follows on opening.

A body that a feature makes is written as `{"id", "name", "visible", "madeBy"}`, without
geometry: it is built again from its feature on opening.

### Components and assemblies

Added in Phase 5, still in version 1: each of the following is optional, and a file without it
opens as it always did.

A body, a sketch, and a feature may have `component`, the id of the component it is part of
(none for the document itself; its geometry, or its plane, is then in the component's own
coordinates). A body may have `material`, the name of what it is made of, and a body no feature
makes, or a feature, may have `tag`, the text the names of its faces start with in place of its
id (a copy of a component keeps its original's, so that the copy's features find their faces).

A body may also have `piece`, set where a feature leaves solids that do not touch. The body the
feature works on holds the first of them, and `piece` says which of the others this body holds,
from 1, with `madeBy` naming that feature. A body with `discarded` set to true was deleted while
the feature behind it stayed for the sake of its other solids: it has no shape, and is not made
again.

`components` lists `{"id", "name"}`, and for a linked part `link`, the path of its document
relative to this one's folder (or whole), and `linkStamp`, what identifies the content that
file had when it was last read. Each component appears at least once in `occurrences`:

```json
{ "id": 21, "component": 20, "parent": 0,
  "placement": { "origin": [80, 20, 15], "xAxis": [1, 0, 0], "yAxis": [0, 1, 0], "zAxis": [0, 0, 1] },
  "grounded": false, "visible": true, "suppressed": false }
```

| Field | Meaning |
|---|---|
| `component` | The component that appears. |
| `parent` | The component it appears in, 0 for the document itself. A component never appears inside itself. |
| `placement` | Where it is in the parent's coordinates when no joint places it: its origin and its X, Y, and Z directions, of unit length, square, and right-handed. |
| `grounded` | Joints move the other side, not this one. |
| `visible`, `suppressed` | Shown; left out of the assembly altogether. |

`joints` lists the joints:

```json
{ "id": 30, "name": "Revolute 1", "kind": "Revolute", "component": 0,
  "moving": { "path": [23], "kind": "face", "body": 24, "name": "24/f5", "snap": "center" },
  "fixed": { "path": [21], "kind": "face", "body": 22, "name": "22/f4", "snap": "center" },
  "offset": 0, "angle": 0, "flipped": false,
  "values": [30, 0, 0], "limits": [{ "minimum": -90, "maximum": 90 }, {}, {}],
  "suppressed": false }
```

| Field | Meaning |
|---|---|
| `kind` | `Rigid`, `Revolute`, `Slider`, `Cylindrical`, `Pin-Slot`, `Planar`, or `Ball`. |
| `component` | The component the joint is part of; both sides lie in it. 0 for the document itself. |
| `moving`, `fixed` | Where each side is: `path`, the occurrences from the joint's component down to the part (empty for the component's own bodies); `kind`, `origin`, `face`, or `edge`; the `body` and the `name` of its face or edge; and `snap`, `center`, `start`, or `end`, along a straight edge or a round face. |
| `offset`, `angle`, `flipped` | How far the moving side stands off along the joint's axis, in millimeters, and turns about it, in degrees, and whether it faces the same way rather than against. |
| `values`, `limits` | How far the joint has moved along each way it moves (in degrees or millimeters, in the order its kind lists them), and the least and most for each, where there are limits. |

`materials` lists the document's own materials, `{"name", "density"}`, the density in grams per
cubic centimeter. Names not among them are LCAD's own materials.

A body of a linked part is stored like a body put in as it is, with `"role": "linked"` for its
geometry, and `linkKey`, which tells it apart among the bodies of the linked document, and
`faceNames`, the names its faces have there. It is not in the `timeline`: the linked document is
where it is made. It is the last known shape of that body, shown when the linked file cannot be
found. `timeline` lists the ids of the bodies
put in as they are, the sketches, and the features in the order they are applied; `marker`, if
present, is how many of them are applied. A feature that cannot be applied on opening does not
stop the file from opening: it is reported, with its reason, as a warning.

A reader ignores fields and archive entries it does not know, so that a later LCAD can add
optional ones without changing the version. Everything listed above is required, and a file
with a missing or wrong value is refused as a whole. LCAD never opens a document partially.

### Drawings, tools, and setups

These lists, each left out when empty, keep what the user chose. The views on a sheet and the
toolpaths of a setup are worked out from the model when they are wanted, and are not stored.

`drawings` is a list of sheets:

| Field | Meaning |
|---|---|
| `id`, `name` | As for other objects |
| `size` | `A4` to `A0`, `Letter`, `Tabloid`, `ANSI C`, `ANSI D` |
| `standard` | `iso` for first-angle projection, `ansi` for third-angle |
| `titleBlock` | `title`, `partNumber`, `revision`, `drawnBy`, `date`, `material`, `company` |
| `views` | Each with `id`, `kind` (`front`, `back`, `top`, `bottom`, `left`, `right`, `isometric`, `section`, `detail`), `label`, `scale` (sheet millimeters per model millimeter), `x` and `y` (its middle on the sheet), `showHidden`, `centerMarks`. A section or a detail has `parent`. A section has `sectionVertical`, `sectionPosition`, `sectionFlipped`. A detail has `detailX`, `detailY`, `detailRadius` |
| `dimensions` | Each with `id`, `view`, `kind` (`aligned`, `horizontal`, `vertical`, `radius`, `diameter`), `first`, and but for a radius or a diameter `second`, then `offset` and `suffix`. A point is `body`, `path`, `edge` (the edge's name), and `where` (`start`, `end`, `middle`, `center`) |
| `notes` | Each with `id`, `text`, `x`, `y`, `height` |

`tools` is a list with `id`, `name`, `kind` (`flat`, `ball`, `drill`), `diameter`, `fluteLength`,
`stickout`, `flutes`, `tipAngle`, and `number`.

`setups` is a list with:

| Field | Meaning |
|---|---|
| `id`, `name` | As for other objects |
| `machine` | `name`, `dialect` (`grbl`, `linuxcnc`), `travelX`, `travelY`, `travelZ`, `rapidRate`, `maxFeed`, `maxSpindle` |
| `stock` | `side`, `top`, `bottom`: how much around the part's box |
| `origin` | `stock top corner` or `stock top center` |
| `operations` | Each with `id`, `name`, `kind` (`face`, `pocket`, `contour`, `drill`), `tool`, `targets` (each `body`, `path`, and `face`, the face's name), `feed`, `plungeFeed`, `spindle`, `stepover`, `stepdown`, `clearance`, `retract`, `stockToLeave`, `side` (`outside`, `inside`), `peck`, `suppressed` |

### Geometry

Each file in `geometry/` is one shape in OCCT's binary B-rep format, without display meshes.
Display meshes are regenerated when a document is opened.

### What is authoritative

`document.json` is the model. A file in `geometry/` is one of three things, and its `role` says
which:

- **`source`: the only copy.** A solid made directly, with no history that could rebuild it, or
  imported from another program's file.
- **`linked`: a copy.** The last known shape of a body of a linked part, kept for when its file
  cannot be found. The linked file is what is authoritative.
- **A cached result** (not in version 1). When features arrive, the shape a feature produced
  will be stored so that a document opens without recomputing, and can still show its geometry
  in a version of LCAD whose kernel would recompute it differently. That changes what a reader
  must do with the file, so it comes with a new format version.

### Preview

`thumbnail.png` is optional: a picture of the 3D view, at most 256 pixels on a side, taken when
the document was saved.

### Limits

A reader refuses an archive holding more than 4 GiB of uncompressed data, so that a damaged or
hostile file cannot exhaust memory. Every entry is checked against its zip checksum.

## Versioning

- `manifest.json` carries `formatVersion`, an integer that starts at 1.
- It increases whenever a file written the new way would be misread by an older LCAD. Adding
  an optional field that old readers can ignore does not increase it.
- LCAD opens older versions by running migrations in order: one function per step, from version
  N to N+1. Migration happens in memory. The file on disk is not touched until the user saves,
  and LCAD says so when saving will convert a file to a newer format.
- LCAD refuses a file whose `formatVersion` is newer than it understands, and names the LCAD
  version needed. It does not open such a file partially.
- Every released format version keeps a sample file in the test suite, so each migration stays
  tested for as long as LCAD can read that version.
- Version 1 is not frozen until LCAD's first release. Until then there is no older LCAD in
  anyone's hands to protect, so what a phase adds goes into version 1: sketches were added in
  Phase 2 as an optional field. The first release fixes version 1 as it is on that day.

### B-rep compatibility between OCCT versions

The same LCAD release is built against OCCT 7.6 on one distribution and 7.9 on another, and a
file saved on either must open on the other. OCCT's binary B-rep format has numbered versions of
its own, and its writer takes the version as an argument. LCAD always writes B-rep format
version 4, which OCCT 7.6 reads and writes and newer versions still read, and never "whatever is
current".

This is tested, not assumed. `tests/models/format-1/` holds the same document as written by
LCAD built against each OCCT series, every build reads all of them, and CI builds against both
7.6 and 7.9.

## Saving

- A save never modifies the existing file in place. LCAD writes the complete new archive to a
  temporary file in the same directory, flushes it to disk, and renames it over the target.
  A rename within one directory is atomic, so after a crash or power loss the file is either the
  whole old version or the whole new one. The directory entry is flushed too.
- The new file keeps the permissions of the one it replaces. Saving through a symbolic link
  updates the file the link points to.
- If a save fails, the existing file is untouched and no temporary file is left behind.
- No backup copy of the previous version is kept beside the document. The atomic replacement
  covers a failed save, and keeping old versions is the job of autosave and of version control.
- Autosave never writes to the user's file. Every minute LCAD writes a copy of each document with
  unsaved changes, as a complete `.lcad` file and in the same way as a save, into a folder of its
  own under `$XDG_STATE_HOME/lcad/recovery/` (by default `~/.local/state/lcad/recovery/`), with
  a note of the file it belongs to. After a crash LCAD offers to restore from it.
- Saving a document, saving a copy, exporting geometry, and packaging a project are four separate
  commands, as the brief requires. Saving and packaging write `.lcad` files; a package also
  writes copies of the files the document links to, with their links pointing at each other.

## Linked components

A component inserted from another file is stored as a link: the path relative to the referring
document's folder (`link`), and a stamp of the file's content (`linkStamp`: its size and a hash
of its bytes), which tells LCAD when the file changed. The bodies of the linked document are
copied into this one (role `linked`, above). A missing link is reported, and the copy is shown.
File > Linked Files is the dependency report, and Package Project gathers every linked file into
one portable folder. When a document is saved in another folder, its links are rewritten to lead
to the same files from there.

## Why this and not something else

| Option | Why not |
|---|---|
| A SQLite database | Its strength is updating part of a large file in place. An LCAD document's source of truth is a small history plus cached shapes, and rewriting it whole is cheap. In exchange SQLite gives up a format that can be inspected with an unzip tool and a text editor. Very large assemblies are handled by linking part files, not by one huge file. |
| A directory of files | Friendly to version control, but users expect a document to be one file they can email, copy to a USB stick, or attach to an issue. |
| OCCT's own document format | Follows from using OCCT's application framework for the model, which [ARCHITECTURE.md](ARCHITECTURE.md) decides against. |
| A single JSON or XML file | Binary geometry would have to be encoded as text, which is large and slow. |

A zip container holding a text description plus binary parts is also how comparable formats are
built, FreeCAD's `.FCStd` and 3MF among them.

## Libraries

libzip for the archive, nlohmann/json for JSON, and OCCT's `BinTools` for B-rep. Versions and
licenses are in [DEPENDENCIES.md](DEPENDENCIES.md).

## Not done yet

- **Where a projected edge came from.** A projected edge keeps the geometry, not the edge. (A
  sketch on a face now follows the face; see `attachedTo` above.)
- **Where a dimension's value is drawn.** Not stored: values place themselves.

- **The notice before converting.** When the first migration exists, LCAD has to say so before
  a save converts an older file to the newer format. There is no older format yet.
- **Inches.** `units.length` will accept `"in"` when LCAD can show lengths in inches.
- **Cached display meshes.** Not stored. To be reconsidered if large documents open slowly.
