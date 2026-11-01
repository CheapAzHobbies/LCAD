# Using LCAD

What LCAD can do today, and how to do it. This grows with each phase; the
[roadmap](../ROADMAP.md) says what comes next.

LCAD sketches, pulls and revolves sketches into solids, rounds, bevels, hollows, patterns, and
drills them, keeps the steps in a timeline that can be edited, changes imported solids directly,
reads and writes other programs' files, and puts parts together into assemblies: components,
joints between them, linked parts from other documents, and their weight and overlaps. From a
model it makes engineering drawings, and toolpaths and G-code for a milling machine.

The box at the top chooses what the window is set up for: **Design**, **Drawing**, or
**Manufacture**. The toolbar, the menus, and the panel on the right change with it, and the model
stays as it is.

## The window

| Region | What it is for |
|---|---|
| Top bar | Menus; buttons for new, open, save, undo, and redo; a tab for each open document; command search. |
| Toolbar | The modeling tools. **Create** starts a sketch, pulls and revolves it, or makes a primitive. **Modify** changes solids. **Assemble** makes components and joins them. **Select** sets what a click picks. **View** fits the view, shows the grid, and sets how bodies are drawn. While a sketch is open, the sketch tools are here instead. |
| Browser (left) | The document as a tree. Tick a body or a sketch to show it, untick to hide it, right-click for more. Double-click a body to rename it, or a sketch to open it. The box above the tree filters by name. Under **Origin** are the reference planes and axes, which can be ticked to show them as guides. After the document's own bodies and sketches come its components, each with its own, and its joints. |
| 3D view | The model. |
| Properties (right) | While a command runs, its settings. While a sketch is open, the sketch: whether it is fully constrained, and the constraints on what is selected. Otherwise, what is selected: a body's name, visibility, and measurements, or the area, length, or position of a face, edge, or vertex. |
| Timeline (bottom) | The sketches and features in the order they are applied. |
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

## Making a solid from a sketch

This is how most parts start: sketch an outline, then **Pull** it into a solid (what other
programs call extrude).

1. Draw a closed outline in a sketch. You do not have to finish the sketch first.
2. Press **E**, or click **Pull** under **Create**. If the sketch has one closed area, it is
   picked already; otherwise click the areas you want in the 3D view (they light up under the
   pointer; a second click lets one go).
3. Type the distance on the right, or drag the arrow in the 3D view up or down, or type the
   distance into the box beside the arrow and press Enter. A negative distance, or **Flip
   Direction**, pulls the other way. **Direction** pulls to one side, by the same amount to both sides (**Symmetric**), by
   different amounts (**Two Sides**), or up to a flat face you click in the 3D view (**To
   Face**), which it then follows. **Taper** leans the sides in as the pull goes out (negative
   leans them out); it works on a pull to one side.
4. **Operation**: a **New Body**, **Join** it to a body, **Cut** it out of one, or keep only
   what it shares with one (**Intersect**). Until you choose, LCAD guesses: a pull that lies
   mostly inside a body cuts it, one that touches a body joins it, one on its own is a new body.
5. Press Enter or click **OK**. The sketch is hidden; show it again from the browser.

The 3D view shows the result as you type. If it cannot be made, the panel says why and OK waits.

Parts that do not touch are separate bodies. Pull two outlines that lie apart and you get a body
for each, both from the one Pull, so changing its distance changes both. A cut right across a
body leaves two bodies the same way, and so do pattern or mirror copies that do not touch the
original. Delete one body and the others stay; delete the Pull from the timeline and all of them
go.

**Revolve** turns closed areas about a line of the same sketch (draw that line as construction
geometry if it is not part of the outline), an origin axis, or a construction axis: choose it
under **Axis** in the panel. **Mirror**, **Rectangular Pattern**, and **Circular
Pattern** (under **Create**, and the pattern button in the toolbar) copy pulls and revolves, or
a whole body: select the feature in the timeline first, or check it in the panel. The plane,
direction, or axis is one through the origin, a construction plane or axis, or an edge or face
you click in the 3D view.

## Holes

Draw points where the holes go, in a sketch on the face to drill into. Then press **H**, or click
**Hole**: the panel takes the points of the sketch that was open, or click another sketch in the
3D view. Choose **Simple**, **Counterbore**, or **Countersink**, the diameter, and a depth (with
the point a drill leaves) or **Through all**. **Flip Direction** drills the other way, for a
sketch that is not on the face itself.

## Changing a solid

Under **Modify**: **Fillet** (F) rounds edges, **Chamfer** bevels them, **Shell** hollows a body
out, open at the faces you click. Click the edges or faces in the 3D view, before or after
starting the command; Shift-click picks more.

## Changing faces directly

These work on any body, whatever made it, and are the way to change a body imported from a
STEP or IGES file, which has no sketches or features behind it. Each is a step in the timeline
like any other: edit it, suppress it, or move it.

- **Offset Face** (Q) moves the faces you click along their normals by a distance, and the faces
  around them follow. A positive distance adds material (a plate gets thicker); a negative one
  takes it away. On the wall of a hole, a negative distance makes the hole wider.
- **Move Face** moves the faces you click in a direction (along X, Y, or Z, along a construction
  axis, or square to the first face). Click the wall of a hole, or the top and side of a boss, to
  move it across the face it is in. A flat face only moves along its own normal: moving it within
  its plane changes nothing.
- **Delete Face** takes the faces you click out, and the faces around them grow to close the gap:
  a fillet, a hole, or a boss goes as if it had never been made. Where the faces around cannot
  close the gap (the top of a box), it says so.
- **Repair Body** sews loose faces together, turns faces to face outward, and fixes edges and
  tolerances, where that can be done.

## Importing and exporting

**File > Import** (Ctrl + I) adds the solids of a STEP (.step, .stp) or IGES (.igs, .iges) file to
the document as bodies, named as the parts are in the file, in millimeters whatever unit the file
uses. Faces in the file that are not part of a solid are sewn together, and those that close up
become solids; the rest are kept as surfaces, and LCAD says so. Importing is one step to undo.
An imported body has no timeline of its own: change it with the commands above.

**File > Export** (Ctrl + Shift + E) writes the bodies shown, or those selected, to:

- **STEP**: the exact solids, each a part with its name, for other CAD programs. Not the timeline:
  they get the shapes, not how they were made.
- **STL**, **3MF**, or **OBJ**: triangles, for 3D printing and other tools. Choose how closely the
  triangles follow curved surfaces (Coarse, Medium, Fine, or your own deviation and angle). STL
  can be written as text; 3MF keeps each body's name and the unit. A mesh is an approximation of
  the solid; the LCAD document itself stays as it is.

LCAD remembers the folder and the settings you used last.

## Checking geometry

**View > Check Geometry** looks at the bodies selected, or at all of them, and lists what is wrong
with each: edges that border only one face (it is not closed), faces that face inward, faces that
cross each other, broken outlines, edges too short to work with. Click a problem to see where it
is in the 3D view. **Repair Body** in that list adds a Repair of the body to the timeline.

## Components and assemblies

A **component** is a part, or a group of parts, with bodies, sketches, and features of its own.
The document itself is the top of the assembly; components go inside it, and inside each other.

- **Assemble > New Component** makes an empty one and starts working on it: new sketches and
  bodies go into it, and its name is in bold in the browser. Double-click another component in
  the browser to work on that one, or the document's own entry at the top to work on the whole
  again. A sketch drawn on a face belongs to that face's component, whichever one is being
  worked on.
- **Assemble > Component from Bodies** makes a component of the bodies selected, together with
  the features that made them and the sketches those use. If a sketch is also used by a body
  that is not selected, LCAD says so: select that body too.
- A component can appear more than once. **Duplicate** (right-click it in the browser) adds
  another appearance of the same component: change one and every one changes. **Make
  Independent Copy** makes a new component that starts the same and is changed on its own.
- **Move Component** moves or turns a component by typed amounts. **Ground** holds one where it
  is, so that joints move the other side instead. **Suppress Component** leaves one out of the
  assembly without deleting it.
- Hide, show, isolate, rename (F2), and delete components as you do bodies. Drag a component
  onto another in the browser to move it inside that one, or onto the document's entry to move
  it back to the top.

### Joints

A joint holds two parts together where you say, and lets one move against the other in the
ways its kind allows.

1. **Assemble > Joint** (J).
2. Click a face or an edge of the part that is to move, then a face or an edge of the part it
   goes onto. A flat face joins at its middle, a round face or a circular edge at its axis, a
   straight edge along its length (**At** says where along it). The part moves there in the
   preview, face to face.
3. **Type** is what LCAD suggests from what you clicked; choose another to change it:
   **Rigid** (does not move), **Revolute** (turns), **Slider** (slides), **Cylindrical** (turns
   and slides), **Pin-Slot** (turns, and slides across), **Planar** (slides in a plane and turns
   in it), **Ball** (turns any way).
4. **Offset** stands it off along the joint's axis, **Angle** turns it, **Flip** turns it over.
   Below them, how far the joint has moved now, and with **Limits**, how far it may move.
   **Animate** shows it moving through its range.
5. Click **OK**. The joint is listed under **Joints** in the browser; double-click it to change
   it, untick it to suppress it, right-click it to animate it.

Joints start from the part that is grounded (or from the document's own bodies) and place the
others one after another. A joint that cannot hold together with the others, because both
parts are already placed, says so in the browser and in its properties, and leaves its part
where it was.

### Linked parts

**Assemble > Insert Part** puts another LCAD document into this one as a **linked part**: it is
changed in its own file, and this document follows. Inserting the same file again adds another
appearance of it.

- **File > Linked Files** lists every file the document links to, and the files those link to,
  with whether each is found and whether it changed since it was last read. **Update All** (or
  **File > Update Linked Parts**) takes the changes. When a document is opened, LCAD says if a
  linked part changed or cannot be found.
- A copy of every linked part is kept in the document, so it still opens and shows the part when
  the file is missing. **Find Linked File** (right-click the part) says where the file is now.
- **Open Linked File** opens the part's own document to change it.
- **File > Package Project** gathers the document and every file it links to into one folder,
  with the links pointing at each other there: the folder can be moved to another computer, or
  shared, as it is.

Links are kept relative to the document's folder, so a project moved together keeps working.

### Weight, materials, and overlaps

- Choose what a body is made of under **Material** in its properties: plastics for printing,
  metals, and woods, with their densities. Its mass is shown below. **View > Materials** lists
  them, and adds materials of the document's own with a density you give.
- **View > Physical Properties** shows the mass, volume, center of mass, and moments of inertia of
  the bodies selected, or of everything shown. Bodies without a material are left out of the
  mass and counted.
- **View > Check Interference** finds where parts that are shown overlap, lists each pair with
  the volume they share, and shows that volume in red in the 3D view. Parts that only touch do
  not count.

## Parameters

**Modify > Parameters…** opens a table of named values. **Add** a row, name it (`width`), and type
what it is (`60 mm`), or work it out from others (`width / 10`). Then type the name, or an
expression with it, in place of a number in any value box of a feature (`thick`, `width / 2 + 3`)
or in a sketch dimension. A dimension worked out from parameters is marked **ƒ**. Change a
parameter and everything that uses it follows; rename it and the new name goes everywhere the old
one was. Expressions take units (mm, cm, m, in, ft for lengths; deg, rad for angles), + - * /
and ^, parentheses, and sqrt, abs, sin, cos, tan, asin, acos, atan, min, max, round, floor, ceil,
and pi.

## Construction planes

**Offset Plane** (under **Create**) adds a plane to sketch on: pick an origin plane in the panel,
or click a flat face in the 3D view, and type how far from it. Planes are listed under
**Construction** in the browser (uncheck to hide, double-click to move), and drawn in amber.
To sketch on one, select it in the browser and click **New Sketch**, or click **New Sketch** and
then the plane in the 3D view.

A sketch drawn on a construction plane, or started on a flat face of a body, follows it: move
the plane or change the body, and the sketch and what was made from it move too.

**Axis** (under **Create**) adds a construction axis: pick an origin axis in the panel, or click
a straight edge, or a round edge or face (the axis of a hole or a rod), in the 3D view. Axes are
listed under **Construction** too and drawn as dashed amber lines. A revolve can turn about one,
and a pattern go along or around one; when the edge or face it runs along moves, the axis and
what uses it follow.

## The timeline

The strip under the 3D view lists the sketches and features in the order they are applied.

- **Click** an entry to select what it made. **Double-click** it, or press Enter, to edit it:
  the model goes back to how it was just before that feature, so that what it works on can be
  picked again, and comes forward again when you click OK or Cancel.
- **Right-click** for **Edit**, **Rename**, **Suppress** (leave it out, and keep it),
  **Roll Back to Here**, **Roll Forward to the End**, and **Delete**.
- **Drag an entry** to move it. LCAD refuses, and says why in the status bar, if that would put
  a feature before something it depends on.
- **Drag the marker** (the dark line with the handle) to the left to see, or work on, an
  earlier state of the model. New features go where the marker is.
- An entry with a **red outline and dot** fails: hover over it, or select it, to read why. The
  rest of the model is built without it.
- **Compact**, on the right, makes the entries smaller.

When you change a sketch, the features made from it follow when you finish the sketch.

## Measuring

Press **M**, or click the measure button under **View**. Click one thing in the 3D view to see
its length, area, radius, or position; click a second to see the distance between them (also
along X, Y, and Z) and, for flat faces and straight edges, the angle between them. A third click
starts again. Esc closes it.

## Sketching

A sketch is flat geometry on a plane: lines, circles, and arcs, held in shape by constraints
and dimensions.

### Starting and finishing

1. Click **New Sketch** in the toolbar (or press S and type "sketch").
2. The three planes through the origin appear in the 3D view: blue for XY, green for XZ, red
   for YZ. Click one. The plane under the pointer lights up, and the status bar names it. Or
   click a flat face of a body. If a flat face was already selected, the sketch goes straight
   onto it. A plane that is seen edge-on is hard to click; turn the view, or use its button in
   the panel on the right.
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
| Ellipse | | Click the center, then the end of the major axis, then how far out it reaches across (its minor radius). The dimension tool gives it its minor radius; its major radius is the distance from the center to the end of the major axis. |
| Spline | | Click the points it goes through first and last, and the ones it is drawn toward in between. Enter, a double-click, or Esc ends it. Drag its points to reshape it. |

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

Press D. The panel on the right says what to click. Click a line and type its length: the
dimension is placed where the pointer is, and Enter sets it. To put it somewhere else first,
move the pointer there, where the dimension follows it, and click. A circle or an arc needs one
click; two points or lines need a click on each.

Or select first, then press D: a line gets its length, a circle its diameter, and the value
opens for typing. With a line selected, the panel says "Press D to give it a length".

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

Not there yet: tangent arcs in a chain of lines; patterns in a sketch; moving a dimension's
value somewhere else; trimming, extending, or offsetting an ellipse or a spline, and
constraining a point onto one. Curves that cross are not split where
they cross, so an outline is closed only where curves meet at their ends: trim first. A
projected edge is a copy, and does not follow its body if the body changes.

## Drawings

Choose **Drawing** in the box at the top. The first time, LCAD starts a sheet with views of the
model: the front, the top, the right, and an isometric view, at a scale that fits. The panel on
the right has the sheet's size, the standard, and the title block. ISO puts the top view below the
front view (first-angle), and ANSI above it (third-angle). The views show everything that is shown
in the model.

- **Move a view** by dragging it. Select it to change its scale, or to leave out hidden lines or
  center marks.
- **Add a view** from the View button.
- **A section:** choose Section, Up or Section, Across, and click the view where the cut goes. The
  section goes beside it, and the cutting line onto it. "Look from the other side" in the panel
  turns it around.
- **A detail:** choose Detail and click the middle of what to show larger.
- **A dimension:** press D, or choose Dimension, and click a line for its length, or a circle for
  its diameter; an arc gives its radius, and Shift gives the other. To measure between two points,
  click near each. Select a dimension to add a tolerance after its value, or to set how far it stands
  off.
- **A note:** choose Note, click, type, and press Enter.
- Delete removes what is selected. A view goes with the sections and details taken from it.

Views are worked out from the model each time: change the model, and the sheet follows. A
dimension hangs on an edge by the name the edge keeps, so it follows too. If the model no longer
has that edge, the dimension is not shown, the panel says so, and it comes back if the edge does.

**Export PDF** writes the sheet at its true size, and **Export DXF** writes it for other CAD
programs and cutters, with each kind of line on a layer of its own.

Not there yet: auxiliary views, GD&T, hole callouts and balloons, a bill of materials, drawing
templates, dragging a dimension, more than one sheet in a drawing (use more drawings), and views of
some bodies only (hide the others).

## Manufacture

Choose **Manufacture** in the box at the top. The first time, LCAD starts a setup: the block the part
is cut from, the machine, and four tools to start from. The tool comes straight down along the
model's Z, so turn the model so that what is to be cut faces up.

1. Check the panel: **Stock** is how much is around, above, and below the part, **Zero at** is where
   zero is on the machine, and **Machine** is what it can do and which G-code it takes. Under
   **Tools**, change the four to match your cutters, or click **Add Tool** for another: its kind,
   diameter, how deep its **Flutes** cut, how far it sticks out of the holder, and its number. Every
   check goes by these, so measure them. A tool an operation uses cannot be deleted.
2. Add operations in the order they run. Each works with what is selected in the 3D view:
   - **Face** levels the top of the stock down to the top of the part.
   - **Pocket** clears down to the flat floor face selected, around any islands in it.
   - **Contour** cuts around the outline of the flat face selected, down to it: select the part's
     bottom face to cut it out.
   - **Drill** drills the round faces selected. A tool has to match the hole.
3. Select an operation in the list to set its tool, feeds, and depths. Use Selected Faces gives it
   other faces. The toolpaths in the 3D view change at once: cuts in blue, rapid moves dotted in red.
   What cannot be done is said at the top of the panel, and the path is left out.
   - **Ramp** is how steeply a pocket or contour goes down into the material, along its path and
     at the plunge feed. The default is 3 degrees; 0 goes straight down. A pocket with no long
     enough ring in the middle spirals down on a small circle. Either way the tool cuts with its
     end, so use an end mill that can: one that is center-cutting.
   - **Tabs** are for a contour that cuts right through, which otherwise leaves the part loose
     against a spinning cutter, and LCAD warns of that. Set how many, how wide, and how high. They
     go evenly around, on straight stretches where they fit. Cut them by hand afterward.
4. **Simulate** runs it all on the stock and shows what is left, with the time it takes. Drag the
   slider to any move. It looks for cuts into the part, rapid moves into the material, and tools
   buried past their flutes.
5. **Write G-code** writes the file. It runs every check again first, and reads back what it wrote as
   the machine would. It refuses if anything fails, and says why.

G-code is for Grbl or LinuxCNC. Grbl has no tool changer, so between tools the program stops for you
to change the tool and set Z zero. Zero in Z is the top of the stock. Level curves are written as
arcs, which makes the file shorter and the motion smoother. Turn off **Write curves as arcs** under
**Machine** for a controller that does not take them.

**Be careful with this.** LCAD's G-code has been checked against its own simulation and its own
reader of each dialect, and has not yet been run on a real machine. The checks know about the
tools, the stock, and the part. They do not know about your clamps, your vise, or feeds that suit your
material, which are yours to set: the defaults are a starting point, not advice. Run a new program
in the air first, with the spindle off and the tool well above the work, and keep a hand near the
stop button.

Not there yet: 3-axis surfacing, adaptive clearing, chamfering, holders checked by their real shape,
and tool libraries shared between documents.

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
| Look from the front, top, and so on | Click a side, edge, or corner of the view cube, or use the View menu. The cube's sides are three different shades and its edges are dark, so that it shows which way the model is turned at a glance. |

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

Every minute LCAD keeps a copy of each document with unsaved changes, apart from your files, in
the `recovery` folder of LCAD's state folder (see [If something goes wrong](#if-something-goes-wrong)
for where that is). If LCAD did not close properly (a crash, a power cut), it
offers to restore that work the next time it starts: each document opens in a tab with its
changes not yet saved, and saving it writes to its own file as usual. The copies go away when
the work is saved, or closed without its changes.

To open a document from a terminal or a file manager, give its name to LCAD: `lcad part.lcad`.
On Windows and macOS, double-clicking a `.lcad` document opens it in LCAD; on Windows, when the
installer was left to make LCAD what opens them.

## The window itself

- **Window > Theme** chooses a light or a dark interface, or follows the desktop.
- Panels can be dragged to another side, resized, or hidden from the **Window** menu. LCAD
  remembers the arrangement. **Window > Reset Layout** puts everything back.
- **Window > Focus on 3D View** (F11) hides the panels and the toolbar; the same command brings
  them back.
- **Window > Keyboard Shortcuts** lists every command with its shortcut and lets you change it.

## Keyboard

These are the shortcuts LCAD starts with. On macOS, Ctrl in them is the Command key (⌘).

| Key | Action |
|---|---|
| S, or Ctrl + K | Search commands |
| Ctrl + N, Ctrl + O, Ctrl + W | New document, open a document, close the document |
| Ctrl + S, Ctrl + Shift + S | Save, save under another name |
| Ctrl + Z | Undo |
| Ctrl + Shift + Z, or Ctrl + Y | Redo |
| Delete | Delete what is selected |
| F2 | Rename the selected body, sketch, component, or joint |
| Esc | Leave the command in progress; in a sketch, step back from the tool in use |
| Ctrl + Enter | Finish the sketch |
| L, R, C, A | In a sketch: line, rectangle, circle, arc |
| D, T, O, P, X | In a sketch: dimension, trim, offset, project, construction. D also dimensions on a drawing's sheet |
| E | Pull (also from inside a sketch, which it finishes) |
| F | Fillet |
| H | Hole |
| Q | Offset Face |
| J | Joint |
| M | Measure |
| Ctrl + I, Ctrl + Shift + E | Import a STEP or IGES file, export bodies |
| F6, Shift + F6, Home | Fit the view, zoom to the selection, go to the home view |
| F11 | Give the window to the 3D view, and back |
| Ctrl + Q | Quit |

Not there yet: inches as a display unit, importing meshes (STL, OBJ, 3MF) or exporting IGES,
pulling up to a curved face, dragging a component or a joint in the 3D view (they are moved by
typed values), joints that close a loop (each part is placed by one joint at a time), exploded
views, and a bill of materials. Construction axes are not picked by clicking them in the
3D view: choose them from the list in the panel.
The origin guides that are shown are not saved with the document.

## If something goes wrong

LCAD keeps a log, `lcad.log`, in its state folder:

| System | State folder |
|---|---|
| Linux | `~/.local/state/lcad/` (or `lcad` under `$XDG_STATE_HOME`, if that is set) |
| Linux, the Flatpak | `~/.var/app/io.github.cheapazhobbies.LCAD/.local/state/lcad/` |
| Windows | `%LOCALAPPDATA%\LCAD\` |
| macOS | `~/Library/Application Support/LCAD/` |

Help > About shows the path, along with the versions of LCAD, Qt, and Open CASCADE and the
graphics driver in use. Include both when reporting a problem.
