# Using LCAD

What LCAD can do today, and how to do it. This grows with each phase; the
[roadmap](../ROADMAP.md) says what comes next.

LCAD cannot sketch or extrude yet. For now a solid is made from a primitive: a box, cylinder,
sphere, cone, or torus, by its sizes. Sketching arrives in Phase 2, and extrude, revolve,
fillet, and the timeline in Phase 3.

## The window

| Region | What it is for |
|---|---|
| Top bar | Menus; buttons for new, open, save, undo, and redo; a tab for each open document; command search. |
| Toolbar | The modeling tools. **Create** makes a primitive. **Select** sets what a click picks. **View** fits the view, shows the grid, and sets how bodies are drawn. |
| Browser (left) | The document as a tree. Tick a body to show it, untick to hide it, double-click to rename it, right-click for more. The box above the tree filters bodies by name. Under **Origin** are the reference planes and axes, which can be ticked to show them as guides. |
| 3D view | The model. |
| Properties (right) | While a command runs, its settings. Otherwise, what is selected: a body's name, visibility, and measurements, or the area, length, or position of a face, edge, or vertex. |
| Timeline (bottom) | The feature history. Empty until LCAD has parametric features. |
| Status bar | What is selected and its measurement, the command in progress, short messages such as "Saved". |

## Finding a command

Press **S** (or Ctrl + K) and type part of a command's name, or another word for it: "ball"
finds Sphere, "zoom all" finds Fit, "dark mode" finds the dark theme. Enter starts the
highlighted command, the arrow keys move the highlight, and Esc closes the box. With nothing
typed, the box lists the commands you used last.

Every command is also in a menu. Commands that cannot be used at the moment are shown grayed.

## Making a solid

1. Click a shape under **Create** in the toolbar, or find it with the command search.
2. Its settings open on the right, and a see-through preview appears in the 3D view and follows
   what you type. Sizes are in millimeters.
3. Press Enter, or click **OK**. Press Esc, or click Cancel, to give up.

Each solid you create is one undo step. You can turn and zoom the view while the panel is open.

## Moving around the 3D view

| To | Do this |
|---|---|
| Zoom | Roll the wheel. The view zooms about the pointer. |
| Pan | Drag with the middle button. |
| Orbit | Hold Shift and drag with the middle button, or drag the view cube. |
| Zoom by dragging | Hold Ctrl + Shift and drag with the middle button. |
| Fit everything in view | Double-click the middle button, or press F6. |
| Zoom to what is selected | Press Shift + F6. |
| Go to the home view | Press Home. |
| Look from the front, top, and so on | Click a side, edge, or corner of the view cube, or use the View menu. |

**View > Mouse** changes this:

- **Middle Button Orbits** swaps pan and orbit: the middle button alone orbits, Shift + middle
  pans, and Ctrl + middle zooms.
- **Reverse Zoom Direction** makes rolling the wheel away from you zoom out instead of in.

**View > Perspective** switches between perspective and the default orthographic projection.
**View > Animate View Changes** can be turned off to make the view cube turn the camera at once.

## How the model is drawn

**View > Display Style**, or the last button of the toolbar, offers shaded with edges (the
default), shaded, wireframe, hidden line, and transparent. **View > Grid** shows a reference
grid on the XY plane with lines 10 mm apart, and **View > Axis Marker** the X, Y, Z marker in
the corner.

## Selecting

| To | Do this |
|---|---|
| Select what is under the pointer | Click it. It is outlined as the pointer passes over it. |
| Add to, or take from, the selection | Shift + click, or Ctrl + click. |
| Select everything inside a rectangle | Drag with the left button. |
| Clear the selection | Click empty space. |
| Select a whole body | Click it in the browser, or set **Select** to Bodies and click it. |
| Select something that lies behind something else | Right-click there and choose from **Select Other**. |

**Select** in the toolbar sets what a click picks: faces, edges, and vertices together (the
default), or only bodies, faces, edges, or vertices.

Right-clicking in the 3D view or on a body in the browser opens a menu of what can be done with
it: hide, isolate (hide everything else), zoom to it, delete, rename.

## Several documents

Each open document has a tab in the top bar, with a `*` after its name while it has unsaved
changes. Ctrl + N opens a new one, Ctrl + W closes the current one. Every document keeps its own
camera, undo history, and origin guides. **File > Open Recent** lists the last ten documents.

A document is one `.lcad` file. LCAD asks before closing a document with unsaved changes.
Saving replaces the file in one step, so a crash or power cut during a save leaves you with
either the old version or the new one, never half of each.

To open a document from a terminal or a file manager, give its name to LCAD: `lcad part.lcad`.

## The window itself

- **Window > Theme** chooses a light or a dark interface, or follows the desktop.
- Panels can be dragged to another side, resized, or hidden from the **Window** menu. LCAD
  remembers the arrangement. **Window > Reset Layout** puts everything back.
- **Window > Focus on 3D View** (F11) hides the panels and the toolbar; the same command brings
  them back.
- **Window > Keyboard Shortcuts** lists every command with its shortcut and lets you change it.

## Keyboard

These are the shortcuts LCAD starts with.

| Key | Action |
|---|---|
| S, or Ctrl + K | Search commands |
| Ctrl + N, Ctrl + O, Ctrl + W | New document, open a document, close the document |
| Ctrl + S, Ctrl + Shift + S | Save, save under another name |
| Ctrl + Z | Undo |
| Ctrl + Shift + Z, or Ctrl + Y | Redo |
| Delete | Delete the selected bodies |
| F2 | Rename the selected body |
| Esc | Leave the command in progress |
| F6, Shift + F6, Home | Fit the view, zoom to the selection, go to the home view |
| F11 | Give the window to the 3D view, and back |
| Ctrl + Q | Quit |

Not there yet: autosave and crash recovery, inches as a display unit, and import or export of
other formats. The origin guides that are shown are not saved with the document.

## If something goes wrong

LCAD keeps a log at `~/.local/state/lcad/lcad.log` (or under `$XDG_STATE_HOME` if that is set).
Help > About shows the path, along with the versions of LCAD, Qt, and Open CASCADE and the
graphics driver in use. Include both when reporting a problem.
