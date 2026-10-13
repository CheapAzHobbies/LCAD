# Using LCAD

What LCAD can do today, and how to do it. This grows with each phase; the
[roadmap](../ROADMAP.md) says what comes next.

LCAD can sketch, and cannot extrude yet. A sketch is drawn, constrained, and dimensioned, and
then waits: turning it into a solid (extrude, revolve) arrives in Phase 3, with fillet and the
timeline. Until then a solid is made from a primitive: a box, cylinder, sphere, cone, or torus,
by its sizes.

## The window

| Region | What it is for |
|---|---|
| Top bar | Menus; buttons for new, open, save, undo, and redo; a tab for each open document; command search. |
| Toolbar | The modeling tools. **Create** starts a sketch or makes a primitive. **Select** sets what a click picks. **View** fits the view, shows the grid, and sets how bodies are drawn. While a sketch is open, the sketch tools are here instead. |
| Browser (left) | The document as a tree. Tick a body or a sketch to show it, untick to hide it, right-click for more. Double-click a body to rename it, or a sketch to open it. The box above the tree filters by name. Under **Origin** are the reference planes and axes, which can be ticked to show them as guides. |
| 3D view | The model. |
| Properties (right) | While a command runs, its settings. While a sketch is open, the sketch: whether it is fully constrained, and the constraints on what is selected. Otherwise, what is selected: a body's name, visibility, and measurements, or the area, length, or position of a face, edge, or vertex. |
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

## Sketching

A sketch is flat geometry on a plane: lines, circles, and arcs, held in shape by constraints
and dimensions.

### Starting and finishing

1. Click **New Sketch** in the toolbar (or press S and type "sketch").
2. Choose a plane in the panel on the right: XY, XZ, or YZ. Or click a flat face of a body. If a
   flat face was already selected, the sketch goes straight onto it.
3. The view turns to face the plane and the toolbar shows the sketch tools.
4. Draw. When done, click **Finish Sketch** at the right of the toolbar, or press Ctrl + Enter.

The view goes back to where it was. To open a sketch again, double-click it in the browser or
in the 3D view.

### Drawing

| Tool | Key | How |
|---|---|---|
| Line | L | Click where it starts, then each next point. Esc ends the line; clicking the first point again closes the outline. |
| Rectangle | R | Click one corner, then the opposite one. |
| Circle | C | Click the center, then a point on the circle. |
| Arc | A | Click the center, then where it starts, then move round the center and click where it ends. |
| Point | | Click where it goes. |
| Polygon | | Click the center, then a corner. The number of sides is set in the panel on the right. |
| Slot | | Click the two centers, then move away from the line between them and click to set the width. |

While drawing, the pointer catches on what is already there: ends, middles of lines, and curves.
What it catches on, the new geometry stays attached to. A line that is nearly level or upright
becomes exactly so. **Pick up constraints while drawing**, in the panel, switches all of that
off.

**Typing a size.** While a line, a rectangle, or a circle is being drawn, type a number and
press Enter: the length of the line (in the direction the pointer gives), the width and height
of the rectangle (`40 25`), or the diameter of the circle. The size becomes a dimension.

**Construction geometry** guides a sketch without being part of its outline, and is drawn
dashed. Press X with curves selected to turn them into construction geometry or back. With
nothing selected, X switches what the drawing tools make.

### Selecting and moving

Press Esc until no tool is active. Then:

- Click a curve or a point to select it. Shift + click or Ctrl + click adds or removes. Drag
  across empty space to select what lies wholly inside the rectangle.
- Drag a point or a curve to move it, as far as its constraints allow. Drag a circle by its rim
  to change its size.
- Delete removes what is selected.

### Constraints

A constraint is a rule the geometry keeps: these lines are parallel, this point is on that
circle. Select what it is about, then click the constraint in the toolbar. Or click the
constraint first and then what it is about.

| Constraint | Takes |
|---|---|
| Coincident | Two points; or a point and the line or circle it should lie on |
| Horizontal, Vertical | A line (or several); or two points |
| Parallel, Perpendicular, Collinear | Two lines |
| Tangent | A line and a circle or arc; or two circles or arcs |
| Equal | Two lines (same length); or two circles or arcs (same radius) |
| Concentric | Two circles or arcs |
| Midpoint | A point and a line |
| Symmetric | Two points and the line between them |
| Fix | Points, or curves, which stay where they are |

Each constraint shows as a small marker by its geometry. Click a marker to select the
constraint, and press Delete to remove it. The markers of joined ends appear when the pointer is
on the point. The panel on the right lists every constraint on what is selected, each with a
button that removes it.

LCAD does not add a constraint that the others already imply, or one that contradicts them. It
says which, in the status bar.

### Dimensions

Press D. Click what to measure, then click empty space, and type the value.

| Click | Gives |
|---|---|
| A line | Its length |
| A circle | Its diameter |
| An arc | Its radius |
| Two points | The distance between them |
| A point and a line | The distance from the point to the line |
| Two lines | The angle between them, or the distance if they are parallel |

For a slanted line or two points, where the value is put decides what is measured: above or
below gives the horizontal distance, beside gives the vertical one, and anywhere else the
distance straight across.

To change a dimension, double-click its value and type another. If the sketch cannot take the
value, it stays as it was, and what was typed stays in the field to be corrected. A value is a
number, with `mm` after it if you like; angles are in degrees.

### Is it fully constrained?

Geometry that can still move is drawn in blue. Geometry that cannot is drawn in the color of
text: near black in the light theme, near white in the dark one. The panel and the status bar
say how many degrees of freedom are left, and "Fully constrained" when there are none. A closed
outline is tinted, which is how to tell an outline that is closed from one that only looks it.

The usual way to get there: fix the sketch to the origin (start drawing on it, or make a point
coincident with it), then add dimensions until nothing is blue.

### Changing curves

| Tool | Key | How |
|---|---|---|
| Trim | T | Click the part of a curve to remove. It goes up to where other curves cross it. |
| Extend | | Click a line or an arc near an end. It grows to the next curve it meets. |
| Offset | O | Click a curve, which takes the whole outline it belongs to. Then click where the copy should go. |
| Mirror | | Select what to mirror, choose Mirror, and click the line to mirror in. |
| Project | P | Click an edge or a corner of a body. It comes into the sketch as reference geometry, in its own color, and does not move. |

Not there yet: ellipses and splines; tangent arcs in a chain of lines; patterns; moving a
dimension's value somewhere else; expressions as values. Curves that cross are not split where
they cross, so an outline is closed only where curves meet at their ends: trim first. A
projected edge is a copy, and does not follow its body if the body changes.

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
| Select a sketch | Click one of its curves, or click it in the browser. |
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
| Delete | Delete what is selected |
| F2 | Rename the selected body or sketch |
| Esc | Leave the command in progress; in a sketch, step back from the tool in use |
| Ctrl + Enter | Finish the sketch |
| L, R, C, A | In a sketch: line, rectangle, circle, arc |
| D, T, O, P, X | In a sketch: dimension, trim, offset, project, construction |
| F6, Shift + F6, Home | Fit the view, zoom to the selection, go to the home view |
| F11 | Give the window to the 3D view, and back |
| Ctrl + Q | Quit |

Not there yet: autosave and crash recovery, inches as a display unit, and import or export of
other formats. The origin guides that are shown are not saved with the document.

## If something goes wrong

LCAD keeps a log at `~/.local/state/lcad/lcad.log` (or under `$XDG_STATE_HOME` if that is set).
Help > About shows the path, along with the versions of LCAD, Qt, and Open CASCADE and the
graphics driver in use. Include both when reporting a problem.
