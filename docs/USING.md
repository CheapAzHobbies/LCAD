# Using LCAD

What LCAD can do today, and how to do it. This grows with each phase; the
[roadmap](../ROADMAP.md) says what comes next.

LCAD cannot sketch or extrude yet. For now a solid is made from a primitive: a box, cylinder,
sphere, cone, or torus, by its sizes. Sketching arrives in Phase 2, and extrude, revolve,
fillet, and the timeline in Phase 3. Before those, Phase 1b reworks the look of the interface
and adds themes, command search, and tabs.

## The window

| Region | What it is for |
|---|---|
| Top bar | Menus; buttons for new, open, save, undo, and redo; the document's name. |
| Toolbar | The modeling tools. **Create** makes a primitive. **Select** sets what a click picks. |
| Browser (left) | The document's bodies. Tick a body to show it, untick to hide it, double-click to rename it. Right-click for more. |
| 3D view | The model. The cube in the corner turns the view when you click one of its sides, edges, or corners. |
| Timeline (bottom) | The feature history. Empty until LCAD has parametric features. |

## Making a solid

1. Click a shape under **Create** in the toolbar, or pick it from the Create menu.
2. A panel opens on the right, and a see-through preview appears in the 3D view. Type the sizes
   and, if you want, where the solid goes. Sizes are in millimeters.
3. Click **OK**, or press Enter. Press Esc, or click Cancel, to give up.

Each solid you create is one undo step.

## Moving around the 3D view

LCAD uses Fusion's mouse controls.

| To | Do this |
|---|---|
| Zoom | Roll the wheel. The view zooms about the pointer. |
| Pan | Drag with the middle button. |
| Orbit | Hold Shift and drag with the middle button. |
| Zoom by dragging | Hold Ctrl + Shift and drag with the middle button. |
| Fit everything in view | Double-click the middle button, or press F6. |
| Go to the home view | Press Home. |
| Look from the front, top, and so on | View menu, or click the cube. |

Rolling the wheel away from you zooms in. **View > Reverse Zoom Direction** swaps that, and LCAD
remembers the choice. **View > Perspective** switches between perspective and the default
orthographic projection.

## Selecting

| To | Do this |
|---|---|
| Select what is under the pointer | Click it. It is outlined as the pointer passes over it. |
| Add to, or take from, the selection | Shift + click, or Ctrl + click. |
| Select everything inside a rectangle | Drag with the left button. |
| Clear the selection | Click empty space. |
| Select a whole body | Click it in the browser, or set **Select** to Bodies and click it. |

**Select** in the toolbar sets what a click picks: faces, edges, and vertices together (the
default), or only bodies, faces, edges, or vertices.

The status bar measures what is selected: the area of a face, the length of an edge, the
position of a vertex, or the volume and area of a body.

## Keyboard

| Key | Action |
|---|---|
| Ctrl + N, Ctrl + O | New document, open a document |
| Ctrl + S, Ctrl + Shift + S | Save, save under another name |
| Ctrl + Z | Undo |
| Ctrl + Shift + Z, or Ctrl + Y | Redo |
| Delete | Delete the selected bodies |
| Esc | Leave the command in progress |
| F6, Home | Fit the view, go to the home view |
| F2, in the browser | Rename the body |
| Ctrl + Q | Quit |

## Documents

A document is one `.lcad` file. LCAD asks before closing a document with unsaved changes.
Saving replaces the file in one step, so a crash or power cut during a save leaves you with
either the old version or the new one, never half of each.

To open a document from a terminal or a file manager, give its name to LCAD: `lcad part.lcad`.

Not there yet: autosave and crash recovery, several documents open at once, inches as a display
unit, and import or export of other formats.

## If something goes wrong

LCAD keeps a log at `~/.local/state/lcad/lcad.log` (or under `$XDG_STATE_HOME` if that is set).
Help > About shows the path, along with the versions of LCAD, Qt, and Open CASCADE and the
graphics driver in use. Include both when reporting a problem.
