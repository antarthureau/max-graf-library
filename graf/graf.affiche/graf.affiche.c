/**
 * @file graf.affiche.c
 * graf.affiche — real-time graph visualizer for the graf external family.
 * antoine hureau-parreira
 *
 * A jbox UI external: draws a named [graf] instance as a directed graph
 * inside a resizable patcher window. Nodes are circles, edges are directed
 * arrows, weights are labels. The current traversal position is highlighted.
 *
 * Layout: circular (nodes evenly spaced on a ring, starting at 12 o'clock).
 * Update: subscribes to the named [graf] instance and redraws automatically
 *         whenever graf calls object_notify("modified").
 *
 * Messages:
 *   bang              — force immediate redraw
 *   graph <name>      — switch to a different named [graf] instance
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


////////////////////////// defines

/* Visual geometry */
#define GAFF_NODE_RADIUS    18.0    /* pixel radius of each node circle */
#define GAFF_ARROW_LEN      10.0    /* arrowhead arm length in pixels */
#define GAFF_ARROW_ANGLE    0.42    /* arrowhead opening half-angle in radians (~24°) */
#define GAFF_WEIGHT_OFFSET  13.0    /* perpendicular offset for edge weight labels */
#define GAFF_LOOP_RADIUS    9.0     /* radius of self-loop circle */
#define GAFF_MAX_NODES      256     /* stack allocation cap for layout position arrays */

/* Default box size (pixels) */
#define GAFF_DEFAULT_WIDTH  400
#define GAFF_DEFAULT_HEIGHT 300


////////////////////////// colors (dark theme, RGBA in [0..1])

/* Static globals: initialized once, referenced by pointer in jgraphics calls.
   In C, static globals at file scope are zero-initialized, then assigned here.
   Java analogy: private static final Color fields. */

static t_jrgba GAFF_COLOR_BG           = {0.12, 0.12, 0.14, 1.0};  /* near-black background */
static t_jrgba GAFF_COLOR_NODE         = {0.24, 0.27, 0.32, 1.0};  /* default node fill */
static t_jrgba GAFF_COLOR_NODE_CURRENT = {0.18, 0.60, 0.38, 1.0};  /* current node: green */
static t_jrgba GAFF_COLOR_NODE_BORDER  = {0.48, 0.52, 0.60, 1.0};  /* node outline */
static t_jrgba GAFF_COLOR_EDGE         = {0.48, 0.52, 0.58, 1.0};  /* edge lines and arrows */
static t_jrgba GAFF_COLOR_TEXT_NODE    = {0.92, 0.92, 0.94, 1.0};  /* node ID label */
static t_jrgba GAFF_COLOR_TEXT_WEIGHT  = {0.82, 0.78, 0.38, 1.0};  /* weight label: warm yellow */
static t_jrgba GAFF_COLOR_PLACEHOLDER  = {0.34, 0.34, 0.38, 1.0};  /* "no graph" message */


////////////////////////// data structure

/**
 * graf.affiche object struct.
 *
 * IMPORTANT: t_jbox MUST be the first member. This is the jbox equivalent of
 * the t_object ob convention — Max's type system treats the first member as
 * the object header and casts between t_jbox* and t_graf_affiche* freely.
 *
 * Java analogy: extends JPanel { ... }
 * The JPanel state lives at the start of the object; our fields come after.
 */
typedef struct _graf_affiche {
    t_jbox      box;        /* MUST be first — jbox subclass header */
    t_symbol   *graf_name;  /* name of the [graf] instance we are watching */
} t_graf_affiche;


////////////////////////// function prototypes

void *graf_affiche_new(t_symbol *s, long argc, t_atom *argv);
void  graf_affiche_free(t_graf_affiche *x);
void  graf_affiche_assist(t_graf_affiche *x, void *b, long m, long a, char *s);

/* Paint is the draw callback — registered as "paint" A_CANT.
   A_CANT means "called by Max internally, not from a patcher" — like @Override. */
void  graf_affiche_paint(t_graf_affiche *x, t_object *patcherview);

/* notify receives events from subscribed objects (our watched graf instance)
   and from jbox itself (attribute changes, resize, etc.). */
t_max_err graf_affiche_notify(t_graf_affiche *x, t_symbol *s, t_symbol *msg,
                               void *sender, void *data);

void  graf_affiche_bang(t_graf_affiche *x);
void  graf_affiche_update(t_graf_affiche *x, t_symbol *name);

/* Internal drawing helpers — static so they are private to this translation unit */
static void gaff_draw_arrow(t_jgraphics *g, double x1, double y1,
                             double x2, double y2);
static void gaff_draw_selfloop(t_jgraphics *g, double cx, double cy);
static void gaff_draw_node_label(t_jgraphics *g, const char *text,
                                  double cx, double cy, double r,
                                  t_jrgba *color);
static void gaff_draw_weight_label(t_jgraphics *g, double mx, double my,
                                    double weight);
static void gaff_draw_placeholder(t_jgraphics *g, t_rect *rect,
                                   const char *line1, const char *line2);

/* Global class pointer */
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

    /* Required for jbox — enables the dictionary-based constructor protocol */
    c->c_flags |= CLASS_FLAG_NEWDICTIONARY;

    /* Register as a jbox subclass. 0 = no extra attributes (we handle colors
       ourselves; use JBOX_COLOR here later if you want inspector color pickers) */
    jbox_initclass(c, 0);

    /* Internal callbacks — A_CANT means Max calls these, not the user */
    class_addmethod(c, (method)graf_affiche_paint,  "paint",  A_CANT, 0);
    class_addmethod(c, (method)graf_affiche_notify, "notify", A_CANT, 0);
    class_addmethod(c, (method)graf_affiche_assist, "assist", A_CANT, 0);

    /* User-facing messages */
    class_addmethod(c, (method)graf_affiche_bang,   "bang",   0);
    class_addmethod(c, (method)graf_affiche_update,  "update",  A_SYM, 0);

    /* Default box size — shows up in the patcher when the user creates the object */
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
 *      (the patcher encodes box position/size here)
 *   2. object_alloc — allocate our struct
 *   3. jbox_new — initialize the jbox portion of the struct
 *   4. set b_firstin — tells Max which object owns the inlets
 *   5. parse user arguments
 *   6. attr_dictionary_process — apply saved attribute values (size, position)
 *   7. jbox_ready — finalize and trigger first paint
 *
 * Java analogy:
 *   super(layout);          // jbox_new
 *   this.grafName = args[0]; // parse user args
 *   model.addObserver(this); // object_subscribe
 *   repaint();               // jbox_ready
 */
void *graf_affiche_new(t_symbol *s, long argc, t_atom *argv)
{
    t_graf_affiche *x = NULL;
    t_dictionary   *d = NULL;
    long i;

    /* Step 1: extract patcher dictionary — MUST come first for jbox objects */
    if (!(d = object_dictionaryarg(argc, argv)))
        return NULL;

    /* Step 2: allocate */
    x = (t_graf_affiche *)object_alloc(graf_affiche_class);
    if (!x) return NULL;

    /* Step 3: initialize jbox portion.
       Box flags control resize behaviour and drawing order:
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

    /* Step 4: first inlet belongs to us */
    x->box.b_firstin = (t_object *)x;

    /* Step 5: parse user arguments.
       The first symbol argument that does not start with '@' is the graf name.
       ('@' prefix marks attribute arguments like @patching_rect.) */
    /* Read instance name: try argv first, then the patcher dictionary "args"
       key. With CLASS_FLAG_NEWDICTIONARY, Max encodes positional arguments
       inside the dictionary rather than in argv directly. */
    x->graf_name = NULL;
    for (i = 0; i < argc; i++) {
        if (atom_gettype(argv + i) == A_SYM) {
            t_symbol *sym = atom_getsym(argv + i);
            if (sym && sym->s_name[0] != '@') { x->graf_name = sym; break; }
        }
    }
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
    }

    /* Subscribe to the named graf instance.
       object_subscribe works by name — the target does not need to exist yet.
       When a [graf my_graph] is created or registered later, we will start
       receiving its notifications automatically.
       Java analogy: model.addObserver(this) — we register as an observer
       on the shared model object identified by name. */
    if (x->graf_name) {
        object_subscribe(CLASS_BOX, x->graf_name,
                         gensym("graf.affiche"), (t_object *)x);
        post("graf.affiche: watching '%s'", x->graf_name->s_name);
    }

    /* Step 6: apply saved attribute values from the patcher dictionary */
    attr_dictionary_process(x, d);

    /* Step 7: finalize — triggers the first paint */
    jbox_ready(&x->box);

    return x;
}

/**
 * Destructor — unsubscribe from the watched graf and free jbox resources.
 * jbox_free handles all jbox-internal cleanup (inlets, outlets, graphics).
 * Java analogy: model.removeObserver(this) + super.finalize()
 */
void graf_affiche_free(t_graf_affiche *x)
{
    if (x->graf_name) {
        object_unsubscribe(CLASS_BOX, x->graf_name,
                           gensym("graf.affiche"), (t_object *)x);
    }
    jbox_free(&x->box);
}

void graf_affiche_assist(t_graf_affiche *x, void *b, long m, long a, char *s)
{
    if (m == ASSIST_INLET)
        sprintf(s, "bang: force redraw | graph <name>: switch watched instance");
}


////////////////////////// notification and message handlers

/**
 * notify — called when a subscribed object sends a notification,
 * and also when jbox attributes (size, position, color) are modified.
 *
 * We redraw on:
 *   "modified"     — graf state changed (node/edge added/removed, goto, next, clear)
 *   "attr_modified" — one of our own jbox attributes changed (resize, etc.)
 *
 * jbox_redraw does NOT redraw immediately — it marks the box dirty and queues
 * a repaint in Max's event loop. Multiple calls before the next event cycle
 * are coalesced into a single paint. This means calling it many times during
 * a bulk load operation (e.g. read) is harmless.
 *
 * Java analogy: @Override public void update(Observable o, Object arg) { repaint(); }
 */
t_max_err graf_affiche_notify(t_graf_affiche *x, t_symbol *s, t_symbol *msg,
                               void *sender, void *data)
{
    if (msg == gensym("modified") || msg == gensym("attr_modified")) {
        jbox_redraw(&x->box);
    }
    return MAX_ERR_NONE;
}

/**
 * bang — force an immediate redraw.
 * Useful if the subscription mechanism misses a state change,
 * or to manually refresh after live-coding a graph.
 */
void graf_affiche_bang(t_graf_affiche *x)
{
    jbox_redraw(&x->box);
}

/**
 * graph <name> — switch to watching a different named [graf] instance.
 * Unsubscribes from the current instance first, then subscribes to the new one.
 */
void graf_affiche_update(t_graf_affiche *x, t_symbol *name)
{
    if (x->graf_name) {
        object_unsubscribe(CLASS_BOX, x->graf_name,
                           gensym("graf.affiche"), (t_object *)x);
    }

    x->graf_name = name;

    if (name && name != gensym("")) {
        object_subscribe(CLASS_BOX, name, gensym("graf.affiche"), (t_object *)x);
        post("graf.affiche: now watching '%s'", name->s_name);
    }

    jbox_redraw(&x->box);
}


////////////////////////// internal drawing helpers

/**
 * Draw a directed edge from (x1,y1) to (x2,y2) with an arrowhead.
 * The line is offset inward by GAFF_NODE_RADIUS so it starts/ends at the
 * circumference of each node circle rather than at the centers.
 */
static void gaff_draw_arrow(t_jgraphics *g,
                             double x1, double y1,
                             double x2, double y2)
{
    double dx  = x2 - x1;
    double dy  = y2 - y1;
    double len = sqrt(dx * dx + dy * dy);
    if (len < 1.0) return;

    double ndx = dx / len;  /* normalized direction */
    double ndy = dy / len;

    /* Start and end points, pulled back from each center by the node radius */
    double sx = x1 + ndx * GAFF_NODE_RADIUS;
    double sy = y1 + ndy * GAFF_NODE_RADIUS;
    double ex = x2 - ndx * GAFF_NODE_RADIUS;
    double ey = y2 - ndy * GAFF_NODE_RADIUS;

    jgraphics_set_source_jrgba(g, &GAFF_COLOR_EDGE);
    jgraphics_set_line_width(g, 1.2);

    /* Edge shaft */
    jgraphics_move_to(g, sx, sy);
    jgraphics_line_to(g, ex, ey);
    jgraphics_stroke(g);

    /* Arrowhead: two lines diverging from the endpoint at ±GAFF_ARROW_ANGLE */
    double angle = atan2(dy, dx);
    jgraphics_move_to(g, ex, ey);
    jgraphics_line_to(g,
        ex - GAFF_ARROW_LEN * cos(angle - GAFF_ARROW_ANGLE),
        ey - GAFF_ARROW_LEN * sin(angle - GAFF_ARROW_ANGLE));
    jgraphics_move_to(g, ex, ey);
    jgraphics_line_to(g,
        ex - GAFF_ARROW_LEN * cos(angle + GAFF_ARROW_ANGLE),
        ey - GAFF_ARROW_LEN * sin(angle + GAFF_ARROW_ANGLE));
    jgraphics_stroke(g);
}

/**
 * Draw a self-loop above a node: a small circle tangent to the top of the
 * node circle, with a downward arrowhead pointing back at the node.
 */
static void gaff_draw_selfloop(t_jgraphics *g, double cx, double cy)
{
    double loop_cy = cy - GAFF_NODE_RADIUS - GAFF_LOOP_RADIUS;

    jgraphics_set_source_jrgba(g, &GAFF_COLOR_EDGE);
    jgraphics_set_line_width(g, 1.2);

    /* Loop circle */
    jgraphics_arc(g, cx, loop_cy, GAFF_LOOP_RADIUS, 0., 2.0 * M_PI);
    jgraphics_stroke(g);

    /* Arrowhead at the bottom of the loop, pointing downward (angle = π/2) */
    double ex = cx;
    double ey = loop_cy + GAFF_LOOP_RADIUS;
    double angle = M_PI * 0.5; /* pointing downward */
    jgraphics_move_to(g, ex, ey);
    jgraphics_line_to(g,
        ex - GAFF_ARROW_LEN * cos(angle - GAFF_ARROW_ANGLE),
        ey - GAFF_ARROW_LEN * sin(angle - GAFF_ARROW_ANGLE));
    jgraphics_move_to(g, ex, ey);
    jgraphics_line_to(g,
        ex - GAFF_ARROW_LEN * cos(angle + GAFF_ARROW_ANGLE),
        ey - GAFF_ARROW_LEN * sin(angle + GAFF_ARROW_ANGLE));
    jgraphics_stroke(g);
}

/**
 * Draw a text label centered inside the node circle.
 * Uses jtextlayout for proper horizontal + vertical centering within the
 * bounding box (cx-r, cy-r, 2r, 2r).
 *
 * jfont and jtextlayout are heap-allocated and must be freed after use.
 * Java analogy: FontMetrics + Graphics2D.drawString with manual offset.
 */
static void gaff_draw_node_label(t_jgraphics *g, const char *text,
                                  double cx, double cy, double r,
                                  t_jrgba *color)
{
    t_jfont *font = jfont_create("Arial",
                                  JGRAPHICS_FONT_SLANT_NORMAL,
                                  JGRAPHICS_FONT_WEIGHT_BOLD,
                                  10.0);
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
                                    double weight)
{
    char text[16];
    /* %g: compact notation — drops trailing zeros, no exponent for normal magnitudes */
    snprintf(text, sizeof(text), "%g", weight);

    t_jfont *font = jfont_create("Arial",
                                  JGRAPHICS_FONT_SLANT_NORMAL,
                                  JGRAPHICS_FONT_WEIGHT_NORMAL,
                                  8.5);
    t_jtextlayout *tl = jtextlayout_create();
    jtextlayout_set(tl, text, font,
                    mx - 18.0, my - 9.0, 36.0, 18.0,
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
 * patcherview is the view object for the current patcher window.
 * patcherview_get_jgraphics() gives us the drawing context.
 *
 * Coordinate system: (0, 0) is the top-left of the box.
 * All drawing is in local pixel coordinates (rect.width × rect.height).
 *
 * Draw order:
 *   1. Background fill
 *   2. Edges (arrows + weight labels)   — drawn first so nodes sit on top
 *   3. Node circles (fill + border)
 *   4. Node ID labels
 *   5. Instance name label (bottom-left corner, dim)
 *
 * Layout: nodes are arranged in a circle. For n nodes, the k-th node sits at
 *   angle = (2π * k / n) − π/2    (−π/2 starts at 12 o'clock)
 *
 * Java analogy: @Override protected void paintComponent(Graphics g) { ... }
 */
void graf_affiche_paint(t_graf_affiche *x, t_object *patcherview)
{
    /* Get drawing context and box bounds */
    t_jgraphics *g = (t_jgraphics *)patcherview_get_jgraphics(patcherview);
    t_rect rect;
    jbox_get_rect_for_view((t_object *)&x->box, patcherview, &rect);

    /* --- background -------------------------------------------------------- */
    jgraphics_set_source_jrgba(g, &GAFF_COLOR_BG);
    jgraphics_rectangle(g, 0., 0., rect.width, rect.height);
    jgraphics_fill(g);

    /* --- guard: no name assigned ------------------------------------------- */
    if (!x->graf_name) {
        gaff_draw_placeholder(g, &rect, "graf.affiche", "(no graph assigned)");
        return;
    }

    /* --- guard: named instance not registered yet --------------------------- */
    t_graf *graph = graf_find(x->graf_name);
    if (!graph) {
        gaff_draw_placeholder(g, &rect, x->graf_name->s_name, "(not found)");
        goto draw_name_label;
    }

    /* --- guard: empty graph ------------------------------------------------- */
    if (graph->node_count == 0) {
        gaff_draw_placeholder(g, &rect, x->graf_name->s_name, "(empty)");
        goto draw_name_label;
    }

    {
        /* Clamp to stack array capacity */
        long n = graph->node_count;
        if (n > GAFF_MAX_NODES) n = GAFF_MAX_NODES;

        /* --- circular layout ----------------------------------------------- */
        /* Center and layout radius, with margin so nodes don't clip the edge */
        double cx       = rect.width  * 0.5;
        double cy       = rect.height * 0.5;
        double margin   = GAFF_NODE_RADIUS + 6.0;
        double layout_r = (cx < cy ? cx : cy) - margin;
        if (layout_r < GAFF_NODE_RADIUS + 2.0)
            layout_r = GAFF_NODE_RADIUS + 2.0;

        /* Stack-allocated position arrays — fine for <= 256 nodes.
           Java analogy: double[] px = new double[n]; — stack version. */
        double px[GAFF_MAX_NODES];
        double py[GAFF_MAX_NODES];
        long i;

        if (n == 1) {
            /* Special case: single node centered */
            px[0] = cx;
            py[0] = cy;
        } else {
            for (i = 0; i < n; i++) {
                /* Start at 12 o'clock (−π/2) and go clockwise */
                double angle = (2.0 * M_PI * i / (double)n) - M_PI * 0.5;
                px[i] = cx + layout_r * cos(angle);
                py[i] = cy + layout_r * sin(angle);
            }
        }

        /* --- draw edges ---------------------------------------------------- */
        for (i = 0; i < n; i++) {
            t_graf_node *src = &graph->nodes[i];
            long j;

            for (j = 0; j < src->edge_count; j++) {
                /* Find the index of the target node for its layout position.
                   Linear scan by symbol pointer equality — O(n) per edge. */
                long ti = -1, k;
                for (k = 0; k < n; k++) {
                    if (graph->nodes[k].id == src->edges_to[j]) {
                        ti = k;
                        break;
                    }
                }
                if (ti < 0) continue; /* target not in visible range */

                if (i == ti) {
                    /* Self-loop */
                    gaff_draw_selfloop(g, px[i], py[i]);
                } else {
                    gaff_draw_arrow(g, px[i], py[i], px[ti], py[ti]);

                    /* Weight label — only when weight != 0.0 (avoids clutter
                       on unweighted graphs where all weights are 0) */
                    if (src->edge_weights[j] != 0.0) {
                        double dx  = px[ti] - px[i];
                        double dy  = py[ti] - py[i];
                        double len = sqrt(dx * dx + dy * dy);
                        if (len > 0.1) {
                            double ndx = dx / len;
                            double ndy = dy / len;
                            /* Midpoint of the visible shaft (between circle edges) */
                            double sx  = px[i] + ndx * GAFF_NODE_RADIUS;
                            double sy  = py[i] + ndy * GAFF_NODE_RADIUS;
                            double ex  = px[ti] - ndx * GAFF_NODE_RADIUS;
                            double ey  = py[ti] - ndy * GAFF_NODE_RADIUS;
                            double mx  = (sx + ex) * 0.5 + (-ndy) * GAFF_WEIGHT_OFFSET;
                            double my  = (sy + ey) * 0.5 + ( ndx) * GAFF_WEIGHT_OFFSET;
                            gaff_draw_weight_label(g, mx, my, src->edge_weights[j]);
                        }
                    }
                }
            }
        }

        /* --- draw nodes ---------------------------------------------------- */
        for (i = 0; i < n; i++) {
            t_graf_node *node = &graph->nodes[i];
            t_jrgba *fill = (graph->current == node->id)
                            ? &GAFF_COLOR_NODE_CURRENT
                            : &GAFF_COLOR_NODE;

            /* Filled circle */
            jgraphics_arc(g, px[i], py[i], GAFF_NODE_RADIUS, 0., 2.0 * M_PI);
            jgraphics_set_source_jrgba(g, fill);
            jgraphics_fill(g);

            /* Border (re-draw path since fill consumed it) */
            jgraphics_arc(g, px[i], py[i], GAFF_NODE_RADIUS, 0., 2.0 * M_PI);
            jgraphics_set_source_jrgba(g, &GAFF_COLOR_NODE_BORDER);
            jgraphics_set_line_width(g, 1.5);
            jgraphics_stroke(g);

            /* Node ID label centered inside the circle */
            gaff_draw_node_label(g, node->id->s_name,
                                  px[i], py[i], GAFF_NODE_RADIUS,
                                  &GAFF_COLOR_TEXT_NODE);
        }
    }

    /* --- instance name label (bottom-left corner, always drawn) ------------ */
draw_name_label:
    if (x->graf_name) {
        t_jfont *font = jfont_create("Arial",
                                      JGRAPHICS_FONT_SLANT_NORMAL,
                                      JGRAPHICS_FONT_WEIGHT_NORMAL,
                                      9.0);
        t_jtextlayout *tl = jtextlayout_create();
        jtextlayout_set(tl, x->graf_name->s_name, font,
                        5., rect.height - 16., 150., 14.,
                        JGRAPHICS_TEXT_JUSTIFICATION_LEFT |
                        JGRAPHICS_TEXT_JUSTIFICATION_TOP, 0);
        jtextlayout_settextcolor(tl, &GAFF_COLOR_PLACEHOLDER);
        jtextlayout_draw(tl, g);
        jtextlayout_destroy(tl);
        jfont_destroy(font);
    }
}