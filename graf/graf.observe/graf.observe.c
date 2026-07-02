/**
 * @file graf_observe.c
 * graf_observe.c — Markov transition observer writing into named [graf] instances.
 * antoine hureau-parreira
 *
 * Part of the graf external family for Max/MSP.
 * Listens to a stream of symbols (or numbers) and records the observed
 * transitions as edge weights in a [graf] object identified by name.
 * Weights are RAW COUNTS, not probabilities — observing is additive:
 * record more material later (or in a later session, after read) and the
 * counts simply keep growing.
 *
 * -------------------------------------------------------------------------
 * USAGE
 *   [graf.observe my_graph]          order 1 (default)
 *   [graf.observe my_graph 2]        order 2 (transitions between 2-grams)
 *
 * MESSAGES IN
 *   record <v> [<v> ...]   record one or more values in sequence. Symbols
 *                          are used as node ids directly; ints/floats are
 *                          stringified ("60", "0.5") — handy for MIDI pitches.
 *   order <n>              set the Markov order (1..8). Clears the context
 *                          window. Nodes already observed at another order
 *                          remain in the graph untouched.
 *   forget                 clear the context window WITHOUT touching the
 *                          graph — use at phrase boundaries so the last note
 *                          of one phrase doesn't link to the first of the next.
 *   normalize [prob|cost]  DESTRUCTIVE: convert each node's outgoing counts
 *                          in the target graf. prob (default): weights become
 *                          probabilities summing to 1 per node — ready for
 *                          [graf.traverse] mode random. cost: weights become
 *                          -log(p) — ready for mode dijkstra, where the
 *                          shortest path is then the MOST PROBABLE path.
 *                          Counts are destroyed; recording afterwards mixes
 *                          scales (a warning is posted).
 *   bang                   post observer state (order, context) to the console.
 *
 * OUTLETS
 *   0 (left)  — transition update after each recorded transition:
 *               "<from> <to> <count>" (message; selector = from id)
 *   1 (right) — newly discovered node id (fires only when a node is created)
 *
 * -------------------------------------------------------------------------
 * MARKOV ORDER AND NODE IDENTITY
 *
 * Order 1: node ids are the recorded symbols themselves. observing writes
 * into exactly the same namespace as hand-built graphs — you can record on
 * top of a graph you drew by hand, and the counts merge.
 *
 * Order k > 1: the Markov state is the last k symbols, so nodes are k-grams
 * with composite ids joined by '|' (e.g. recording a b c at order 2 creates
 * nodes "a|b" and "b|c" and the edge between them). This keeps the graph a
 * plain first-order graph structurally — the memory lives in the node ids,
 * not in the traversal. [graf.traverse] walks k-gram graphs unchanged.
 *
 * -------------------------------------------------------------------------
 * WEIGHT SEMANTICS BRIDGE (why 'normalize cost' exists)
 *
 * In the weighted random walk, weight = likelihood (higher = more probable).
 * In Dijkstra, weight = cost (lower = better). Storing observed weights as
 * -log(p) converts products of probabilities into sums of costs:
 *     argmax  p1*p2*...*pn  ==  argmin  -log(p1) + ... + -log(pn)
 * so Dijkstra's shortest path over 'normalize cost' weights is the most
 * probable path through the observed model.
 *
 * -------------------------------------------------------------------------
 * C / JAVA MAPPING
 *
 * The observer parallels a MarkovCounter class in Java:
 *
 *   class MarkovCounter {
 *       Deque<String> window;              // sliding context, size = order
 *       Map<String, Map<String, Double>> counts;  // lives in t_graf here
 *       void record(String s) { ... }
 *   }
 *
 * except the count table is not owned by this object — it is written by
 * reference into a separate t_graf found by name at message time (service
 * locator pattern), exactly like graf.traverse reads it.
 *
 * The sliding window is a plain C array shifted by hand (memmove-style
 * loop) rather than a Deque — for order <= 8 the shift is trivially cheap.
 */

#include "ext.h"
#include "ext_obex.h"
#include "graf.h"           // t_graf, graf_find(), graf_ensure_node(), graf_increment_edge()
#include <math.h>           // log() — normalize cost
#include <string.h>
#include <stdio.h>          // snprintf


//////////////////////////////////////////////////////////////////////////
// Constants

#define GRAF_OBSERVE_MAX_ORDER    8       // sanity ceiling for the Markov order
#define GRAF_OBSERVE_KEY_LEN      512     // max length of a composite k-gram id
#define GRAF_OBSERVE_SEP          "|"     // k-gram id separator (CSV-safe: no comma/space)
#define GRAF_OBSERVE_MIN_PROB     1e-9    // clamp for -log(p) so cost stays finite


//////////////////////////////////////////////////////////////////////////
// Object struct

/**
 * t_graf_observe — state for one graf.observe instance.
 *
 * The only state the observer itself owns is the sliding context window;
 * all observed data lives in the target t_graf. This means the observer is
 * fully disposable — delete and recreate it and nothing observed is lost.
 */
typedef struct _graf_observe {
    t_object    ob;

    void       *outlet_trans;       // left outlet (created 2nd): transition updates
    void       *outlet_newnode;     // right outlet (created 1st): newly created node ids

    t_symbol   *graf_name;          // name of the [graf] instance to write into
    long        order;              // Markov order (state = last `order` symbols)

    t_symbol  **window;             // sliding context window, capacity = order
    long        window_count;       // symbols currently in the window (<= order)
} t_graf_observe;


//////////////////////////////////////////////////////////////////////////
// Class variable

static t_class *s_graf_observe_class = NULL;


//////////////////////////////////////////////////////////////////////////
// Prototypes

/* lifecycle */
void *graf_observe_new(t_symbol *s, long argc, t_atom *argv);
void  graf_observe_free(t_graf_observe *x);
void  graf_observe_assist(t_graf_observe *x, void *b, long m, long a, char *s);

/* graf lookup */
t_graf *graf_observe_get_graf(t_graf_observe *x);

/* internal helpers */
static t_symbol *graf_observe_atom_to_sym(t_graf_observe *x, const t_atom *a);
static t_symbol *graf_observe_window_key(t_graf_observe *x, char *buf, long buflen);
static void      graf_observe_record_one(t_graf_observe *x, t_graf *g, t_symbol *id);

/* message handlers */
void graf_observe_record(t_graf_observe *x, t_symbol *s, long argc, t_atom *argv);
void graf_observe_order(t_graf_observe *x, long n);
void graf_observe_forget(t_graf_observe *x);
void graf_observe_normalize(t_graf_observe *x, t_symbol *mode);
void graf_observe_bang(t_graf_observe *x);


//////////////////////////////////////////////////////////////////////////
// ext_main — class registration

void ext_main(void *r)
{
    t_class *c = class_new("graf.observe",
                           (method)graf_observe_new,
                           (method)graf_observe_free,
                           sizeof(t_graf_observe),
                           NULL,
                           A_GIMME,     // new() receives graf name [+ optional order]
                           0);

    class_addmethod(c, (method)graf_observe_assist,    "assist",    A_CANT,   0);
    class_addmethod(c, (method)graf_observe_bang,      "bang",      0);
    class_addmethod(c, (method)graf_observe_record,    "record",    A_GIMME,  0);
    class_addmethod(c, (method)graf_observe_order,     "order",     A_LONG,   0);
    class_addmethod(c, (method)graf_observe_forget,    "forget",    0);

    /* A_DEFSYM: optional symbol — gensym("") when omitted, like an
       Optional<String> defaulting to empty. */
    class_addmethod(c, (method)graf_observe_normalize, "normalize", A_DEFSYM, 0);

    class_register(CLASS_BOX, c);
    s_graf_observe_class = c;

    post("graf.observe: loaded");
}


//////////////////////////////////////////////////////////////////////////
// Object lifecycle

/**
 * graf_observe_new — create a new graf.observe instance.
 *
 * Arguments: <graf name> [<order>]
 * The [graf] does not need to exist yet — it is looked up at message time,
 * same lazy-lookup pattern as graf.traverse.
 *
 * Outlet ordering: Max creates outlets right-to-left visually.
 * outlet_newnode is created first (rightmost), outlet_trans second (left).
 */
void *graf_observe_new(t_symbol *s, long argc, t_atom *argv)
{
    t_graf_observe *x = (t_graf_observe *)object_alloc(s_graf_observe_class);
    if (!x) return NULL;

    if (argc < 1 || atom_gettype(argv) != A_SYM) {
        object_error((t_object *)x,
                     "graf.observe: requires a graf name as argument. "
                     "Usage: [graf.observe my_graph] or [graf.observe my_graph 2]");
        object_free(x);
        return NULL;
    }
    x->graf_name = atom_getsym(argv);

    /* optional second argument: Markov order */
    x->order = 1;
    if (argc >= 2 && atom_gettype(argv + 1) == A_LONG) {
        long n = atom_getlong(argv + 1);
        if (n < 1 || n > GRAF_OBSERVE_MAX_ORDER) {
            object_warn((t_object *)x,
                        "graf.observe: order %ld out of range (1..%d) — using 1",
                        n, GRAF_OBSERVE_MAX_ORDER);
        } else {
            x->order = n;
        }
    }

    x->window = (t_symbol **)sysmem_newptr(x->order * sizeof(t_symbol *));
    if (!x->window) {
        object_error((t_object *)x, "graf.observe: out of memory");
        object_free(x);
        return NULL;
    }
    x->window_count = 0;

    x->outlet_newnode = outlet_new((t_object *)x, NULL);   /* right */
    x->outlet_trans   = outlet_new((t_object *)x, NULL);   /* left  */

    return x;
}

/**
 * graf_observe_free — release the context window.
 * The target graf is NOT owned by this object — never freed here.
 */
void graf_observe_free(t_graf_observe *x)
{
    if (x->window) sysmem_freeptr(x->window);
}

/**
 * graf_observe_assist — inlet/outlet hover tooltips.
 */
void graf_observe_assist(t_graf_observe *x, void *b, long m, long a, char *s)
{
    if (m == ASSIST_INLET) {
        snprintf(s, 256,
                 "record <values...> / order <n> / forget / normalize [prob|cost]");
    } else {
        if (a == 0)
            snprintf(s, 256, "transition update: <from> <to> <count>");
        else
            snprintf(s, 256, "newly discovered node id");
    }
}


//////////////////////////////////////////////////////////////////////////
// graf lookup

/**
 * Resolve the named graf at message time.
 * Posts an error and returns NULL if it doesn't exist (yet).
 */
t_graf *graf_observe_get_graf(t_graf_observe *x)
{
    t_graf *g = graf_find(x->graf_name);
    if (!g) {
        object_error((t_object *)x,
                     "graf.observe: no graf named '%s' found",
                     x->graf_name->s_name);
    }
    return g;
}


//////////////////////////////////////////////////////////////////////////
// Internal helpers

/**
 * Convert one incoming atom to an interned node-id symbol.
 *
 * Symbols pass through. Numbers are rendered with the same formats as
 * graf.c's CSV writer ("%ld" / "%.10g") so a pitch recorded as the int 60
 * and a node written to CSV as 60 intern to the SAME symbol — pointer
 * equality holds across record and read/write round-trips.
 *
 * Returns NULL for unhandled atom types (with a warning).
 */
static t_symbol *graf_observe_atom_to_sym(t_graf_observe *x, const t_atom *a)
{
    char buf[64];

    switch (atom_gettype(a)) {
        case A_SYM:
            return atom_getsym(a);
        case A_LONG:
            snprintf(buf, sizeof(buf), "%ld", (long)atom_getlong(a));
            return gensym(buf);
        case A_FLOAT:
            snprintf(buf, sizeof(buf), "%.10g", (double)atom_getfloat(a));
            return gensym(buf);
        default:
            object_warn((t_object *)x,
                        "record: skipping atom of unsupported type");
            return NULL;
    }
}

/**
 * Build the node id for the current (full) context window.
 *
 * Order 1: the window's single symbol IS the id — returned directly,
 * no string work, no new symbol interned.
 *
 * Order k: symbols joined with GRAF_OBSERVE_SEP into buf, then interned.
 * Truncation (id longer than GRAF_OBSERVE_KEY_LEN) is warned once per call —
 * with 8 symbols max this only happens with pathological id lengths.
 *
 * Java analogy: String.join("|", window).intern()
 */
static t_symbol *graf_observe_window_key(t_graf_observe *x, char *buf, long buflen)
{
    long i, pos = 0;

    if (x->order == 1)
        return x->window[0];

    buf[0] = '\0';
    for (i = 0; i < x->order; i++) {
        int written = snprintf(buf + pos, (size_t)(buflen - pos), "%s%s",
                               (i > 0) ? GRAF_OBSERVE_SEP : "",
                               x->window[i]->s_name);
        if (written < 0 || pos + written >= buflen) {
            object_warn((t_object *)x,
                        "record: composite node id truncated (> %d chars)",
                        GRAF_OBSERVE_KEY_LEN);
            pos = buflen - 1;
            break;
        }
        pos += written;
    }
    buf[pos] = '\0';
    return gensym(buf);
}

/**
 * Record a single value: the core observing step.
 *
 * Two phases of life:
 *
 * 1. Window not yet full (fewer than `order` symbols seen since start /
 *    forget / order change): just accumulate. When the window fills for
 *    the first time, the initial state node is created — but no transition
 *    yet (a transition needs two consecutive full states).
 *
 * 2. Window full: we have a previous state. Slide the window (drop oldest,
 *    append newest), giving the new state. Then:
 *      - ensure both state nodes exist (right outlet fires for a new one)
 *      - increment the edge prev -> new by 1.0
 *      - left outlet reports "<from> <to> <count>"
 *      - object_notify on the graf so graf.affiche redraws live
 *
 * Outlet firing order follows the Max right-to-left convention:
 * new-node (right) fires before the transition update (left).
 */
static void graf_observe_record_one(t_graf_observe *x, t_graf *g, t_symbol *id)
{
    char      keybuf[GRAF_OBSERVE_KEY_LEN];
    t_symbol *prev_key = NULL;
    t_symbol *new_key  = NULL;
    long      created  = 0;
    long      i;

    if (x->window_count < x->order) {
        /* phase 1 — filling the initial context */
        x->window[x->window_count++] = id;

        if (x->window_count == x->order) {
            /* window just became full: create the initial state node */
            new_key = graf_observe_window_key(x, keybuf, GRAF_OBSERVE_KEY_LEN);
            if (!graf_ensure_node(g, new_key, &created)) {
                object_error((t_object *)x, "record: out of memory");
                return;
            }
            if (created) {
                outlet_anything(x->outlet_newnode, new_key, 0, NULL);
                object_notify((t_object *)g, gensym("modified"), NULL);
            }
        }
        return;
    }

    /* phase 2 — full window: previous state -> new state transition */
    prev_key = graf_observe_window_key(x, keybuf, GRAF_OBSERVE_KEY_LEN);

    /* slide: drop oldest, append newest */
    for (i = 1; i < x->order; i++)
        x->window[i - 1] = x->window[i];
    x->window[x->order - 1] = id;

    {
        char keybuf2[GRAF_OBSERVE_KEY_LEN];
        new_key = graf_observe_window_key(x, keybuf2, GRAF_OBSERVE_KEY_LEN);
    }

    /* ensure both endpoints exist. prev normally already does (it was
       created when the window last slid) — ensured defensively anyway,
       e.g. after the user sent 'clear' to the graf mid-stream. */
    if (!graf_ensure_node(g, prev_key, NULL)) {
        object_error((t_object *)x, "record: out of memory");
        return;
    }
    if (!graf_ensure_node(g, new_key, &created)) {
        object_error((t_object *)x, "record: out of memory");
        return;
    }

    {
        double count = graf_increment_edge(g, prev_key, new_key, 1.0);
        if (count < 0) {
            object_error((t_object *)x,
                         "record: failed to increment edge '%s'->'%s'",
                         prev_key->s_name, new_key->s_name);
            return;
        }

        /* right outlet first (Max convention), then left */
        if (created)
            outlet_anything(x->outlet_newnode, new_key, 0, NULL);

        {
            t_atom out[2];
            atom_setsym(&out[0], new_key);
            atom_setfloat(&out[1], count);
            outlet_anything(x->outlet_trans, prev_key, 2, out);
        }
    }

    object_notify((t_object *)g, gensym("modified"), NULL);
}


//////////////////////////////////////////////////////////////////////////
// Message handlers

/**
 * record <v> [<v> ...]
 *
 * Record one or more values in sequence. A multi-value record message is
 * exactly equivalent to sending each value as its own record message —
 * convenient for feeding a whole phrase at once (e.g. from [zl group]
 * or [coll] dumps).
 */
void graf_observe_record(t_graf_observe *x, t_symbol *s, long argc, t_atom *argv)
{
    t_graf *g = graf_observe_get_graf(x);
    long i;

    if (!g) return;
    if (argc < 1) {
        object_warn((t_object *)x, "record: expected at least one value");
        return;
    }

    for (i = 0; i < argc; i++) {
        t_symbol *id = graf_observe_atom_to_sym(x, &argv[i]);
        if (id)
            graf_observe_record_one(x, g, id);
    }
}

/**
 * order <n>
 *
 * Set the Markov order. Clears the context window (states of different
 * lengths are incomparable). Does NOT touch the graph: nodes observed at
 * a different order simply coexist — 1-gram node "a" and 2-gram node "a|b"
 * are distinct symbols and never collide.
 */
void graf_observe_order(t_graf_observe *x, long n)
{
    t_symbol **grown;

    if (n < 1 || n > GRAF_OBSERVE_MAX_ORDER) {
        object_error((t_object *)x,
                     "order: %ld out of range (1..%d)", n, GRAF_OBSERVE_MAX_ORDER);
        return;
    }
    if (n == x->order) return;

    grown = (t_symbol **)sysmem_resizeptr(x->window, n * sizeof(t_symbol *));
    if (!grown) {
        object_error((t_object *)x, "order: out of memory");
        return;
    }

    x->window       = grown;
    x->order        = n;
    x->window_count = 0;
    post("graf.observe: order set to %ld — context cleared", n);
}

/**
 * forget
 *
 * Clear the sliding context window without touching the graph.
 * Send this at phrase boundaries: the last state of one phrase will not
 * be linked to the first state of the next.
 */
void graf_observe_forget(t_graf_observe *x)
{
    x->window_count = 0;
    post("graf.observe: context cleared");
}

/**
 * normalize [prob|cost]
 *
 * DESTRUCTIVE conversion of the target graf's edge weights, per node:
 *
 *   prob (default): each outgoing weight becomes weight / sum-of-outgoing.
 *     Weights per node sum to 1 — a proper transition probability
 *     distribution, directly usable by [graf.traverse] mode random
 *     (which already treats weight as likelihood).
 *
 *   cost: each outgoing weight becomes -log(p) with p as above.
 *     Ready for [graf.traverse] mode dijkstra: shortest path over these
 *     costs == most probable path through the observed model. p is clamped
 *     to GRAF_OBSERVE_MIN_PROB so hand-added zero-weight edges get a large
 *     finite cost instead of infinity.
 *
 * Counts are destroyed either way — recording after normalize mixes count
 * and probability/cost scales in the same graph, which is meaningless.
 * A warning says so. If you need to keep observing, 'write' the counts to
 * CSV first and normalize a copy.
 *
 * Nodes with no outgoing edges, or an all-zero outgoing sum, are skipped.
 */
void graf_observe_normalize(t_graf_observe *x, t_symbol *mode)
{
    t_graf *g = graf_observe_get_graf(x);
    long    as_cost = 0;
    long    i, j, touched = 0;

    if (!g) return;

    if (mode && mode->s_name[0] != '\0') {
        if (mode == gensym("cost"))
            as_cost = 1;
        else if (mode != gensym("prob")) {
            object_error((t_object *)x,
                         "normalize: unknown mode '%s' (use prob or cost)",
                         mode->s_name);
            return;
        }
    }

    for (i = 0; i < g->node_count; i++) {
        t_graf_node *n = &g->nodes[i];
        double sum = 0.0;

        for (j = 0; j < n->edge_count; j++)
            sum += n->edge_weights[j];

        if (n->edge_count == 0 || sum <= 0.0)
            continue;

        for (j = 0; j < n->edge_count; j++) {
            double p = n->edge_weights[j] / sum;
            if (as_cost) {
                if (p < GRAF_OBSERVE_MIN_PROB) p = GRAF_OBSERVE_MIN_PROB;
                n->edge_weights[j] = -log(p);
            } else {
                n->edge_weights[j] = p;
            }
        }
        touched++;
    }

    post("graf.observe: normalized %ld node%s in '%s' to %s",
         touched, touched == 1 ? "" : "s",
         g->name->s_name,
         as_cost ? "costs (-log p, dijkstra-ready)" : "probabilities");
    object_warn((t_object *)x,
                "normalize is destructive: raw counts are gone — "
                "further record messages will mix scales");
    object_notify((t_object *)g, gensym("modified"), NULL);
}

/**
 * bang — post observer state to the console (debugging aid).
 */
void graf_observe_bang(t_graf_observe *x)
{
    char ctx[GRAF_OBSERVE_KEY_LEN] = "";
    long i, pos = 0;

    for (i = 0; i < x->window_count; i++) {
        int written = snprintf(ctx + pos, sizeof(ctx) - pos, "%s%s",
                               (i > 0) ? " " : "",
                               x->window[i]->s_name);
        if (written < 0 || pos + written >= (long)sizeof(ctx)) break;
        pos += written;
    }

    post("graf.observe -> '%s': order %ld, context [%s] (%ld/%ld)",
         x->graf_name->s_name, x->order,
         x->window_count > 0 ? ctx : "empty",
         x->window_count, x->order);
}