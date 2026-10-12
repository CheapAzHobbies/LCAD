# Native file format

Decided on 2026-10-12. Nothing here is implemented yet; Phase 1 builds saving and loading to
this design, and this document then becomes the format's specification.

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

### What is authoritative

`document.json` is the model. A file in `geometry/` is one of two things:

- **A cached result.** The shape a feature produced when the document was last recomputed. It
  lets a document open without recomputing, and lets it still show its geometry in a version of
  LCAD whose kernel would recompute it differently. It can always be regenerated from the history.
- **Imported geometry.** A shape that came from a STEP or IGES file has no history to regenerate
  it from, so this file is the only copy. `document.json` records which kind each file is.

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

### B-rep compatibility between OCCT versions

The same LCAD release is built against OCCT 7.6 on one distribution and 7.9 on another, and a
file saved on either must open on the other. OCCT's binary B-rep format has numbered versions of
its own, and its writer takes the version as an argument. LCAD therefore always writes one pinned
B-rep format version that the oldest supported OCCT can read, and never "whatever is current".
Phase 1 has to prove this with a file written under each OCCT version and read under the other.

## Saving

- A save never modifies the existing file in place. LCAD writes the complete new archive to a
  temporary file in the same directory, flushes it to disk, and renames it over the target.
  A rename within one directory is atomic, so after a crash or power loss the file is either the
  whole old version or the whole new one.
- Autosave and crash recovery do not write to the user's file at all. They keep a journal under
  `$XDG_STATE_HOME/lcad/recovery/` (by default `~/.local/state/lcad/recovery/`). After a crash,
  LCAD offers to restore from it.
- Saving a document, saving a copy, exporting geometry, and packaging a project are four separate
  commands, as the brief requires. Only the first two write `.lcad` files.

## Linked components

A component inserted from another file is stored as a reference: the path relative to the
referring document, plus an identifier that lets LCAD tell when the path now leads to a different
file. A missing link is reported; the last known geometry stays visible from the cache. Phase 5
adds the dependency report and the command that gathers every linked file into one portable
folder.

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

## Left for Phase 1

- The schema of `document.json`.
- Whether saving keeps the previous version as a backup file beside the document.
- Whether to cache display meshes in the file so large documents open faster. The default is no:
  meshes are regenerated.
