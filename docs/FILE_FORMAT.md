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
| `entities` | The geometry. Every position is a `point`; a `line` names its `start` and `end`, a `circle` its `center` and has a `radius`, an `arc` names its `center`, `start`, and `end` (it runs counter-clockwise from start to end). `owner` is the curve a point belongs to, or 0. The first entity is the sketch's origin: a fixed point at (0, 0) that belongs to nothing. |
| `construction` | Geometry that guides the sketch and is left out of its outlines. |
| `fixed` | Reference geometry, which the solver leaves where it is. |
| `constraints` | `kind` is one of `Coincident`, `PointOnLine`, `PointOnCircle`, `Horizontal`, `Vertical`, `Parallel`, `Perpendicular`, `Tangent`, `Equal`, `Concentric`, `Midpoint`, `Symmetric`, `Collinear`, `Fix`, `Distance`, `DistanceX`, `DistanceY`, `PointLineDistance`, `Radius`, `Diameter`, `Angle`. `entities` are the up to three entities it is about, 0 for none. `value` is a dimension's value in millimeters or radians. |
| `nextEntityId`, `nextConstraintId` | The ids the next entity and constraint get, as `nextId` is for objects. |

Numbers are written with every digit, so a sketch comes back exactly as it was saved and is as
constrained on reopening as it was on saving. A sketch that does not fit together (a line whose
end is not a point, a constraint on entities it does not take, no origin) makes the file
damaged, and the file is refused as a whole like any other.

A reader ignores fields and archive entries it does not know, so that a later LCAD can add
optional ones without changing the version. Everything listed above is required, and a file
with a missing or wrong value is refused as a whole. LCAD never opens a document partially.

### Geometry

Each file in `geometry/` is one shape in OCCT's binary B-rep format, without display meshes.
Display meshes are regenerated when a document is opened.

### What is authoritative

`document.json` is the model. A file in `geometry/` is one of two things, and its `role` says
which:

- **`source`: the only copy.** In version 1 every body is one of these: a solid made directly,
  with no history that could rebuild it. Imported geometry will be the same.
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
- Autosave and crash recovery (Phase 5) will not write to the user's file at all. They will keep
  a journal under `$XDG_STATE_HOME/lcad/recovery/` (by default `~/.local/state/lcad/recovery/`),
  and after a crash LCAD will offer to restore from it.
- Saving a document, saving a copy, exporting geometry, and packaging a project are four separate
  commands, as the brief requires. Only the first two write `.lcad` files. Phase 1 has the first.

## Linked components

Planned for Phase 5, and not part of version 1. A component inserted from another file is stored as a reference: the path relative to the
referring document, plus an identifier that lets LCAD tell when the path now leads to a different
file. A missing link is reported; the last known geometry stays visible from the cache. A dependency
report and a command that gathers every linked file into one portable folder come with it.

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

- **Where a sketch came from.** A sketch started on a face keeps the plane, not the face. A
  projected edge keeps the geometry, not the edge. Both need the stable references of Phase 3.
- **Where a dimension's value is drawn.** Not stored: values place themselves.

- **The notice before converting.** When the first migration exists, LCAD has to say so before
  a save converts an older file to the newer format. There is no older format yet.
- **Inches.** `units.length` will accept `"in"` when LCAD can show lengths in inches.
- **Cached display meshes.** Not stored. To be reconsidered if large documents open slowly.
