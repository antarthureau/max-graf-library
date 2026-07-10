/**
 * @file graf.affiche.c
 * graf.affiche — real-time graph visualizer for the graf external family.
 * antoine hureau-parreira
 *
 * A jbox UI external: draws a named [graf] instance as a directed graph
 * inside a resizable patcher window. Nodes are circles, edges are directed
 * arrows, weights are labels. The current traversal position is highlighted.
 *
 * Architecture: node positions live in a fixed-scale WORLD coordinate space,
 * stored in a heap-allocated array owned by this object (no node-count cap).
 * Zoom and pan are a separate view transform applied at paint time
 * (screen = world * zoom + pan) — they never modify stored positions.
 * World positions are recomputed only when the layout mode changes or the
 * watched graph reports "modified", never on a plain repaint.
 * 
 * TODO: add visited node/edge colors and last-visited node color to the UI, make them attributes
 * TODO: allow parameters from inspector window for the layout such as colors, zoom, etc
 *
 * Layout modes (message: mode <name>):
 *   circle    — ring, fixed arc length per node (radius grows with n). Default.
 *   line      — single row, fixed spacing, extends rightward.
 *   random    — position is a pure hash of the node id (stable per id and
 *               across sessions); `redraw` draws a new seed.
 *   grid      — fixed cell size, fixed column count, extends by adding rows.
 *               Each node keeps its cell for life; removals leave holes,
 *               `redraw` defragments.
 *   comb      — grid with odd rows offset half a cell (same cell stability).
 *   treeup    — vertical tree/forest, roots at top, children grow downward.
 *   treedown  — roots at bottom, children grow upward.
 *   treeleft  — horizontal, roots at left, children grow rightward.
 *   treeright — roots at right, children grow leftward.
 *   rings     — concentric circles by BFS depth from the roots; disconnected
 *               components form separate ring clusters on a coarse grid.
 *
 * Messages:
 *   bang              — force immediate repaint (no layout recompute)
 *   update <name>     — switch to a different named [graf] instance
 *   mode <name>       — switch layout mode (see list above)
 *   zoom <in|out>     — zoom by a fixed factor about the viewport center
 *   move <left|right|up|down> — pan the viewport by a fixed pixel step
 *   reset             — recompute zoom+pan so the whole graph fits the box
 *   center            — pan only (keep zoom) so graf's current node is centered
 *   redraw            — force a full from-scratch layout recompute
 *                       (defragments grid/comb, reseeds random)
 *
 * Arguments:
 *   [graf.affiche my_graph]  — watches the instance named "my_graph"
 *
 * Java analogy: this is like a JPanel subclass that registers as an Observer
 * on a model object and calls repaint() in its update() method.
 * jbox is to t_object what JPanel is to Object — a UI component with a paint()
 * callback. The struct must have t_jbox as its first member, exactly like every
 * Max object struct starts with t_object ob.
 */

#include "ext.h"
#include "ext_obex.h"
#include "jpatcher_api.h"
#include "jgraphics.h"
#include "../graf.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif


////////////////////////// defines — visual geometry (screen px at zoom 1.0)

#define GAFF_NODE_RADIUS    18.0    // pixel radius of each node circle
#define GAFF_ARROW_LEN      10.0    // arrowhead arm length in pixels
#define GAFF_ARROW_ANGLE    0.42    // arrowhead opening half-angle in radians (~24°)
#define GAFF_WEIGHT_OFFSET  13.0    // perpendicular offset for edge weight labels
#define GAFF_LOOP_RADIUS    9.0     // radius of self-loop circle

// Default box size (pixels) */
#define GAFF_DEFAULT_WIDTH  400
#define GAFF_DEFAULT_HEIGHT 300


////////////////////////// defines — world-space layout constants

// World units are an arbitrary fixed scale: 1 world unit == 1 screen pixel at
   /* zoom 1.0. All layout spacing is defined here, in world units, and NEVER
   depends on the box size — `reset` fits the view to the content instead. */

#define GAFF_ARC_LENGTH     90.0    // circle: arc length allotted per node
#define GAFF_LINE_SPACING   90.0    // line: horizontal pitch per node
#define GAFF_CELL           90.0    // grid/comb: square cell size
#define GAFF_GRID_COLS      8       // grid/comb: fixed column count (rows grow)
#define GAFF_RANDOM_EXTENT  800.0   // random: side of the square ids hash into
#define GAFF_TREE_PITCH     70.0    // tree: sibling-axis distance per width unit
#define GAFF_TREE_LEVEL     100.0   // tree: depth-axis distance per level
#define GAFF_TREE_GAP       1.0     // tree: extra gap between trees, in width units
#define GAFF_RING_STEP      90.0    // rings: radius increment per BFS depth
#define GAFF_RING_CLUSTER_MARGIN 90.0 // rings: clearance between component clusters
                                         /* (added to 2 * largest cluster radius to get
                                         the arrangement grid's cell size) */

// Edge curving: a non-adjacent edge bows LEFT of its direction of travel by
   /* GAFF_CURVE_STEP world units per skipped node, capped at a fraction of the
   edge length. One consistent side means reciprocal edges A->B / B->A bow to
   opposite sides and never overlap. */
#define GAFF_CURVE_STEP     14.0
#define GAFF_CURVE_MAX_FRAC 0.4

// Zoom / pan */
#define GAFF_ZOOM_STEP      1.25    // multiplier per `zoom in` (divide for out) */
#define GAFF_ZOOM_MIN       0.1
#define GAFF_ZOOM_MAX       10.0
#define GAFF_PAN_STEP       50.0    // screen px per `move` — constant feel at any zoom */
#define GAFF_FIT_MARGIN     0.9     // `reset` fills 90% of the box */
#define GAFF_FIT_MIN_SPAN   500.0   // world units — autofit bbox floor: keeps 1-3 node
                                       /* graphs near 1:1 instead of over-magnifying;
                                       tune against a real patch */

/* Label legibility floors: below these the text would be unreadable anyway,
   so skip it — declutters heavily zoomed-out views of large graphs. */
#define GAFF_MIN_LABEL_RADIUS 5.0   // skip node labels when screen radius < this */
#define GAFF_MIN_WEIGHT_ZOOM  0.5   // skip weight labels when zoom < this */


////////////////////////// defines — layout modes

enum {
    GAFF_MODE_CIRCLE = 0,
    GAFF_MODE_LINE,
    GAFF_MODE_RANDOM,
    GAFF_MODE_GRID,
    GAFF_MODE_COMB,
    GAFF_MODE_TREEUP,
    GAFF_MODE_TREEDOWN,
    GAFF_MODE_TREELEFT,
    GAFF_MODE_TREERIGHT,
    GAFF_MODE_RINGS
};


////////////////////////// colors (dark theme, RGBA in [0..1])

// Static globals: initialized once, referenced by pointer in jgraphics calls.
   /*In C, static globals at file scope are zero-initialized, then assigned here.
   Java analogy: private static final Color fields. */

static t_jrgba GAFF_COLOR_BG           = {0.12, 0.12, 0.14, 1.0};  // near-black background
static t_jrgba GAFF_COLOR_NODE         = {0.24, 0.27, 0.32, 1.0};  // default node fill
static t_jrgba GAFF_COLOR_NODE_CURRENT = {0.18, 0.60, 0.38, 1.0};  // current node: green
static t_jrgba GAFF_COLOR_NODE_BORDER  = {0.48, 0.52, 0.60, 1.0};  // node outline
static t_jrgba GAFF_COLOR_EDGE         = {0.48, 0.52, 0.58, 1.0};  // edge lines and arrows
static t_jrgba GAFF_COLOR_TEXT_NODE    = {0.92, 0.92, 0.94, 1.0};  // node ID label
static t_jrgba GAFF_COLOR_TEXT_WEIGHT  = {0.82, 0.78, 0.38, 1.0};  // weight label: warm yellow
static t_jrgba GAFF_COLOR_PLACEHOLDER  = {0.34, 0.34, 0.38, 1.0};  // "no graph" message

//TODO: add visited node/edge colors and last-visited node color to the UI, make them attributes
//These needs to be tied to new edits in graf.c and graf.h because last visited and next visited etc are not tracked for now.
 static t_jrgba GAFF_COLLOR_VISITED_NODES = {0.18, 0.60, 0.38, 0.3};  // visited nodes: translucent green
 static t_jrgba GAFF_COLOR_VISITED_ED_EDGES = {0.82, 0.78, 0.38, 0.3};  // visited edges: half-yellow
 static t_jrgba GAFF_COLOR_LAST_VISITED_NODE = {0.18, 0.60, 0.38, 0.6};  // last visited node: half-green
 static t_jrgba GAFF_COLOR_LAST_VISITED_EDGE = {0.82, 0.78, 0.38, 0.3};  // last visited edge: half-yellow

// could also add most probable next node/edge based on navigation algo or weight. 
// could also add a day/night mode or even better some inspector-window layout attributes such as background color, node color, edge color
//and derive visited color from them (half values for grey channel).

////////////////////////// data structures

/**
 * One stored node position in world space.
 *
 * The array of these is rebuilt on every layout sync so that entry k always
 * mirrors graph->nodes[k] — paint can index it directly, no lookup.
 * Stability across syncs (where the mode promises it) is carried by the
 * COORDINATES and the grid cell, which are looked up by id from the previous
 * array before it is discarded.
 *
 * Java analogy: the whole array is a Map<NodeId, Point2D> flattened into the
 * graphs own node order for O(1) access during painting.
 */
typedef struct _gaff_pos {
    t_symbol   *id;         // node id this entry belongs to
    double      wx, wy;     // world coordinates
    long        cell;       // grid/comb: persistent cell index; -1 elsewhere
    long        parent;     // tree/rings: BFS parent as a node index; -1 = root
    long        depth;      // tree/rings: BFS depth from the root
} t_gaff_pos;

/**
 * graf.affiche object struct.
 *
 * IMPORTANT: t_jbox MUST be the first member. This is the jbox equivalent of
 * the t_object ob convention — Max's type system treats the first member as
 * the object header and casts between t_jbox* and t_graf_affiche* freely.
 *
 * Java analogy: extends JPanel { ... }
 */
typedef struct _graf_affiche {
    t_jbox      box;            // MUST be first — jbox subclass header
    t_symbol   *graf_name;      // name of the [graf] instance we are watching

    long        mode;           // GAFF_MODE_* — current layout

    t_gaff_pos *pos;            // heap array of world positions (sysmem)
    long        pos_count;      // entries currently valid
    long        pos_capacity;   // allocated entries (doubling growth)

    double      view_zoom;      // view transform: screen = world*zoom + pan + boxcenter
    double      view_pan_x;
    double      view_pan_y;

    unsigned long long rand_seed; // instance seed for random mode; `redraw` advances it

    char        layout_dirty;   // graph changed — sync positions before next paint
    char        layout_full;    // discard per-id stability, relayout from scratch
    char        needs_autofit;  // run the `reset` fit at next paint (box size known there)
} t_graf_affiche;


////////////////////////// function prototypes

void *graf_affiche_new(t_symbol *s, long argc, t_atom *argv);
void  graf_affiche_free(t_graf_affiche *x);
void  graf_affiche_assist(t_graf_affiche *x, void *b, long m, long a, char *s);

// Paint is the draw callback — registered as "paint" A_CANT.
//A_CANT means "called by Max internally, not from a patcher" — like @Override.
void  graf_affiche_paint(t_graf_affiche *x, t_object *patcherview);

// notify receives events from subscribed objects (our watched graf instance)
// and from jbox itself (attribute changes, resize, etc.).
t_max_err graf_affiche_notify(t_graf_affiche *x, t_symbol *s, t_symbol *msg,
                               void *sender, void *data);

void  graf_affiche_bang(t_graf_affiche *x);
void  graf_affiche_update(t_graf_affiche *x, t_symbol *name);
void  graf_affiche_mode(t_graf_affiche *x, t_symbol *name);
void  graf_affiche_zoom(t_graf_affiche *x, t_symbol *dir);
void  graf_affiche_move(t_graf_affiche *x, t_symbol *dir);
void  graf_affiche_reset(t_graf_affiche *x);
void  graf_affiche_center(t_graf_affiche *x);
void  graf_affiche_redraw(t_graf_affiche *x);

// NOTE: future mouse support (node dragging) hooks in here as jbox "mousedown"/"mousedrag"
// A_CANT methods doing inverse-transform hit-testing — deferred.

// Layout engine — static so they are private to this translation unit */
static void gaff_layout_sync(t_graf_affiche *x, t_graf *graph);
static long gaff_tree_bfs(t_graf *graph, long *order, long *parent, long *depth,
                           long *component);
static long gaff_edge_skip(t_graf_affiche *x, long i, long ti, long n);
static void gaff_autofit(t_graf_affiche *x, double view_w, double view_h);
static long gaff_index_of(t_graf *graph, t_symbol *id);
static const char *gaff_mode_name(long mode);

// Drawing helpers */
static void gaff_draw_arrow(t_jgraphics *g, double x1, double y1,
                             double x2, double y2, double node_r, double zoom);
static void gaff_draw_curve(t_jgraphics *g, double x1, double y1,
                             double x2, double y2, double bow,
                             double node_r, double zoom,
                             double *label_x, double *label_y);
static void gaff_draw_selfloop(t_jgraphics *g, double cx, double cy, double zoom);
static void gaff_draw_node_label(t_jgraphics *g, const char *text,
                                  double cx, double cy, double r, double zoom,
                                  t_jrgba *color);
static void gaff_draw_weight_label(t_jgraphics *g, double mx, double my,
                                    double weight, double zoom);
static void gaff_draw_placeholder(t_jgraphics *g, t_rect *rect,
                                   const char *line1, const char *line2);

// Global class pointer */
void *graf_affiche_class;


////////////////////////// class registration

/**
 * ext_main — called once when Max loads the external.
 *
 * jbox subclass setup differs from plain t_object in three ways:
 *   1. c->c_flags |= CLASS_FLAG_NEWDICTIONARY — jbox constructors receive
 *      a t_dictionary encoding the patcher box state (position, size, etc.)
 *   2. jbox_initclass(c, flags) — adds standard jbox attributes (patching_rect etc.)
 *   3. CLASS_ATTR_DEFAULT for "patching_rect" — sets the initial box size
 *
 * Java analogy: this is the static initializer that registers the class with
 * Max's runtime type system, like Class.forName() but in reverse.
 */
void ext_main(void *r)
{
    t_class *c;

    c = class_new("graf.affiche",
                  (method)graf_affiche_new,
                  (method)graf_affiche_free,
                  sizeof(t_graf_affiche),
                  0L, A_GIMME, 0);

    // Required for jbox — enables the dictionary-based constructor protocol
    c->c_flags |= CLASS_FLAG_NEWDICTIONARY;

    /*Register as a jbox subclass. 0 = no extra attributes (we handle colors
       ourselves; use JBOX_COLOR here later if you want inspector color pickers)*/
    jbox_initclass(c, 0);

    // Internal callbacks — A_CANT means Max calls these, not the user 
    class_addmethod(c, (method)graf_affiche_paint,  "paint",  A_CANT, 0);
    class_addmethod(c, (method)graf_affiche_notify, "notify", A_CANT, 0);
    class_addmethod(c, (method)graf_affiche_assist, "assist", A_CANT, 0);

    // User-facing messages
    class_addmethod(c, (method)graf_affiche_bang,   "bang",   0);
    class_addmethod(c, (method)graf_affiche_update, "update", A_SYM, 0);
    class_addmethod(c, (method)graf_affiche_mode,   "mode",   A_SYM, 0);
    class_addmethod(c, (method)graf_affiche_zoom,   "zoom",   A_SYM, 0);
    class_addmethod(c, (method)graf_affiche_move,   "move",   A_SYM, 0);
    class_addmethod(c, (method)graf_affiche_reset,  "reset",  0);
    class_addmethod(c, (method)graf_affiche_center, "center", 0);
    class_addmethod(c, (method)graf_affiche_redraw, "redraw", 0);

    // Default box size — shows up in the patcher when the user creates the object
    CLASS_ATTR_DEFAULT(c, "patching_rect", 0, "0. 0. 400. 300.");

    class_register(CLASS_BOX, c);
    graf_affiche_class = c;

    post("graf.affiche: loaded");
}


////////////////////////// object lifecycle

/**
 * Constructor — called when the user creates [graf.affiche] or
 * [graf.affiche my_graph] in a patcher.
 *
 * jbox new protocol (required sequence):
 *   1. object_dictionaryarg — extract the box dictionary from argv
 *   2. object_alloc — allocate our struct
 *   3. jbox_new — initialize the jbox portion of the struct
 *   4. set b_firstin — tells Max which object owns the inlets
 *   5. parse user arguments (see the three-stage lookup below)
 *   6. attr_dictionary_process — apply saved attribute values (size, position)
 *   7. jbox_ready — finalize and trigger first paint
 *
 * CONSTRUCTOR-ARG FIX (CLASS_FLAG_NEWDICTIONARY): with the dictionary
 * protocol, Max does not reliably hand positional box arguments in argv —
 * they live inside the box dictionary. We look in three places, in order:
 *   a. argv itself (plain A_SYM atoms — some Max versions still pass them)
 *   b. the dictionary "args" key (atom array of the positional arguments)
 *   c. the dictionary "text" key (the literal box text "graf.affiche my_graph"
 *      — tokenize it, skip the class-name token, stop at the first @attribute)
 * Stage (c) is the belt-and-braces fallback: the box text always exists.
 *
 * Java analogy:
 *   super(layout);            // jbox_new
 *   this.grafName = args[0];  // parse user args
 *   model.addObserver(this);  // object_subscribe
 *   repaint();                // jbox_ready
 */
void *graf_affiche_new(t_symbol *s, long argc, t_atom *argv)
{
    t_graf_affiche *x = NULL;
    t_dictionary   *d = NULL;
    long i;

    // Step 1: extract patcher dictionary — MUST come first for jbox objects
    if (!(d = object_dictionaryarg(argc, argv)))
        return NULL;

    // Step 2: allocate
    x = (t_graf_affiche *)object_alloc(graf_affiche_class);
    if (!x) return NULL;

    // Step 3: initialize jbox portion.
       /* Box flags control resize behaviour and drawing order:
       JBOX_DRAWFIRSTIN  — draw the first inlet on the box
       JBOX_NODRAWBOX    — suppress Max's default box border (we draw our own bg)
       JBOX_DRAWINLAST   — paint content after inlets/outlets are drawn
       JBOX_TRANSPARENT  — no automatic background fill (we fill in paint)
       JBOX_GROWBOTH     — user can resize both width and height */
    long boxflags = 0
        | JBOX_DRAWFIRSTIN
        | JBOX_NODRAWBOX
        | JBOX_DRAWINLAST
        | JBOX_TRANSPARENT
        | JBOX_GROWBOTH;

    jbox_new(&x->box, boxflags, argc, argv);

    // Step 4: first inlet belongs to us
    x->box.b_firstin = (t_object *)x;

    // Initialize our own state before anything can trigger a paint
    x->graf_name     = NULL;
    x->mode          = GAFF_MODE_CIRCLE;
    x->pos           = NULL;
    x->pos_count     = 0;
    x->pos_capacity  = 0;
    x->view_zoom     = 1.0;
    x->view_pan_x    = 0.0;
    x->view_pan_y    = 0.0;
    // Fixed default seed: random layouts reproduce across patcher reloads.
       //redraw advances it for a fresh scatter.
    x->rand_seed     = 0x9E3779B97F4A7C15ULL;
    x->layout_dirty  = 1;
    x->layout_full   = 1;
    x->needs_autofit = 1;

    // Step 5a: try argv directly — first non-'@' symbol atom
    for (i = 0; i < argc; i++) {
        if (atom_gettype(argv + i) == A_SYM) {
            t_symbol *sym = atom_getsym(argv + i);
            if (sym && sym->s_name[0] != '@') { x->graf_name = sym; break; }
        }
    }
    // TEMP DIAG (issue 3) — remove once the constructor-arg path is confirmed
    if (x->graf_name)
        post("graf.affiche: name '%s' found via argv", x->graf_name->s_name);

    // Step 5b: dictionary "args" key
    if (!x->graf_name && d) {
        long dargc = 0; t_atom *dargv = NULL;
        if (dictionary_getatoms(d, gensym("args"), &dargc, &dargv) == MAX_ERR_NONE) {
            long j;
            for (j = 0; j < dargc; j++) {
                if (atom_gettype(dargv + j) == A_SYM) {
                    t_symbol *sym = atom_getsym(dargv + j);
                    if (sym && sym->s_name[0] != '@') { x->graf_name = sym; break; }
                }
            }
        }
        // TEMP DIAG (issue 3)
        if (x->graf_name)
            post("graf.affiche: name '%s' found via dict args", x->graf_name->s_name);
    }

    // Step 5c: dictionary "text" key — tokenize the raw box text.
    /*Token 0 is the class name ("graf.affiche"); the first following token
    that doesn't start with '@' is our instance name. Positional args always
    precede attribute args in Max box text, so stop at the first '@'. */
    if (!x->graf_name && d) {
        const char *boxtext = NULL;
        if (dictionary_getstring(d, gensym("text"), &boxtext) == MAX_ERR_NONE
            && boxtext) {
            const char *t = boxtext;
            long tokidx = 0;
            char tokbuf[256];
            while (*t) {
                // skip whitespace between tokens
                while (*t == ' ' || *t == '\t' || *t == '\n' || *t == '\r') t++;
                if (!*t) break;
                // measure the token
                long len = 0;
                while (t[len] && t[len] != ' ' && t[len] != '\t'
                       && t[len] != '\n' && t[len] != '\r') len++;
                if (t[0] == '@') break;              // attributes begin — no name given
                if (tokidx > 0) {                    // first token after the class name
                    long copy = (len < 255) ? len : 255;
                    strncpy(tokbuf, t, copy);
                    tokbuf[copy] = '\0';
                    x->graf_name = gensym(tokbuf);
                    break;
                }
                t += len;
                tokidx++;
            }
        }
        // TEMP DIAG (issue 3)
        if (x->graf_name)
            post("graf.affiche: name '%s' found via dict text", x->graf_name->s_name);
    }

    // Permanent guard: all three lookup stages came up empty — either the box
    // genuinely has no name argument, or the lookup is silently failing.
    if (!x->graf_name)
        object_warn((t_object *)x,
            "graf.affiche: no instance name found in argv, dict args, or dict text");

    // Subscribe to the named graf instance.
    /* object_subscribe works by name — the target does not need to exist yet.
    When a [graf my_graph] is created or registered later, we will start
    receiving its notifications automatically.
    Java analogy: model.addObserver(this) — we register as an observer
       on the shared model object identified by name. */
    if (x->graf_name) {
        object_subscribe(CLASS_BOX, x->graf_name,
                         gensym("graf.affiche"), (t_object *)x);
        post("graf.affiche: watching '%s'", x->graf_name->s_name);
    }

    // Step 6: apply saved attribute values from the patcher dictionary
    attr_dictionary_process(x, d);

    // Step 7: finalize — triggers the first paint
    jbox_ready(&x->box);

    return x;
}

/**
 * Destructor — unsubscribe from the watched graf, free the position array,
 * and free jbox resources.
 * Java analogy: model.removeObserver(this) + super.finalize()
 */
void graf_affiche_free(t_graf_affiche *x)
{
    if (x->graf_name) {
        object_unsubscribe(CLASS_BOX, x->graf_name,
                           gensym("graf.affiche"), (t_object *)x);
    }
    if (x->pos)
        sysmem_freeptr(x->pos);
    jbox_free(&x->box);
}

void graf_affiche_assist(t_graf_affiche *x, void *b, long m, long a, char *s)
{
    if (m == ASSIST_INLET)
        sprintf(s, "bang | update <name> | mode <name> | zoom <in/out> | "
                   "move <dir> | reset | center | redraw");
}


////////////////////////// notification and message handlers

/**
 * notify — called when a subscribed object sends a notification,
 * and also when jbox attributes (size, position, color) are modified.
 *
 * "modified" (from the watched graf) marks the layout dirty; the actual
 * position sync happens lazily at the top of the next paint. jbox_redraw
 * coalesces repeated calls into a single repaint per event cycle, so a bulk
 * CSV load fires many notifies but costs exactly one layout sync + paint.
 *
 * "attr_modified" (our own attributes — a resize, say) only needs a repaint:
 * world positions do not depend on the box size, that's the whole point.
 *
 * Java analogy: @Override public void update(Observable o, Object arg) { repaint(); }
 */
t_max_err graf_affiche_notify(t_graf_affiche *x, t_symbol *s, t_symbol *msg,
                               void *sender, void *data)
{
    /** DIAGNOSTIC: uncomment to see all notifications in the console
    if (msg == gensym("modified")) {
        // TEMP DIAG (issue 3)
        post("graf.affiche: got 'modified' notify");
        x->layout_dirty = 1;
        jbox_redraw(&x->box);
    }
    else if (msg == gensym("attr_modified")) { */
    if (msg == gensym("attr_modified")) {
        jbox_redraw(&x->box);
    }
    return MAX_ERR_NONE;
}

/**
 * bang — force an immediate repaint.
 * Does NOT recompute the layout (that's `redraw`); useful if the subscription
 * mechanism ever misses a state change.
 */
void graf_affiche_bang(t_graf_affiche *x)
{
    jbox_redraw(&x->box);
}

/**
 * update <name> — switch to watching a different named [graf] instance.
 * Unsubscribes from the current instance first, then subscribes to the new one.
 * Triggers a full relayout and an auto-fit (new graph, new bounding box).
 */
void graf_affiche_update(t_graf_affiche *x, t_symbol *name)
{
    // No-op switch: already watching this instance. Without this guard,
       /*`update` to the same name forces layout_full + autofit — a view refit
       indistinguishable from `reset`. Just repaint, like bang.
       (First assignment still works: graf_name is NULL then, so any real
       name fails this comparison and falls through.) */
    if (name == x->graf_name) {
        jbox_redraw(&x->box);
        return;
    }

    if (x->graf_name) {
        object_unsubscribe(CLASS_BOX, x->graf_name,
                           gensym("graf.affiche"), (t_object *)x);
    }

    x->graf_name = name;

    if (name && name != gensym("")) {
        object_subscribe(CLASS_BOX, name, gensym("graf.affiche"), (t_object *)x);
        post("graf.affiche: now watching '%s'", name->s_name);
    }

    x->layout_dirty  = 1;
    x->layout_full   = 1;
    x->needs_autofit = 1;
    jbox_redraw(&x->box);
}

/**
 * mode <name> — switch layout mode.
 * A mode switch is always a from-scratch relayout (per-id grid stability is
 * only meaningful within a mode) and auto-fits the view so the new layout's
 * bounding box is visible — a line and a circle of the same graph can live
 * in very different corners of world space.
 */
void graf_affiche_mode(t_graf_affiche *x, t_symbol *name)
{
    long m = -1;

    if      (name == gensym("circle"))    m = GAFF_MODE_CIRCLE;
    else if (name == gensym("line"))      m = GAFF_MODE_LINE;
    else if (name == gensym("random"))    m = GAFF_MODE_RANDOM;
    else if (name == gensym("grid"))      m = GAFF_MODE_GRID;
    else if (name == gensym("comb"))      m = GAFF_MODE_COMB;
    else if (name == gensym("treeup"))    m = GAFF_MODE_TREEUP;
    else if (name == gensym("treedown"))  m = GAFF_MODE_TREEDOWN;
    else if (name == gensym("treeleft"))  m = GAFF_MODE_TREELEFT;
    else if (name == gensym("treeright")) m = GAFF_MODE_TREERIGHT;
    else if (name == gensym("rings"))     m = GAFF_MODE_RINGS;

    if (m < 0) {
        object_error((t_object *)x,
            "mode: unknown mode '%s' (circle line random grid comb "
            "treeup treedown treeleft treeright rings)",
            name ? name->s_name : "");
        return;
    }

    x->mode          = m;
    x->layout_dirty  = 1;
    x->layout_full   = 1;
    x->needs_autofit = 1;
    jbox_redraw(&x->box);
}

/**
 * zoom <in|out> — multiply/divide the zoom factor by GAFF_ZOOM_STEP,
 * clamped to [GAFF_ZOOM_MIN, GAFF_ZOOM_MAX].
 *
 * Zoom is anchored at the viewport center: whatever world point sits at the
 * center of the box stays there. With screen = world*zoom + pan + boxcenter,
 * that anchor point is -pan/zoom, so scaling pan by the same factor as zoom
 * keeps it fixed.
 */
void graf_affiche_zoom(t_graf_affiche *x, t_symbol *dir)
{
    double factor;

    if      (dir == gensym("in"))  factor = GAFF_ZOOM_STEP;
    else if (dir == gensym("out")) factor = 1.0 / GAFF_ZOOM_STEP;
    else {
        object_error((t_object *)x, "zoom: expected 'in' or 'out'");
        return;
    }

    double newzoom = x->view_zoom * factor;
    if (newzoom < GAFF_ZOOM_MIN) newzoom = GAFF_ZOOM_MIN;
    if (newzoom > GAFF_ZOOM_MAX) newzoom = GAFF_ZOOM_MAX;

    // applied factor after clamping (may differ from requested at the bounds)
    double applied = newzoom / x->view_zoom;
    x->view_zoom   = newzoom;
    x->view_pan_x *= applied;
    x->view_pan_y *= applied;

    // report the APPLIED value — at the clamp bounds it differs from requested
    post("graf.affiche: zoom %.0f%%", x->view_zoom * 100.0);
    jbox_redraw(&x->box);
}

/**
 * move <left|right|up|down> — pan the VIEWPORT by GAFF_PAN_STEP screen pixels.
 * "move left" moves the viewport left, so the content slides right.
 * Pan is deliberately unclamped — `reset` is the recovery path if you get lost.
 */
void graf_affiche_move(t_graf_affiche *x, t_symbol *dir)
{
    if      (dir == gensym("left"))  x->view_pan_x += GAFF_PAN_STEP;
    else if (dir == gensym("right")) x->view_pan_x -= GAFF_PAN_STEP;
    else if (dir == gensym("up"))    x->view_pan_y += GAFF_PAN_STEP;
    else if (dir == gensym("down"))  x->view_pan_y -= GAFF_PAN_STEP;
    else {
        object_error((t_object *)x, "move: expected left, right, up or down");
        return;
    }

    post("graf.affiche: pan %.0f %.0f", x->view_pan_x, x->view_pan_y);
    jbox_redraw(&x->box);
}

/**
 * reset — recompute pan+zoom together so the full world-space bounding box
 * of the current graph fits the viewport ("show me everything").
 * Positions are untouched; this is purely a view operation.
 */
void graf_affiche_reset(t_graf_affiche *x)
{
    t_rect rect;
    jbox_get_patching_rect((t_object *)&x->box, &rect);

    if (x->pos_count == 0) {
        // nothing to frame — return to the neutral view
        x->view_zoom  = 1.0;
        x->view_pan_x = 0.0;
        x->view_pan_y = 0.0;
    } else {
        gaff_autofit(x, rect.width, rect.height);
    }

    post("graf.affiche: reset — zoom %.0f%%", x->view_zoom * 100.0);
    jbox_redraw(&x->box);
}

/**
 * center — pan only (zoom unchanged) so that graf's current traversal node
 * sits at the center of the viewport. For following playback in a large
 * graph without losing your zoom level.
 */
void graf_affiche_center(t_graf_affiche *x)
{
    t_graf *graph = x->graf_name ? graf_find(x->graf_name) : NULL;
    long i;

    if (!graph || !graph->current) {
        object_warn((t_object *)x, "center: no current node to center on");
        return;
    }

    // find the stored world position of the current node
    for (i = 0; i < x->pos_count; i++) {
        if (x->pos[i].id == graph->current) {
            // screen = world*zoom + pan + boxcenter; we want screen == boxcenter, therfore pan = -world*zoom */
            x->view_pan_x = -x->pos[i].wx * x->view_zoom;
            x->view_pan_y = -x->pos[i].wy * x->view_zoom;
            post("graf.affiche: centered on '%s'", graph->current->s_name);
            jbox_redraw(&x->box);
            return;
        }
    }

    object_warn((t_object *)x, "center: current node has no layout position yet");
}

/**
 * redraw — force a full from-scratch layout recompute in the current mode.
 * Distinct from the automatic recompute-on-modified:
 *   grid/comb — abandons per-id cells, repacks densely (defragment after removals)
 *   random    — advances the instance seed for a fresh scatter
 *   others    — recompute is deterministic anyway; this is the escape hatch
 *               if positions ever look wrong.
 */
void graf_affiche_redraw(t_graf_affiche *x)
{
    if (x->mode == GAFF_MODE_RANDOM) {
        // LCG step — a new seed gives a completely different (but again stable) scatter
        x->rand_seed = x->rand_seed * 6364136223846793005ULL + 1442695040888963407ULL;
    }

    post("graf.affiche: redraw (%s)", gaff_mode_name(x->mode));
    x->layout_dirty = 1;
    x->layout_full  = 1;
    jbox_redraw(&x->box);
}


////////////////////////// layout engine — helpers

// Insertion-order index of a node id within the graph, -1 if absent.
//Same linear scan as graf_find_node, but returning the index.
static long gaff_index_of(t_graf *graph, t_symbol *id)
{
    long k;
    for (k = 0; k < graph->node_count; k++) {
        if (graph->nodes[k].id == id)
            return k;
    }
    return -1;
}

static const char *gaff_mode_name(long mode)
{
    switch (mode) {
        case GAFF_MODE_CIRCLE:    return "circle";
        case GAFF_MODE_LINE:      return "line";
        case GAFF_MODE_RANDOM:    return "random";
        case GAFF_MODE_GRID:      return "grid";
        case GAFF_MODE_COMB:      return "comb";
        case GAFF_MODE_TREEUP:    return "treeup";
        case GAFF_MODE_TREEDOWN:  return "treedown";
        case GAFF_MODE_TREELEFT:  return "treeleft";
        case GAFF_MODE_TREERIGHT: return "treeright";
        case GAFF_MODE_RINGS:     return "rings";
        default:                  return "?";
    }
}

/**
 * FNV-1a 64-bit hash of a node id string, mixed with the instance seed and
 * finalized with the splitmix64 avalanche. Hashing the STRING (not the
 * t_symbol pointer) makes random-mode positions stable across sessions —
 * interned pointers change every launch, the characters don't.
 *
 * Java analogy: a deterministic String.hashCode() variant with much better
 * bit dispersion, used the way you'd seed new Random(id.hashCode() ^ seed).
 */
static unsigned long long gaff_hash_id(const char *s, unsigned long long seed)
{
    unsigned long long h = 1469598103934665603ULL;      // FNV offset basis
    while (*s) {
        h ^= (unsigned long long)(unsigned char)*s++;
        h *= 1099511628211ULL;                          // FNV prime
    }
    h ^= seed;
    // splitmix64 finalizer — avalanches every input bit across the output
    h += 0x9E3779B97F4A7C15ULL;
    h = (h ^ (h >> 30)) * 0xBF58476D1CE4E5B9ULL;
    h = (h ^ (h >> 27)) * 0x94D049BB133111EBULL;
    return h ^ (h >> 31);
}

// Map the top 53 bits of a hash to a double in [0, 1). */
static double gaff_hash01(unsigned long long h)
{
    return (double)(h >> 11) * (1.0 / 9007199254740992.0);  // 2^-53
}

/**
 * gaff_tree_bfs — build a spanning forest of the graph by BFS.
 *
 * Root detection: every node with in-degree 0 is a root (self-loops ignored
 * when counting in-degree — a node whose only incoming edge is its own loop
 * is still an entry point). Multiple roots = a forest, all traversed in one
 * multi-source BFS so the trees sit side by side.
 *
 * FALLBACK for nodes unreachable from any root (a pure cycle with no
 * in-degree-0 entry): the unvisited node with the LOWEST insertion index is
 * promoted to pseudo-root and BFS continues from it, repeating until every
 * node is placed. Every node therefore gets a proper tree position; the
 * cycle's closing edge simply draws as a curved back-edge.
 *
 * Cycle breaking: the visited[] set guarantees each node is placed exactly
 * once — a back-edge to an already-placed node is drawn as a normal (curved)
 * edge by paint, it never re-places the node.
 *
 * Outputs (caller-allocated, length n):
 *   order[]     — node indices in BFS placement order. Children of a node are
 *                 CONTIGUOUS in this array, in out-edge order — the property
 *                 the Reingold–Tilford pass in gaff_layout_sync relies on.
 *   parent[]    — BFS parent as a node index, -1 for roots/pseudo-roots
 *   depth[]     — distance from the root
 *   component[] — WEAKLY-CONNECTED component id, dense 0..K-1 in insertion
 *                 order. Computed by union-find over all edges with direction
 *                 ignored — NOT per BFS root: two roots that converge on a
 *                 shared node are one component (they are connected), so
 *                 rings keeps them in one cluster sharing the half-step
 *                 depth-0 ring. Only genuinely disconnected islands (including
 *                 pure-cycle pseudo-root islands) get distinct ids.
 *                 Trees ignore this array; rings uses it for clustering.
 *
 * Returns the number of placed nodes (always n), or -1 on allocation failure.
 *
 * Java analogy for union-find: a disjoint-set forest — each entry points at
 * its parent set, find() walks to the root (halving the path as it goes),
 * union() hangs one root under the other.
 *
 * Java analogy: a textbook BFS over an adjacency list, except "the queue" and
 * "the result order" are the same array — order[] never pops, it just grows
 * while qhead chases qlen.
 */
static long gaff_tree_bfs(t_graf *graph, long *order, long *parent, long *depth,
                           long *component)
{
    long  n = graph->node_count;
    long  i, j, qhead = 0, qlen = 0;
    // one block: first n longs = in-degree, second n = union-find parents
    long *indeg   = (long *)sysmem_newptr(2 * n * sizeof(long));
    char *visited = (char *)sysmem_newptr(n * sizeof(char));

    if (!indeg || !visited) {
        if (indeg)   sysmem_freeptr(indeg);
        if (visited) sysmem_freeptr(visited);
        return -1;
    }
    long *uf = indeg + n;

    for (i = 0; i < n; i++) { indeg[i] = 0; visited[i] = 0; uf[i] = i; }

    // in-degree count over all edges, self-loops excluded;
    // same pass unions the endpoints (direction ignored) for weak connectivity
    for (i = 0; i < n; i++) {
        t_graf_node *src = &graph->nodes[i];
        for (j = 0; j < src->edge_count; j++) {
            long ti = gaff_index_of(graph, src->edges_to[j]);
            if (ti >= 0 && ti != i) {
                indeg[ti]++;
                // find both roots (path halving), hang the larger under the smaller
                long a = i, b = ti;
                while (uf[a] != a) { uf[a] = uf[uf[a]]; a = uf[a]; }
                while (uf[b] != b) { uf[b] = uf[uf[b]]; b = uf[b]; }
                if (a != b) { if (a < b) uf[b] = a; else uf[a] = b; }
            }
        }
    }

    // Flatten union-find into dense component ids, numbered 0..K-1 by lowest
       /* insertion index. Because union always keeps the smaller index as root,
       every set root is its own component's first node — so a root's id is
       always assigned before any member that points at it. */
    {
        long ncomp = 0;
        for (i = 0; i < n; i++) {
            long r = i;
            while (uf[r] != r) r = uf[r];
            component[i] = (r == i) ? ncomp++ : component[r];
        }
    }

    // enqueue all true roots first, in insertion order
    for (i = 0; i < n; i++) {
        if (indeg[i] == 0) {
            order[qlen] = i;
            parent[i]   = -1;
            depth[i]    = 0;
            visited[i]  = 1;
            qlen++;
        }
    }

    // BFS to exhaustion; promote pseudo-roots until every node is placed
    for (;;) {
        while (qhead < qlen) {
            long u = order[qhead++];
            t_graf_node *src = &graph->nodes[u];
            for (j = 0; j < src->edge_count; j++) {
                long v = gaff_index_of(graph, src->edges_to[j]);
                if (v >= 0 && v != u && !visited[v]) {
                    visited[v]  = 1;
                    parent[v]   = u;
                    depth[v]    = depth[u] + 1;
                    order[qlen] = v;
                    qlen++;
                }
            }
        }
        if (qlen >= n) break;

        // unreachable cluster: lowest-insertion-index unvisited node becomes
        // a pseudo-root (documented fallback — see block comment above)
        for (i = 0; i < n; i++) {
            if (!visited[i]) {
                order[qlen] = i;
                parent[i]   = -1;
                depth[i]    = 0;
                visited[i]  = 1;
                qlen++;
                break;
            }
        }
    }

    sysmem_freeptr(indeg);
    sysmem_freeptr(visited);
    return qlen;
}

/**
 * gaff_layout_sync — (re)compute world positions for every node.
 *
 * Called lazily from paint when layout_dirty is set (graph "modified", mode
 * change, redraw, graph switch) — never on a plain repaint. Rebuilds x->pos
 * so that entry k mirrors graph->nodes[k]; per-mode stability guarantees are
 * carried by the coordinates, not the array order:
 *
 *   circle/line — deterministic from insertion order; removals close gaps.
 *   random      — pure function of (id string, instance seed); nothing stored
 *                 actually needs to survive, recompute is identical by design.
 *   grid/comb   — STICKY: each id keeps the cell it was first given; removals
 *                 leave holes; new ids take the lowest free cell. layout_full
 *                 (mode change / redraw) repacks cells densely.
 *   trees/rings — full recompute from graph structure every time (cheap,
 *                 O(n + e·n) with the linear id lookup). The determinism of
 *                 the subtree-width algorithm is what gives incremental
 *                 stability for free: adding a child re-centers its ancestor
 *                 path and shifts unrelated branches only as far as the grown
 *                 subtree width forces; other roots move not at all (modulo
 *                 the forest packing cursor).
 */
static void gaff_layout_sync(t_graf_affiche *x, t_graf *graph)
{
    long n = graph->node_count;
    long k;

    if (n == 0) {
        // empty graph: drop everything, including grid cell history —
        // a `clear` starts grid/comb packing from scratch by design
        x->pos_count    = 0;
        x->layout_dirty = 0;
        x->layout_full  = 0;
        return;
    }

    // Grow the position array (doubling, sysmem — kills the old 256 cap).
    // Java analogy: ArrayList.ensureCapacity(n).
    if (n > x->pos_capacity) {
        long newcap = (x->pos_capacity > 0) ? x->pos_capacity : 16;
        while (newcap < n) newcap *= 2;
        t_gaff_pos *grown = x->pos
            ? (t_gaff_pos *)sysmem_resizeptr(x->pos, newcap * sizeof(t_gaff_pos))
            : (t_gaff_pos *)sysmem_newptr(newcap * sizeof(t_gaff_pos));
        if (!grown) {
            object_error((t_object *)x, "layout: out of memory (%ld nodes)", n);
            return;
        }
        x->pos          = grown;
        x->pos_capacity = newcap;
    }

    switch (x->mode) {

    case GAFF_MODE_CIRCLE: {
        // Fixed arc length per node: radius grows as nodes are added instead
          /* of spacing shrinking. World center at the origin, start at
           12 o'clock, clockwise (screen y grows downward). */
        double r = (double)n * GAFF_ARC_LENGTH / (2.0 * M_PI);
        for (k = 0; k < n; k++) {
            double angle = (2.0 * M_PI * (double)k / (double)n) - M_PI * 0.5;
            x->pos[k].id     = graph->nodes[k].id;
            x->pos[k].wx     = (n == 1) ? 0.0 : r * cos(angle);
            x->pos[k].wy     = (n == 1) ? 0.0 : r * sin(angle);
            x->pos[k].cell   = -1;
            x->pos[k].parent = -1;
            x->pos[k].depth  = 0;
        }
        break;
    }

    case GAFF_MODE_LINE: {
        // single row, fixed pitch, extends rightward with insertion order
        for (k = 0; k < n; k++) {
            x->pos[k].id     = graph->nodes[k].id;
            x->pos[k].wx     = (double)k * GAFF_LINE_SPACING;
            x->pos[k].wy     = 0.0;
            x->pos[k].cell   = -1;
            x->pos[k].parent = -1;
            x->pos[k].depth  = 0;
        }
        break;
    }

    case GAFF_MODE_RANDOM: {
        // Position = hash of the id string ⊕ instance seed. Stable per id:
           /* never re-randomized by repaints or by other nodes arriving. Two
           independent avalanche passes give uncorrelated x and y. */
        for (k = 0; k < n; k++) {
            const char *name = graph->nodes[k].id->s_name;
            unsigned long long hx = gaff_hash_id(name, x->rand_seed);
            unsigned long long hy = gaff_hash_id(name, x->rand_seed ^ 0xA5A5A5A5A5A5A5A5ULL);
            x->pos[k].id     = graph->nodes[k].id;
            x->pos[k].wx     = (gaff_hash01(hx) - 0.5) * GAFF_RANDOM_EXTENT;
            x->pos[k].wy     = (gaff_hash01(hy) - 0.5) * GAFF_RANDOM_EXTENT;
            x->pos[k].cell   = -1;
            x->pos[k].parent = -1;
            x->pos[k].depth  = 0;
        }
        break;
    }

    case GAFF_MODE_GRID:
    case GAFF_MODE_COMB: {
        // Sticky cells. Incremental sync: surviving ids keep their cell, new
           /* ids take the lowest free cell. Because freed cells are always
           reused first, the highest cell index stays bounded by the peak
           node count — the used[] bitmap below is sized accordingly. */
        long *cells = (long *)sysmem_newptr(n * sizeof(long));
        if (!cells) {
            object_error((t_object *)x, "layout: out of memory");
            return;
        }

        if (x->layout_full || x->pos_count == 0) {
            // from scratch: densely packed in insertion order (defragment)
            for (k = 0; k < n; k++)
                cells[k] = k;
        } else {
            // carry cells over by id from the previous sync
            long maxcell = 0, m, nfree = 0;
            for (k = 0; k < n; k++) {
                cells[k] = -1;
                for (m = 0; m < x->pos_count; m++) {
                    if (x->pos[m].id == graph->nodes[k].id && x->pos[m].cell >= 0) {
                        cells[k] = x->pos[m].cell;
                        if (cells[k] > maxcell) maxcell = cells[k];
                        break;
                    }
                }
            }
            // lowest-free-cell assignment for the newcomers
            long span = maxcell + n + 2;    // enough room for every candidate cell
            char *used = (char *)sysmem_newptr(span * sizeof(char));
            if (!used) {
                sysmem_freeptr(cells);
                object_error((t_object *)x, "layout: out of memory");
                return;
            }
            for (m = 0; m < span; m++) used[m] = 0;
            for (k = 0; k < n; k++)
                if (cells[k] >= 0) used[cells[k]] = 1;
            for (k = 0; k < n; k++) {
                if (cells[k] < 0) {
                    while (used[nfree]) nfree++;
                    cells[k]       = nfree;
                    used[nfree]    = 1;
                }
            }
            sysmem_freeptr(used);
        }

        // cell -> world coordinates (comb: odd rows shifted half a cell)
        for (k = 0; k < n; k++) {
            long row = cells[k] / GAFF_GRID_COLS;
            long col = cells[k] % GAFF_GRID_COLS;
            double offset = (x->mode == GAFF_MODE_COMB && (row & 1))
                            ? GAFF_CELL * 0.5 : 0.0;
            x->pos[k].id     = graph->nodes[k].id;
            x->pos[k].wx     = (double)col * GAFF_CELL + offset;
            x->pos[k].wy     = (double)row * GAFF_CELL;
            x->pos[k].cell   = cells[k];
            x->pos[k].parent = -1;
            x->pos[k].depth  = 0;
        }
        sysmem_freeptr(cells);
        break;
    }

    case GAFF_MODE_TREEUP:
    case GAFF_MODE_TREEDOWN:
    case GAFF_MODE_TREELEFT:
    case GAFF_MODE_TREERIGHT:
    case GAFF_MODE_RINGS: {
        /* Spanning forest by BFS, then either Reingold–Tilford subtree-width
           placement (trees) or concentric rings by depth (rings).

           Scratch layout: four long arrays + three double arrays, all length
           n, allocated as two blocks. Java analogy: a bunch of local int[n] /
           double[n] — C just makes you do the arena arithmetic yourself. */
        long   *order  = (long *)sysmem_newptr(4 * n * sizeof(long));
        double *width  = (double *)sysmem_newptr(3 * n * sizeof(double));
        if (!order || !width) {
            if (order) sysmem_freeptr(order);
            if (width) sysmem_freeptr(width);
            object_error((t_object *)x, "layout: out of memory");
            return;
        }
        long   *parent    = order + n;
        long   *depth     = order + 2 * n;
        long   *component = order + 3 * n;  // weakly-connected component ids (rings)
        double *center = width + n;     // subtree center along the sibling axis
        double *ccur   = width + 2 * n; // per-node cursor for placing children

        if (gaff_tree_bfs(graph, order, parent, depth, component) < 0) {
            sysmem_freeptr(order);
            sysmem_freeptr(width);
            object_error((t_object *)x, "layout: out of memory");
            return;
        }

        if (x->mode == GAFF_MODE_RINGS) {
            /* Concentric rings, PER COMPONENT: within one weakly-connected
               component, nodes at BFS depth d sit on a circle of radius
               d * GAFF_RING_STEP, evenly spaced in BFS order, 12 o'clock
               start. A single depth-0 node sits at the exact component
               center; several roots share a small half-step ring.

               Disconnected components each get their OWN ring cluster, laid
               out in local polar coordinates and translated to a cell on a
               coarse grid (same row/col math as GAFF_MODE_GRID): cell size =
               2 * largest component radius + margin, row length =
               ceil(sqrt(component count)). A single-component graph gets
               exactly one cell at the origin — behavior unchanged. */
            long qi, c, ncomp = 0;
            for (k = 0; k < n; k++)
                if (component[k] >= ncomp) ncomp = component[k] + 1;

            // per-component max BFS depth -> per-component bounding radius
            long *cmaxd = (long *)sysmem_newptr(ncomp * sizeof(long));
            if (!cmaxd) {
                sysmem_freeptr(order);
                sysmem_freeptr(width);
                object_error((t_object *)x, "layout: out of memory");
                return;
            }
            for (c = 0; c < ncomp; c++) cmaxd[c] = 0;
            for (k = 0; k < n; k++)
                if (depth[k] > cmaxd[component[k]]) cmaxd[component[k]] = depth[k];

            double maxrad = 0.0;
            for (c = 0; c < ncomp; c++) {
                double rad = (double)cmaxd[c] * GAFF_RING_STEP + GAFF_NODE_RADIUS;
                if (rad > maxrad) maxrad = rad;
            }

            long rowlen = (long)ceil(sqrt((double)ncomp));
            if (rowlen < 1) rowlen = 1;
            double cellsize = 2.0 * maxrad + GAFF_RING_CLUSTER_MARGIN;

            // Position one component at a time: its own per-depth population
               /* and fill index (width[]/center[] reused per component — depths
               are local, so only entries 0..cmaxd[c] are touched), then
               translate the local polar coords by the component's grid cell. */
            for (c = 0; c < ncomp; c++) {
                long maxd = cmaxd[c];
                for (k = 0; k <= maxd; k++) { width[k] = 0.0; center[k] = 0.0; }
                for (k = 0; k < n; k++)
                    if (component[k] == c) width[depth[k]] += 1.0;  // ring populations

                double ox = (double)(c % rowlen) * cellsize;        // cell world offset
                double oy = (double)(c / rowlen) * cellsize;

                for (qi = 0; qi < n; qi++) {
                    long v = order[qi];
                    if (component[v] != c) continue;
                    long d = depth[v];
                    double cnt = width[d];
                    double idx = center[d];
                    center[d] += 1.0;

                    double radius = (d == 0)
                        ? (cnt > 1.0 ? GAFF_RING_STEP * 0.5 : 0.0)
                        : (double)d * GAFF_RING_STEP;
                    double angle  = (cnt > 0.0)
                        ? (2.0 * M_PI * idx / cnt) - M_PI * 0.5
                        : -M_PI * 0.5;

                    x->pos[v].id     = graph->nodes[v].id;
                    x->pos[v].wx     = ox + radius * cos(angle);
                    x->pos[v].wy     = oy + radius * sin(angle);
                    x->pos[v].cell   = -1;
                    x->pos[v].parent = parent[v];
                    x->pos[v].depth  = d;
                }
            }
            sysmem_freeptr(cmaxd);
        } else {
            // Simplified Reingold–Tilford, two passes over the BFS order:

               /* Pass 1 (reverse order = children before parents): subtree width
               along the sibling axis. A leaf is 1 unit wide; an internal node
               is exactly the sum of its children's widths.

               Pass 2 (forward order = parents before children): each node is
               centered over the span of its children. Children of u are
               contiguous in order[] (BFS property), so a running cursor
               ccur[u], initialized to the left edge of u's span, hands each
               child its slice in out-edge order.

               This determinism is the whole trick: adding a third child to a
               node with two children recomputes to three even slices and
               pushes unrelated branches only by the width increase — no
               incremental diffing needed. Roots pack left-to-right with a
               fixed gap, so other trees only shift if an earlier tree grew. */
            long qi;

            for (k = 0; k < n; k++) width[k] = 0.0;
            for (qi = n - 1; qi >= 0; qi--) {
                long v = order[qi];
                if (width[v] == 0.0) width[v] = 1.0;           // leaf
                if (parent[v] >= 0)  width[parent[v]] += width[v];
            }

            double cursor = 0.0;                                // forest packing cursor
            for (qi = 0; qi < n; qi++) {
                long v = order[qi];
                if (parent[v] < 0) {
                    center[v] = cursor + width[v] * 0.5;
                    cursor   += width[v] + GAFF_TREE_GAP;
                } else {
                    center[v]        = ccur[parent[v]] + width[v] * 0.5;
                    ccur[parent[v]] += width[v];
                }
                ccur[v] = center[v] - width[v] * 0.5;           // left edge of v's span

                // orientation: sibling axis × depth axis
                double sib = center[v] * GAFF_TREE_PITCH;
                double lvl = (double)depth[v] * GAFF_TREE_LEVEL;

                x->pos[v].id     = graph->nodes[v].id;
                x->pos[v].cell   = -1;
                x->pos[v].parent = parent[v];
                x->pos[v].depth  = depth[v];

                switch (x->mode) {
                    case GAFF_MODE_TREEUP:                      // roots top, grow down
                        x->pos[v].wx =  sib;  x->pos[v].wy =  lvl;  break;
                    case GAFF_MODE_TREEDOWN:                    // roots bottom, grow up
                        x->pos[v].wx =  sib;  x->pos[v].wy = -lvl;  break;
                    case GAFF_MODE_TREELEFT:                    // roots left, grow right
                        x->pos[v].wx =  lvl;  x->pos[v].wy =  sib;  break;
                    default:                                    // TREERIGHT: roots right
                        x->pos[v].wx = -lvl;  x->pos[v].wy =  sib;  break;
                }
            }
        }

        sysmem_freeptr(order);
        sysmem_freeptr(width);
        break;
    }
    } // switch (mode)

    x->pos_count    = n;
    x->layout_dirty = 0;
    x->layout_full  = 0;
}

/**
 * gaff_edge_skip — how many nodes does the edge i -> ti "skip over" in the
 * current layout's natural node ordering? 0 means adjacent-in-layout: draw
 * straight. >0: draw curved, offset scaling with the count.
 *
 * The notion of adjacency is mode-specific:
 *   circle      — ring distance 1 (with wrap-around)
 *   line        — index distance 1
 *   grid/comb   — within one cell in both axes (Chebyshev distance 1)
 *   trees/rings — the BFS spanning-tree edges (parent -> child); everything
 *                 else, including a child's edge BACK to its parent, curves —
 *                 which is exactly what keeps back-edges from lying on top of
 *                 tree edges
 *   random      — index distance 1; there is no natural ordering in a random
 *                 scatter, so insertion order is used as an arbitrary-but-
 *                 STABLE stand-in (the offset cap keeps large values sane)
 */
static long gaff_edge_skip(t_graf_affiche *x, long i, long ti, long n)
{
    long d = (i > ti) ? (i - ti) : (ti - i);

    switch (x->mode) {
        case GAFF_MODE_CIRCLE: {
            long ring = (d < n - d) ? d : n - d;    // shorter way around
            return ring - 1;
        }
        case GAFF_MODE_LINE:
        case GAFF_MODE_RANDOM:
            return d - 1;

        case GAFF_MODE_GRID:
        case GAFF_MODE_COMB: {
            long ci = x->pos[i].cell,  ct = x->pos[ti].cell;
            long dr = (ci / GAFF_GRID_COLS) - (ct / GAFF_GRID_COLS);
            long dc = (ci % GAFF_GRID_COLS) - (ct % GAFF_GRID_COLS);
            if (dr < 0) dr = -dr;
            if (dc < 0) dc = -dc;
            long cheb = (dr > dc) ? dr : dc;
            return cheb - 1;
        }
        default:    // tree modes and rings
            if (x->pos[ti].parent == i)
                return 0;                           // spanning-tree edge: straight
            return (d > 0) ? d : 1;                 // non-tree edge: always curves
    }
}

/**
 * gaff_autofit — the `reset` math: fit the world bounding box of all stored
 * positions into a view of the given size, with margin, zoom clamped.
 * Node circles occupy GAFF_NODE_RADIUS world units around each center
 * (screen radius is R*zoom), so the box is padded accordingly.
 */
static void gaff_autofit(t_graf_affiche *x, double view_w, double view_h)
{
    double minx, miny, maxx, maxy;
    long k;

    if (x->pos_count == 0) return;

    minx = maxx = x->pos[0].wx;
    miny = maxy = x->pos[0].wy;
    for (k = 1; k < x->pos_count; k++) {
        if (x->pos[k].wx < minx) minx = x->pos[k].wx;
        if (x->pos[k].wx > maxx) maxx = x->pos[k].wx;
        if (x->pos[k].wy < miny) miny = x->pos[k].wy;
        if (x->pos[k].wy > maxy) maxy = x->pos[k].wy;
    }

    double pad = GAFF_NODE_RADIUS + 8.0;            // world-space padding
    double bw  = (maxx - minx) + 2.0 * pad;
    double bh  = (maxy - miny) + 2.0 * pad;
    // Floor the fit box: a 1-node graph's real bbox is ~2*pad, which a 400px
      /* view would "fit" at 5x zoom — absurd magnification exactly when nodes
       are being typed in one at a time. Once the real bbox exceeds the floor,
       autofit behaves exactly as before. */
    if (bw < GAFF_FIT_MIN_SPAN) bw = GAFF_FIT_MIN_SPAN;
    if (bh < GAFF_FIT_MIN_SPAN) bh = GAFF_FIT_MIN_SPAN;

    double zx = view_w * GAFF_FIT_MARGIN / bw;
    double zy = view_h * GAFF_FIT_MARGIN / bh;
    double z  = (zx < zy) ? zx : zy;
    if (z < GAFF_ZOOM_MIN) z = GAFF_ZOOM_MIN;
    if (z > GAFF_ZOOM_MAX) z = GAFF_ZOOM_MAX;

    x->view_zoom  = z;
    // pan so the bbox center lands on the viewport center
    x->view_pan_x = -(minx + maxx) * 0.5 * z;
    x->view_pan_y = -(miny + maxy) * 0.5 * z;
}


////////////////////////// internal drawing helpers

/**
 * Draw a straight directed edge from (x1,y1) to (x2,y2) with an arrowhead.
 * All coordinates are SCREEN coordinates; node_r is the screen node radius
 * (already zoom-scaled) so the shaft starts/ends at the circle rims.
 */
static void gaff_draw_arrow(t_jgraphics *g,
                             double x1, double y1,
                             double x2, double y2,
                             double node_r, double zoom)
{
    double dx  = x2 - x1;
    double dy  = y2 - y1;
    double len = sqrt(dx * dx + dy * dy);
    if (len < 1.0) return;

    double ndx = dx / len;  // normalized direction */
    double ndy = dy / len;

    // Start and end points, pulled back from each center by the node radius */
    double sx = x1 + ndx * node_r;
    double sy = y1 + ndy * node_r;
    double ex = x2 - ndx * node_r;
    double ey = y2 - ndy * node_r;

    double alen = GAFF_ARROW_LEN * zoom;

    jgraphics_set_source_jrgba(g, &GAFF_COLOR_EDGE);
    jgraphics_set_line_width(g, 1.2 * zoom);

    // Edge shaft */
    jgraphics_move_to(g, sx, sy);
    jgraphics_line_to(g, ex, ey);
    jgraphics_stroke(g);

    // Arrowhead: two lines diverging from the endpoint at ±GAFF_ARROW_ANGLE */
    double angle = atan2(dy, dx);
    jgraphics_move_to(g, ex, ey);
    jgraphics_line_to(g,
        ex - alen * cos(angle - GAFF_ARROW_ANGLE),
        ey - alen * sin(angle - GAFF_ARROW_ANGLE));
    jgraphics_move_to(g, ex, ey);
    jgraphics_line_to(g,
        ex - alen * cos(angle + GAFF_ARROW_ANGLE),
        ey - alen * sin(angle + GAFF_ARROW_ANGLE));
    jgraphics_stroke(g);
}

/**
 * Draw a curved directed edge as a quadratic bezier bowing LEFT of the
 * direction of travel — the curve arcs around intermediate nodes instead of
 * crossing through them, and reciprocal edges bow to opposite sides.
 *
 * bow    — apex distance from the straight chord, screen px (already scaled).
 * node_r — screen node radius for rim trimming.
 * label_x/label_y — out-parameters: a good spot for the weight label
 *                   (the curve apex, pushed a little further out). Pass NULL
 *                   if not needed.
 *
 * Implementation notes (new C-ish geometry, no new language patterns):
 *   Left normal in y-down screen coords is (ndy, -ndx) — for eastward travel
 *   (1,0) it points up. A quadratic bezier passes the chord midpoint at HALF
 *   the control-point offset, so the control point sits at 2*bow. jgraphics
 *   only has cubic curve_to; the exact quadratic->cubic elevation is
 *   c1 = s + 2/3(ctrl-s), c2 = e + 2/3(ctrl-e).
 */
static void gaff_draw_curve(t_jgraphics *g,
                             double x1, double y1,
                             double x2, double y2,
                             double bow, double node_r, double zoom,
                             double *label_x, double *label_y)
{
    double dx  = x2 - x1;
    double dy  = y2 - y1;
    double len = sqrt(dx * dx + dy * dy);
    if (len < 1.0) return;

    double ndx = dx / len;
    double ndy = dy / len;
    double lnx = ndy;       // left-of-travel normal (y-down coordinates)
    double lny = -ndx;

    // Control point: chord midpoint + 2*bow along the left normal */
    double cx = (x1 + x2) * 0.5 + lnx * 2.0 * bow;
    double cy = (y1 + y2) * 0.5 + lny * 2.0 * bow;

    // Trim the endpoints to the node rims along the local curve direction */
    double d1x = cx - x1, d1y = cy - y1;
    double l1  = sqrt(d1x * d1x + d1y * d1y);
    double d2x = x2 - cx, d2y = y2 - cy;
    double l2  = sqrt(d2x * d2x + d2y * d2y);
    if (l1 < 1.0 || l2 < 1.0) return;

    double sx = x1 + (d1x / l1) * node_r;
    double sy = y1 + (d1y / l1) * node_r;
    double ex = x2 - (d2x / l2) * node_r;
    double ey = y2 - (d2y / l2) * node_r;

    // Quadratic (s, ctrl, e) elevated to the cubic jgraphics understands */
    double c1x = sx + (cx - sx) * (2.0 / 3.0);
    double c1y = sy + (cy - sy) * (2.0 / 3.0);
    double c2x = ex + (cx - ex) * (2.0 / 3.0);
    double c2y = ey + (cy - ey) * (2.0 / 3.0);

    jgraphics_set_source_jrgba(g, &GAFF_COLOR_EDGE);
    jgraphics_set_line_width(g, 1.2 * zoom);
    jgraphics_move_to(g, sx, sy);
    jgraphics_curve_to(g, c1x, c1y, c2x, c2y, ex, ey);
    jgraphics_stroke(g);

    // Arrowhead: tangent at the endpoint runs from the control point to it */
    double alen  = GAFF_ARROW_LEN * zoom;
    double angle = atan2(ey - cy, ex - cx);
    jgraphics_move_to(g, ex, ey);
    jgraphics_line_to(g,
        ex - alen * cos(angle - GAFF_ARROW_ANGLE),
        ey - alen * sin(angle - GAFF_ARROW_ANGLE));
    jgraphics_move_to(g, ex, ey);
    jgraphics_line_to(g,
        ex - alen * cos(angle + GAFF_ARROW_ANGLE),
        ey - alen * sin(angle + GAFF_ARROW_ANGLE));
    jgraphics_stroke(g);

    // Label anchor: curve point at t=0.5 (= ¼s + ½ctrl + ¼e), nudged outward */
    if (label_x && label_y) {
        *label_x = 0.25 * sx + 0.5 * cx + 0.25 * ex + lnx * GAFF_WEIGHT_OFFSET * zoom;
        *label_y = 0.25 * sy + 0.5 * cy + 0.25 * ey + lny * GAFF_WEIGHT_OFFSET * zoom;
    }
}

/**
 * Draw a self-loop above a node: a small circle tangent to the top of the
 * node circle, with a downward arrowhead pointing back at the node.
 * All radii scale with zoom.
 */
static void gaff_draw_selfloop(t_jgraphics *g, double cx, double cy, double zoom)
{
    double node_r = GAFF_NODE_RADIUS * zoom;
    double loop_r = GAFF_LOOP_RADIUS * zoom;
    double alen   = GAFF_ARROW_LEN   * zoom;
    double loop_cy = cy - node_r - loop_r;

    jgraphics_set_source_jrgba(g, &GAFF_COLOR_EDGE);
    jgraphics_set_line_width(g, 1.2 * zoom);

    // Loop circle */
    jgraphics_arc(g, cx, loop_cy, loop_r, 0., 2.0 * M_PI);
    jgraphics_stroke(g);

    // Arrowhead at the bottom of the loop, pointing downward (angle = π/2) */
    double ex = cx;
    double ey = loop_cy + loop_r;
    double angle = M_PI * 0.5; // pointing downward */
    jgraphics_move_to(g, ex, ey);
    jgraphics_line_to(g,
        ex - alen * cos(angle - GAFF_ARROW_ANGLE),
        ey - alen * sin(angle - GAFF_ARROW_ANGLE));
    jgraphics_move_to(g, ex, ey);
    jgraphics_line_to(g,
        ex - alen * cos(angle + GAFF_ARROW_ANGLE),
        ey - alen * sin(angle + GAFF_ARROW_ANGLE));
    jgraphics_stroke(g);
}

/**
 * Draw a text label centered inside the node circle.
 * Uses jtextlayout for proper horizontal + vertical centering within the
 * bounding box (cx-r, cy-r, 2r, 2r). Font size scales with zoom.
 *
 * jfont and jtextlayout are heap-allocated and must be freed after use.
 * Java analogy: FontMetrics + Graphics2D.drawString with manual offset.
 */
static void gaff_draw_node_label(t_jgraphics *g, const char *text,
                                  double cx, double cy, double r, double zoom,
                                  t_jrgba *color)
{
    t_jfont *font = jfont_create("Arial",
                                  JGRAPHICS_FONT_SLANT_NORMAL,
                                  JGRAPHICS_FONT_WEIGHT_BOLD,
                                  10.0 * zoom);
    t_jtextlayout *tl = jtextlayout_create();
    jtextlayout_set(tl, text, font,
                    cx - r, cy - r, 2.0 * r, 2.0 * r,
                    JGRAPHICS_TEXT_JUSTIFICATION_HCENTERED |
                    JGRAPHICS_TEXT_JUSTIFICATION_VCENTERED, 0);
    jtextlayout_settextcolor(tl, color);
    jtextlayout_draw(tl, g);
    jtextlayout_destroy(tl);
    jfont_destroy(font);
}

/**
 * Draw an edge weight label at (mx, my).
 * Uses %g format: "0.7" not "0.700000", integers without decimal point.
 */
static void gaff_draw_weight_label(t_jgraphics *g, double mx, double my,
                                    double weight, double zoom)
{
    char text[16];
    // %g: compact notation — drops trailing zeros, no exponent for normal magnitudes */
    snprintf(text, sizeof(text), "%g", weight);

    t_jfont *font = jfont_create("Arial",
                                  JGRAPHICS_FONT_SLANT_NORMAL,
                                  JGRAPHICS_FONT_WEIGHT_NORMAL,
                                  8.5 * zoom);
    t_jtextlayout *tl = jtextlayout_create();
    jtextlayout_set(tl, text, font,
                    mx - 18.0 * zoom, my - 9.0 * zoom, 36.0 * zoom, 18.0 * zoom,
                    JGRAPHICS_TEXT_JUSTIFICATION_HCENTERED |
                    JGRAPHICS_TEXT_JUSTIFICATION_VCENTERED, 0);
    jtextlayout_settextcolor(tl, &GAFF_COLOR_TEXT_WEIGHT);
    jtextlayout_draw(tl, g);
    jtextlayout_destroy(tl);
    jfont_destroy(font);
}

/**
 * Draw a two-line placeholder message centered in the box.
 * Shown when no graph is attached, the named graph doesn't exist yet,
 * or the graph is empty.
 */
static void gaff_draw_placeholder(t_jgraphics *g, t_rect *rect,
                                   const char *line1, const char *line2)
{
    char text[128];
    snprintf(text, sizeof(text), "%s\n%s", line1, line2);

    t_jfont *font = jfont_create("Arial",
                                  JGRAPHICS_FONT_SLANT_ITALIC,
                                  JGRAPHICS_FONT_WEIGHT_NORMAL,
                                  11.0);
    t_jtextlayout *tl = jtextlayout_create();
    jtextlayout_set(tl, text, font,
                    0., 0., rect->width, rect->height,
                    JGRAPHICS_TEXT_JUSTIFICATION_HCENTERED |
                    JGRAPHICS_TEXT_JUSTIFICATION_VCENTERED, 0);
    jtextlayout_settextcolor(tl, &GAFF_COLOR_PLACEHOLDER);
    jtextlayout_draw(tl, g);
    jtextlayout_destroy(tl);
    jfont_destroy(font);
}


////////////////////////// paint

/**
 * paint — the main draw callback. Called by Max whenever the box needs
 * to be redrawn (on load, resize, jbox_redraw(), patcher scroll, etc.).
 *
 * Coordinate pipeline:
 *   world (stored, fixed scale) --*zoom--> +pan +boxcenter --> screen
 * The transform is applied manually (coordinates, radii, fonts, line widths
 * all multiplied by zoom) rather than through jgraphics_translate/scale,
 * because jtextlayout does not reliably honor the context matrix. The result
 * is identical to a geometric zoom.
 *
 * Layout is synced lazily here (layout_dirty / count mismatch), never
 * recomputed on a plain repaint.
 *
 * Draw order:
 *   1. Background fill
 *   2. Edges (straight, curved, self-loops + weight labels) — under the nodes
 *   3. Node circles (fill + border)
 *   4. Node ID labels
 *   5. Instance name + mode label (bottom-left corner, dim)
 *
 * Java analogy: @Override protected void paintComponent(Graphics g) { ... }
 */
void graf_affiche_paint(t_graf_affiche *x, t_object *patcherview)
{
    // Get drawing context and box bounds */
    t_jgraphics *g = (t_jgraphics *)patcherview_get_jgraphics(patcherview);
    t_rect rect;
    jbox_get_rect_for_view((t_object *)&x->box, patcherview, &rect);

    // --- background -------------------------------------------------------- */
    jgraphics_set_source_jrgba(g, &GAFF_COLOR_BG);
    jgraphics_rectangle(g, 0., 0., rect.width, rect.height);
    jgraphics_fill(g);

    // --- guard: no name assigned ------------------------------------------- */
    if (!x->graf_name) {
        gaff_draw_placeholder(g, &rect, "graf.affiche", "(no graph assigned)");
        return;
    }

    // --- guard: named instance not registered yet --------------------------- */
    t_graf *graph = graf_find(x->graf_name);
    if (!graph) {
        gaff_draw_placeholder(g, &rect, x->graf_name->s_name, "(not found)");
        goto draw_name_label;
    }

    // --- lazy layout sync (runs even for n==0, clearing stale positions) ---- */
    if (x->layout_dirty || x->layout_full || x->pos_count != graph->node_count)
        gaff_layout_sync(x, graph);

    // --- guard: empty graph ------------------------------------------------- */
    if (graph->node_count == 0) {
        gaff_draw_placeholder(g, &rect, x->graf_name->s_name, "(empty)");
        goto draw_name_label;
    }

    // --- deferred auto-fit (needs the box size, only known here) ------------ */
    if (x->needs_autofit) {
        gaff_autofit(x, rect.width, rect.height);
        x->needs_autofit = 0;
    }

    {
        long   n    = x->pos_count;
        double zoom = x->view_zoom;
        double vcx  = rect.width  * 0.5;
        double vcy  = rect.height * 0.5;
        double node_r = GAFF_NODE_RADIUS * zoom;
        long   i;

        // Screen coordinates, computed once per paint.
        /* Heap scratch — n is unbounded now, no stack arrays.
           Java analogy: double[] sx = new double[n]; but freed by hand. */
        double *sx = (double *)sysmem_newptr(n * sizeof(double));
        double *sy = (double *)sysmem_newptr(n * sizeof(double));
        if (!sx || !sy) {
            if (sx) sysmem_freeptr(sx);
            if (sy) sysmem_freeptr(sy);
            return;
        }
        for (i = 0; i < n; i++) {
            sx[i] = x->pos[i].wx * zoom + x->view_pan_x + vcx;
            sy[i] = x->pos[i].wy * zoom + x->view_pan_y + vcy;
        }

        // --- draw edges ----------------------------------------------------
        for (i = 0; i < n; i++) {
            t_graf_node *src = &graph->nodes[i];
            long j;

            for (j = 0; j < src->edge_count; j++) {
                // Find the index of the target node for its layout position.
                // Linear scan by symbol pointer equality — O(n) per edge.
                long ti = gaff_index_of(graph, src->edges_to[j]);
                if (ti < 0) continue; // dangling edge — target gone

                double weight  = src->edge_weights[j];
                double label_x = 0.0, label_y = 0.0;
                int    have_label = 0;

                if (i == ti) {
                    // Self-loop */
                    gaff_draw_selfloop(g, sx[i], sy[i], zoom);
                    continue;
                }

                long skip = gaff_edge_skip(x, i, ti, n);

                if (skip <= 0) {
                    // Adjacent-in-layout: straight, as before
                    gaff_draw_arrow(g, sx[i], sy[i], sx[ti], sy[ti], node_r, zoom);

                    if (weight != 0.0) {
                        double dx  = sx[ti] - sx[i];
                        double dy  = sy[ti] - sy[i];
                        double len = sqrt(dx * dx + dy * dy);
                        if (len > 0.1) {
                            double ndx = dx / len;
                            double ndy = dy / len;
                            // Midpoint of the visible shaft, offset to the side
                            double ssx = sx[i]  + ndx * node_r;
                            double ssy = sy[i]  + ndy * node_r;
                            double eex = sx[ti] - ndx * node_r;
                            double eey = sy[ti] - ndy * node_r;
                            label_x = (ssx + eex) * 0.5 + (-ndy) * GAFF_WEIGHT_OFFSET * zoom;
                            label_y = (ssy + eey) * 0.5 + ( ndx) * GAFF_WEIGHT_OFFSET * zoom;
                            have_label = 1;
                        }
                    }
                } else {
                    // Non-adjacent: bow left of travel, GAFF_CURVE_STEP world
                    /*   units per skipped node, capped at a fraction of the
                       edge length so short edges with big skips stay sane. */
                    double dx  = sx[ti] - sx[i];
                    double dy  = sy[ti] - sy[i];
                    double len = sqrt(dx * dx + dy * dy);
                    double bow = GAFF_CURVE_STEP * (double)skip * zoom;
                    double cap = GAFF_CURVE_MAX_FRAC * len;
                    if (bow > cap) bow = cap;

                    gaff_draw_curve(g, sx[i], sy[i], sx[ti], sy[ti],
                                    bow, node_r, zoom,
                                    &label_x, &label_y);
                    have_label = (weight != 0.0);
                }

                // Weight label — only when weight != 0.0 (avoids clutter on
                // unweighted graphs) and only above the legibility floor
                if (have_label && weight != 0.0 && zoom >= GAFF_MIN_WEIGHT_ZOOM)
                    gaff_draw_weight_label(g, label_x, label_y, weight, zoom);
            }
        }

        // --- draw nodes ---------------------------------------------------- 
        for (i = 0; i < n; i++) {
            t_graf_node *node = &graph->nodes[i];
            t_jrgba *fill = (graph->current == node->id)
                            ? &GAFF_COLOR_NODE_CURRENT
                            : &GAFF_COLOR_NODE;

            // Filled circle
            jgraphics_arc(g, sx[i], sy[i], node_r, 0., 2.0 * M_PI);
            jgraphics_set_source_jrgba(g, fill);
            jgraphics_fill(g);

            // Border (re-draw path since fill consumed it)
            jgraphics_arc(g, sx[i], sy[i], node_r, 0., 2.0 * M_PI);
            jgraphics_set_source_jrgba(g, &GAFF_COLOR_NODE_BORDER);
            jgraphics_set_line_width(g, 1.5 * zoom);
            jgraphics_stroke(g);

            // Node ID label — skipped when too small to read anyway
            if (node_r >= GAFF_MIN_LABEL_RADIUS)
                gaff_draw_node_label(g, node->id->s_name,
                                      sx[i], sy[i], node_r, zoom,
                                      &GAFF_COLOR_TEXT_NODE);
        }

        sysmem_freeptr(sx);
        sysmem_freeptr(sy);
    }

    // instance name + mode label (bottom-left corner, always drawn) ------
draw_name_label:
    if (x->graf_name) {
        char corner[160];
        // always-visible readout: instance, mode, current zoom
        snprintf(corner, sizeof(corner), "%s [%s] %.0f%%",
                 x->graf_name->s_name, gaff_mode_name(x->mode),
                 x->view_zoom * 100.0);

        t_jfont *font = jfont_create("Arial",
                                      JGRAPHICS_FONT_SLANT_NORMAL,
                                      JGRAPHICS_FONT_WEIGHT_NORMAL,
                                      9.0);
        t_jtextlayout *tl = jtextlayout_create();
        jtextlayout_set(tl, corner, font,
                        5., rect.height - 16., 220., 14.,
                        JGRAPHICS_TEXT_JUSTIFICATION_LEFT |
                        JGRAPHICS_TEXT_JUSTIFICATION_TOP, 0);
        jtextlayout_settextcolor(tl, &GAFF_COLOR_PLACEHOLDER);
        jtextlayout_draw(tl, g);
        jtextlayout_destroy(tl);
        jfont_destroy(font);
    }
}