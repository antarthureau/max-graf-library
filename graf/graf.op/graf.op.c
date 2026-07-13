/**
 * graf_op.c — Graph algebra over named [graf] instances.
 *
 * Part of the graf external family for Max/MSP.
 * Reads one or two existing named [graf] instances and writes a computed
 * result graph into a third named [graf] instance. Set/relation operations
 * lifted to the graph level: union, intersection, difference, complement,
 * transpose, induced subgraph.
 *
 * -------------------------------------------------------------------------
 * USAGE
 *   [graf.op]
 *     No creation arguments. Every message carries its own operand names,
 *     since this object works across multiple named instances rather than
 *     being bound to one.
 *
 * MESSAGES IN
 *   union <a> <b> <target>       target := nodes(a) ∪ nodes(b), edges(a) ∪ edges(b);
 *                                shared edges take fmax(wA, wB)
 *   intersect <a> <b> <target>   target := nodes in both a and b; edges present in
 *                                both (surviving endpoints only), weight (wA+wB)/2
 *   difference <a> <b> <target>  target := nodes in a not in b; edges copied from a
 *                                where both endpoints survive
 *   complement <a> <target> [w]  target := same nodes as a; edge u->v added iff it
 *                                does NOT exist in a (no self-loops); weight w
 *                                defaults to 1.0
 *   transpose <a> <target>       target := same nodes as a; every edge u->v becomes
 *                                v->u, weight unchanged
 *   subgraph <a> <target> <id>...  target := induced subgraph of a on the id list;
 *                                unknown ids are skipped with a warning
 *   bang                         re-run the last operation with the same operands
 *
 * OUTLETS
 *   0  (left)  — target graf's name (symbol) on successful completion;
 *                chain into [prepend update] -> [graf.affiche] to auto-refresh
 *   1  (right) — bang on successful completion
 *
 * -------------------------------------------------------------------------
 * C / JAVA MAPPING
 *
 *   This object parallels the static-utility style of java.util.Collections
 *   or Guava's Sets.union()/intersection()/difference() — it owns no graph
 *   of its own, it only computes over graphs found by name at message time
 *   (service-locator lookup via graf_find, never a stored reference).
 *
 *   Key C patterns used here:
 *
 *   - The "last message" replay copy (last_selector/last_argv/last_argc) is
 *     a manually managed heap snapshot of the incoming atoms. In Java you'd
 *     just keep a reference to an immutable argument list; in C the caller's
 *     argv is stack/transient memory owned by Max, so we must copy it with
 *     sysmem_newptr and free the previous copy ourselves.
 *
 *   - object_method((t_object *)target, gensym("clear")) is dynamic message
 *     dispatch on another object — like calling target.getClass()
 *     .getMethod("clear").invoke(target) via reflection, except it is the
 *     normal, idiomatic way Max objects talk to each other.
 *
 *   - Reading a source graf's raw edges_to[]/edge_weights[]/edge_count
 *     arrays directly is like accessing package-visible fields of a sibling
 *     class in the same package — graf.h deliberately exposes the structs
 *     to the family instead of forcing accessor indirection.
 */

#include "ext.h"
#include "ext_obex.h"
#include "graf.h"           // t_graf, t_graf_node, graf_find(), graf_find_node(),
                            // graf_atom_to_id(), graf_ensure_node(), graf_increment_edge()
#include <math.h>           // fmax — union weight conflict resolution


//////////////////////////////////////////////////////////////////////////
// Object struct

/**
 * t_graf_op — state for one graf.op instance.
 *
 * Deliberately thin: no graph data lives here. The only real state is the
 * heap copy of the most recent message, kept so 'bang' can replay it.
 *
 * Java equivalent: a stateless service class holding one memento
 * (lastCommand) for redo — like a single-slot command history.
 */
typedef struct _graf_op {
    t_object    ob;

    void       *outlet_target;      // left outlet (created 2nd): target name symbol on success
    void       *outlet_done;        // right outlet (created 1st): bang on success

    /* replay memento — heap copy of the most recent operation message */
    t_symbol   *last_selector;      // e.g. gensym("union"); NULL until first operation
    t_atom     *last_argv;          // sysmem-owned copy of the message atoms
    long        last_argc;
} t_graf_op;


//////////////////////////////////////////////////////////////////////////
// Class variable

static t_class *s_graf_op_class = NULL;


//////////////////////////////////////////////////////////////////////////
// Prototypes

/* lifecycle */
void *graf_op_new(t_symbol *s, long argc, t_atom *argv);
void  graf_op_free(t_graf_op *x);
void  graf_op_assist(t_graf_op *x, void *b, long m, long a, char *s);

/* replay memento */
static void graf_op_store_last(t_graf_op *x, t_symbol *s, long argc, t_atom *argv);

/* shared validation / lookup helpers */
static t_graf   *graf_op_resolve(t_graf_op *x, t_symbol *name);
static t_symbol *graf_op_arg_sym(t_graf_op *x, t_symbol *s, const t_atom *a, const char *role);
static long      graf_op_find_edge(t_graf *g, t_symbol *u, t_symbol *v, double *w_out);
static t_max_err graf_op_copy_nodes(t_graf_op *x, t_graf *src, t_graf *dst, t_symbol *dst_name);
static t_max_err graf_op_write_edge(t_graf_op *x, t_graf *t, t_symbol *tname,
                                    t_symbol *u, t_symbol *v, double w);
static void      graf_op_clear_target(t_graf *t);

/* operations — one internal function per operation, called from both the
   message dispatch and bang's replay path */
static t_max_err graf_op_do_union     (t_graf_op *x, t_symbol *a, t_symbol *b, t_symbol *target);
static t_max_err graf_op_do_intersect (t_graf_op *x, t_symbol *a, t_symbol *b, t_symbol *target);
static t_max_err graf_op_do_difference(t_graf_op *x, t_symbol *a, t_symbol *b, t_symbol *target);
static t_max_err graf_op_do_complement(t_graf_op *x, t_symbol *a, t_symbol *target, double weight);
static t_max_err graf_op_do_transpose (t_graf_op *x, t_symbol *a, t_symbol *target);
static t_max_err graf_op_do_subgraph  (t_graf_op *x, t_symbol *a, t_symbol *target,
                                       long idc, const t_atom *idv);

/* dispatch */
static t_max_err graf_op_dispatch(t_graf_op *x, t_symbol *s, long argc, t_atom *argv);

/* message handlers */
void graf_op_msg (t_graf_op *x, t_symbol *s, long argc, t_atom *argv);
void graf_op_bang(t_graf_op *x);


//////////////////////////////////////////////////////////////////////////
// ext_main — class registration

/**
 * ext_main — called once when Max loads the external.
 *
 * All six operation messages share ONE trampoline (graf_op_msg): Max passes
 * the selector as the s argument, so a single A_GIMME handler can dispatch
 * on it. This is like six Java method references all pointing at the same
 * varargs dispatcher — the selector plays the role of the method name.
 *
 * A_GIMME everywhere because argument counts vary per operation; each
 * handler parses and validates manually (same rationale as graf.c's
 * id-taking messages).
 */
void ext_main(void *r)
{
    t_class *c = class_new("graf.op",
                           (method)graf_op_new,
                           (method)graf_op_free,
                           (long)sizeof(t_graf_op),
                           0L,
                           A_GIMME,
                           0);

    // inlet/outlet tooltip
    class_addmethod(c, (method)graf_op_assist, "assist",     A_CANT,  0);

    // replay last operation
    class_addmethod(c, (method)graf_op_bang,   "bang",       0);

    // operations — all routed through the same trampoline, dispatched on selector
    class_addmethod(c, (method)graf_op_msg,    "union",      A_GIMME, 0);
    class_addmethod(c, (method)graf_op_msg,    "intersect",  A_GIMME, 0);
    class_addmethod(c, (method)graf_op_msg,    "difference", A_GIMME, 0);
    class_addmethod(c, (method)graf_op_msg,    "complement", A_GIMME, 0);
    class_addmethod(c, (method)graf_op_msg,    "transpose",  A_GIMME, 0);
    class_addmethod(c, (method)graf_op_msg,    "subgraph",   A_GIMME, 0);

    class_register(CLASS_BOX, c);
    s_graf_op_class = c;

    post("graf.op: loaded");
}


//////////////////////////////////////////////////////////////////////////
// Object lifecycle

/**
 * graf_op_new — create a new graf.op instance.
 *
 * Takes no creation arguments (any given are ignored): operand names travel
 * with each message, so the object is not bound to a particular graf.
 *
 * Outlet ordering: Max creates outlets right-to-left visually.
 * First outlet_new/bangout call = rightmost outlet in the patcher.
 * We create outlet_done first (right = done bang),
 * then outlet_target second (left = target name symbol).
 */
void *graf_op_new(t_symbol *s, long argc, t_atom *argv)
{
    t_graf_op *x = (t_graf_op *)object_alloc(s_graf_op_class);
    if (!x) return NULL;

    /* Outlets: right-to-left creation order */
    x->outlet_done   = bangout(x);              // right outlet: bang on success
    x->outlet_target = outlet_new(x, NULL);     // left outlet: target graf name

    x->last_selector = NULL;
    x->last_argv     = NULL;
    x->last_argc     = 0;

    return x;
}

/**
 * graf_op_free — called by Max when this object is deleted.
 *
 * The only heap memory this object owns is the replay copy of the last
 * message. Max frees the outlets and the struct itself.
 *
 * Java equivalent: AutoCloseable.close() — guaranteed, no GC uncertainty.
 */
void graf_op_free(t_graf_op *x)
{
    if (x->last_argv) { sysmem_freeptr(x->last_argv); x->last_argv = NULL; }
}

/**
 * graf_op_assist — inlet/outlet hover tooltips.
 *
 * m selects inlet vs outlet; a is the inlet/outlet index (same pattern as
 * graf_assist in graf.c).
 */
void graf_op_assist(t_graf_op *x, void *b, long m, long a, char *s)
{
    if (m == ASSIST_INLET) {
        sprintf(s,
            "union / intersect / difference / complement / "
            "transpose / subgraph / bang (replay last)");
    } else {
        switch (a) {
            case 0:  sprintf(s, "target graf name (symbol) on completion"); break;
            case 1:  sprintf(s, "bang on completion");                      break;
            default: sprintf(s, "graf.op outlet");                          break;
        }
    }
}


//////////////////////////////////////////////////////////////////////////
// Replay memento

/**
 * graf_op_store_last — snapshot the incoming message so bang can replay it.
 *
 * Frees the previous copy and allocates a fresh one. Called at the start of
 * every operation message, BEFORE validation — so bang always replays the
 * most recent attempt, even one that failed validation (re-banging it will
 * simply fail the same way, with the same error message).
 *
 * The copy is required because Max owns the argv memory only for the
 * duration of the message call — keeping the pointer would dangle.
 *
 * Java analogy: this.lastCommand = List.copyOf(args) — except the "copyOf"
 * is a manual sysmem_newptr + element copy, and we must free the old list.
 */
static void graf_op_store_last(t_graf_op *x, t_symbol *s, long argc, t_atom *argv)
{
    long i;

    // drop the previous copy first
    if (x->last_argv) { sysmem_freeptr(x->last_argv); x->last_argv = NULL; }
    x->last_argc     = 0;
    x->last_selector = s;

    if (argc <= 0) return;   // selector-only message — nothing more to copy

    x->last_argv = (t_atom *)sysmem_newptr(argc * sizeof(t_atom));
    if (!x->last_argv) {
        // not fatal for the operation itself — only bang-replay is lost
        object_warn((t_object *)x,
                    "graf.op: out of memory storing message for bang replay");
        x->last_selector = NULL;
        return;
    }

    for (i = 0; i < argc; i++)
        x->last_argv[i] = argv[i];  // t_atom is a plain value struct — assignment copies it

    x->last_argc = argc;
}


//////////////////////////////////////////////////////////////////////////
// Shared validation / lookup helpers

/**
 * graf_op_resolve — find a named [graf] instance, erroring by name if absent.
 *
 * Thin wrapper over graf_find() that reports WHICH operand is missing —
 * every operation looks up two or three instances, so the name in the
 * error message matters.
 *
 * Java analogy: Objects.requireNonNull(registry.find(name), name + " not found").
 */
static t_graf *graf_op_resolve(t_graf_op *x, t_symbol *name)
{
    t_graf *g = graf_find(name);
    if (!g) {
        object_error((t_object *)x,
                     "graf.op: no [graf] named '%s' found", name->s_name);
    }
    return g;
}

/**
 * graf_op_arg_sym — validate that an atom is a symbol and return it.
 *
 * Used for graf INSTANCE names only (node ids go through graf_atom_to_id
 * instead, which also accepts numbers). Instance names are always symbols —
 * they come from [graf <name>] creation arguments — so a numeric atom here
 * is a malformed message, not a convertible id.
 *
 * @param role  human-readable slot name for the error message ("source a",
 *              "target", ...)
 * @return      the symbol, or NULL after posting an error
 */
static t_symbol *graf_op_arg_sym(t_graf_op *x, t_symbol *s, const t_atom *a, const char *role)
{
    if (atom_gettype(a) != A_SYM) {
        object_error((t_object *)x,
                     "graf.op: %s — %s must be a graf instance name (symbol)",
                     s->s_name, role);
        return NULL;
    }
    return atom_getsym(a);
}

/**
 * graf_op_find_edge — does edge u->v exist in g? Optionally report its weight.
 *
 * Reads the source node's raw parallel arrays (edges_to[]/edge_weights[])
 * directly — graf.h exposes the structs to the family for exactly this kind
 * of membership check. Linear scan, O(edge_count), fine at sequencer scale.
 *
 * t_symbol* pointer equality is valid — Max interns every symbol.
 *
 * @param w_out  where to store the weight if found; pass NULL if you only
 *               care about existence
 * @return       1 if the edge exists, 0 otherwise
 *
 * Java analogy: adjacency.get(u).containsKey(v) with an optional .get(v).
 */
static long graf_op_find_edge(t_graf *g, t_symbol *u, t_symbol *v, double *w_out)
{
    t_graf_node *un = graf_find_node(g, u);
    long i;

    if (!un) return 0;

    for (i = 0; i < un->edge_count; i++) {
        if (un->edges_to[i] == v) {
            if (w_out) *w_out = un->edge_weights[i];
            return 1;
        }
    }
    return 0;
}

/**
 * graf_op_copy_nodes — ensure every node of src also exists in dst.
 *
 * Payload-free copy (graf_ensure_node creates bare nodes) — graph algebra
 * operates on structure, not payloads.
 *
 * Iterating src->nodes[] while inserting into dst is safe because src and
 * dst are guaranteed distinct by the target-!= -source check: only dst's
 * node array can reallocate and move, and we never hold pointers into it.
 * (This is the classic C pitfall flagged in graf.h's graf_ensure_node doc —
 * avoided here by construction.)
 *
 * Java analogy: dst.addAll(src.nodeIds()).
 *
 * @return MAX_ERR_NONE, or MAX_ERR_OUT_OF_MEM after posting an error
 */
static t_max_err graf_op_copy_nodes(t_graf_op *x, t_graf *src, t_graf *dst, t_symbol *dst_name)
{
    long i;

    for (i = 0; i < src->node_count; i++) {
        if (!graf_ensure_node(dst, src->nodes[i].id, NULL)) {
            object_error((t_object *)x,
                         "graf.op: out of memory adding node '%s' to [graf '%s']",
                         src->nodes[i].id->s_name, dst_name->s_name);
            return MAX_ERR_OUT_OF_MEM;
        }
    }
    return MAX_ERR_NONE;
}

/**
 * graf_op_write_edge — set edge u->v in the (freshly cleared) target.
 *
 * Because every operation clears its target before writing, the first
 * graf_increment_edge call on any edge behaves as a plain "set" — the
 * increment starts from nothing.
 *
 * Both endpoints must already exist in t (callers copy/filter nodes first),
 * so a -1.0 return here means allocation failure, which is a hard error.
 *
 * IMPORTANT ordering invariant: all node insertions into the target happen
 * BEFORE any edge writes. graf_increment_edge only grows per-node edge
 * arrays, never the node array itself, so t->nodes stays stable while
 * callers iterate over it during the edge phase.
 */
static t_max_err graf_op_write_edge(t_graf_op *x, t_graf *t, t_symbol *tname,
                                    t_symbol *u, t_symbol *v, double w)
{
    if (graf_increment_edge(t, u, v, w) < 0.0) {
        object_error((t_object *)x,
                     "graf.op: failed writing edge '%s' -> '%s' in [graf '%s'] (out of memory?)",
                     u->s_name, v->s_name, tname->s_name);
        return MAX_ERR_GENERIC;
    }
    return MAX_ERR_NONE;
}

/**
 * graf_op_clear_target — empty the target graf before writing the result.
 *
 * Calls the target's own 'clear' message handler via Max's dynamic dispatch
 * rather than touching its internals — the established cross-object pattern
 * in this library. Only ever called AFTER all validation has passed
 * (including target != sources), so a source can never be cleared.
 *
 * Java analogy: reflection-style target.invoke("clear") — but idiomatic
 * in Max, not a workaround.
 */
static void graf_op_clear_target(t_graf *t)
{
    object_method((t_object *)t, gensym("clear"));
}


//////////////////////////////////////////////////////////////////////////
// Operations

/**
 * graf_op_do_union — target := a ∪ b.
 *
 * Nodes: everything in a plus everything in b.
 * Edges: every edge of a and every edge of b. When u->v exists in BOTH
 * (possibly with different weights), the target gets fmax(wA, wB) — the
 * "stronger" relation wins.
 *
 * Two edge passes: (1) all of a's edges, taking fmax against b where the
 * edge is shared; (2) b's edges that a does NOT have (the shared ones were
 * already written in pass 1).
 *
 * Java analogy: result.putAll(a); b.forEach((k,v) ->
 *   result.merge(k, v, Math::max));
 *
 * Safety checks (shape was already validated by dispatch):
 * lookup all three instances, reject target aliasing a source, and only
 * then clear + write.
 */
static t_max_err graf_op_do_union(t_graf_op *x, t_symbol *a, t_symbol *b, t_symbol *target)
{
    t_graf *ga, *gb, *gt;
    long    i, e;

    // 2. look up every named instance
    ga = graf_op_resolve(x, a);      if (!ga) return MAX_ERR_GENERIC;
    gb = graf_op_resolve(x, b);      if (!gb) return MAX_ERR_GENERIC;
    gt = graf_op_resolve(x, target); if (!gt) return MAX_ERR_GENERIC;

    // 3. target must not alias a source — clearing it would destroy the source
    if (target == a || target == b) {
        object_error((t_object *)x,
                     "graf.op: target must be a different graf instance than the source(s)");
        return MAX_ERR_GENERIC;
    }

    // 4. all checks passed — clear, then write
    graf_op_clear_target(gt);

    // node phase: nodes(a) ∪ nodes(b)
    if (graf_op_copy_nodes(x, ga, gt, target) != MAX_ERR_NONE) return MAX_ERR_GENERIC;
    if (graf_op_copy_nodes(x, gb, gt, target) != MAX_ERR_NONE) return MAX_ERR_GENERIC;

    // edge phase 1: every edge of a; shared edges take fmax(wA, wB)
    for (i = 0; i < ga->node_count; i++) {
        t_graf_node *un = &ga->nodes[i];
        for (e = 0; e < un->edge_count; e++) {
            t_symbol *v  = un->edges_to[e];
            double    w  = un->edge_weights[e];
            double    wb;

            // dangling edge guard: v should exist in a (and thus in target),
            // but skip defensively rather than corrupt the result
            if (!graf_find_node(gt, v)) {
                object_warn((t_object *)x,
                            "graf.op: union — dangling edge '%s' -> '%s' in [graf '%s'] skipped",
                            un->id->s_name, v->s_name, a->s_name);
                continue;
            }

            if (graf_op_find_edge(gb, un->id, v, &wb))
                w = fmax(w, wb);    // conflict: the stronger relation wins

            if (graf_op_write_edge(x, gt, target, un->id, v, w) != MAX_ERR_NONE)
                return MAX_ERR_GENERIC;
        }
    }

    // edge phase 2: b's edges that a does not have (shared ones already written)
    for (i = 0; i < gb->node_count; i++) {
        t_graf_node *un = &gb->nodes[i];
        for (e = 0; e < un->edge_count; e++) {
            t_symbol *v = un->edges_to[e];

            if (graf_op_find_edge(ga, un->id, v, NULL))
                continue;   // shared edge — pass 1 handled it

            if (!graf_find_node(gt, v)) {
                object_warn((t_object *)x,
                            "graf.op: union — dangling edge '%s' -> '%s' in [graf '%s'] skipped",
                            un->id->s_name, v->s_name, b->s_name);
                continue;
            }

            if (graf_op_write_edge(x, gt, target, un->id, v, un->edge_weights[e]) != MAX_ERR_NONE)
                return MAX_ERR_GENERIC;
        }
    }

    return MAX_ERR_NONE;
}

/**
 * graf_op_do_intersect — target := a ∩ b.
 *
 * Nodes: only ids present in BOTH a and b.
 * Edges: u->v included only if the edge exists in both a and b AND both
 * endpoints survived the node intersection; weight is the average
 * (wA + wB) / 2.0 — a compromise between the two relations.
 *
 * The endpoint-survival check doubles as a membership test against the
 * target: after the node phase, "in target" == "in both a and b".
 *
 * Java analogy: result = new HashSet<>(a); result.retainAll(b);
 * then edge merge with (x, y) -> (x + y) / 2.
 */
static t_max_err graf_op_do_intersect(t_graf_op *x, t_symbol *a, t_symbol *b, t_symbol *target)
{
    t_graf *ga, *gb, *gt;
    long    i, e;

    // 2. look up every named instance
    ga = graf_op_resolve(x, a);      if (!ga) return MAX_ERR_GENERIC;
    gb = graf_op_resolve(x, b);      if (!gb) return MAX_ERR_GENERIC;
    gt = graf_op_resolve(x, target); if (!gt) return MAX_ERR_GENERIC;

    // 3. target must not alias a source
    if (target == a || target == b) {
        object_error((t_object *)x,
                     "graf.op: target must be a different graf instance than the source(s)");
        return MAX_ERR_GENERIC;
    }

    // 4. clear, then write
    graf_op_clear_target(gt);

    // node phase: ids present in both a and b
    for (i = 0; i < ga->node_count; i++) {
        t_symbol *id = ga->nodes[i].id;
        if (graf_find_node(gb, id)) {
            if (!graf_ensure_node(gt, id, NULL)) {
                object_error((t_object *)x,
                             "graf.op: out of memory adding node '%s' to [graf '%s']",
                             id->s_name, target->s_name);
                return MAX_ERR_OUT_OF_MEM;
            }
        }
    }

    // edge phase: edges present in both, endpoints surviving; weight averaged
    for (i = 0; i < ga->node_count; i++) {
        t_graf_node *un = &ga->nodes[i];

        if (!graf_find_node(gt, un->id)) continue;  // u did not survive

        for (e = 0; e < un->edge_count; e++) {
            t_symbol *v  = un->edges_to[e];
            double    wa = un->edge_weights[e];
            double    wb;

            if (!graf_find_node(gt, v)) continue;               // v did not survive
            if (!graf_op_find_edge(gb, un->id, v, &wb)) continue; // not in b

            if (graf_op_write_edge(x, gt, target, un->id, v, (wa + wb) / 2.0) != MAX_ERR_NONE)
                return MAX_ERR_GENERIC;
        }
    }

    return MAX_ERR_NONE;
}

/**
 * graf_op_do_difference — target := a \ b.
 *
 * Nodes: ids in a that are NOT in b.
 * Edges: copied from a, weights unchanged, restricted to pairs where both
 * endpoints survive. (Edges of b are irrelevant — set difference on nodes,
 * with a's relation induced on what remains.)
 *
 * Java analogy: result = new HashSet<>(a); result.removeAll(b).
 */
static t_max_err graf_op_do_difference(t_graf_op *x, t_symbol *a, t_symbol *b, t_symbol *target)
{
    t_graf *ga, *gb, *gt;
    long    i, e;

    // 2. look up every named instance
    ga = graf_op_resolve(x, a);      if (!ga) return MAX_ERR_GENERIC;
    gb = graf_op_resolve(x, b);      if (!gb) return MAX_ERR_GENERIC;
    gt = graf_op_resolve(x, target); if (!gt) return MAX_ERR_GENERIC;

    // 3. target must not alias a source
    if (target == a || target == b) {
        object_error((t_object *)x,
                     "graf.op: target must be a different graf instance than the source(s)");
        return MAX_ERR_GENERIC;
    }

    // 4. clear, then write
    graf_op_clear_target(gt);

    // node phase: ids in a that are not in b
    for (i = 0; i < ga->node_count; i++) {
        t_symbol *id = ga->nodes[i].id;
        if (!graf_find_node(gb, id)) {
            if (!graf_ensure_node(gt, id, NULL)) {
                object_error((t_object *)x,
                             "graf.op: out of memory adding node '%s' to [graf '%s']",
                             id->s_name, target->s_name);
                return MAX_ERR_OUT_OF_MEM;
            }
        }
    }

    // edge phase: a's edges where both endpoints survived, weights unchanged
    for (i = 0; i < ga->node_count; i++) {
        t_graf_node *un = &ga->nodes[i];

        if (!graf_find_node(gt, un->id)) continue;  // u was removed by the difference

        for (e = 0; e < un->edge_count; e++) {
            t_symbol *v = un->edges_to[e];

            if (!graf_find_node(gt, v)) continue;   // v was removed by the difference

            if (graf_op_write_edge(x, gt, target, un->id, v, un->edge_weights[e]) != MAX_ERR_NONE)
                return MAX_ERR_GENERIC;
        }
    }

    return MAX_ERR_NONE;
}

/**
 * graf_op_do_complement — target := complement of a (same node set).
 *
 * For every ORDERED pair (u, v) with u != v in a's node set, the target
 * gets edge u->v if and only if a does NOT have it. Self-loops are never
 * generated. All complement edges share one weight (default 1.0, or the
 * optional argument) — a has no weight to invert, so a uniform weight is
 * the only sensible choice.
 *
 * COMPLEXITY: O(n²) node pairs, each with an O(edge_count) membership scan.
 * Acceptable at the sequencer-scale graph sizes (< ~100 nodes) this library
 * targets — the same caveat already used for the linear node scans
 * elsewhere in the family.
 *
 * Java analogy: iterating a boolean adjacency matrix and flipping every
 * off-diagonal cell.
 */
static t_max_err graf_op_do_complement(t_graf_op *x, t_symbol *a, t_symbol *target, double weight)
{
    t_graf *ga, *gt;
    long    i, j;

    // 2. look up both named instances
    ga = graf_op_resolve(x, a);      if (!ga) return MAX_ERR_GENERIC;
    gt = graf_op_resolve(x, target); if (!gt) return MAX_ERR_GENERIC;

    // 3. target must not alias the source
    if (target == a) {
        object_error((t_object *)x,
                     "graf.op: target must be a different graf instance than the source(s)");
        return MAX_ERR_GENERIC;
    }

    // 4. clear, then write
    graf_op_clear_target(gt);

    // node phase: same node set as a
    if (graf_op_copy_nodes(x, ga, gt, target) != MAX_ERR_NONE) return MAX_ERR_GENERIC;

    // edge phase: every ordered pair u != v where a has NO edge u->v
    for (i = 0; i < ga->node_count; i++) {
        t_symbol *u = ga->nodes[i].id;

        for (j = 0; j < ga->node_count; j++) {
            t_symbol *v = ga->nodes[j].id;

            if (u == v) continue;                            // never generate self-loops
            if (graf_op_find_edge(ga, u, v, NULL)) continue; // a has it — complement doesn't

            if (graf_op_write_edge(x, gt, target, u, v, weight) != MAX_ERR_NONE)
                return MAX_ERR_GENERIC;
        }
    }

    return MAX_ERR_NONE;
}

/**
 * graf_op_do_transpose — target := a with every edge reversed.
 *
 * Same node set as a; every edge u->v (weight w) becomes v->u (weight w).
 * The classic G^T used to turn "what does u lead to?" into "what leads
 * to u?" — e.g. running a random walk backwards through learned material.
 *
 * Java analogy: building the reverse adjacency map —
 *   a.forEach((u, edges) -> edges.forEach((v, w) -> result.put(v, u, w))).
 */
static t_max_err graf_op_do_transpose(t_graf_op *x, t_symbol *a, t_symbol *target)
{
    t_graf *ga, *gt;
    long    i, e;

    // 2. look up both named instances
    ga = graf_op_resolve(x, a);      if (!ga) return MAX_ERR_GENERIC;
    gt = graf_op_resolve(x, target); if (!gt) return MAX_ERR_GENERIC;

    // 3. target must not alias the source
    if (target == a) {
        object_error((t_object *)x,
                     "graf.op: target must be a different graf instance than the source(s)");
        return MAX_ERR_GENERIC;
    }

    // 4. clear, then write
    graf_op_clear_target(gt);

    // node phase: same node set as a
    if (graf_op_copy_nodes(x, ga, gt, target) != MAX_ERR_NONE) return MAX_ERR_GENERIC;

    // edge phase: u->v (w) becomes v->u (w)
    for (i = 0; i < ga->node_count; i++) {
        t_graf_node *un = &ga->nodes[i];
        for (e = 0; e < un->edge_count; e++) {
            t_symbol *v = un->edges_to[e];

            if (!graf_find_node(gt, v)) {
                object_warn((t_object *)x,
                            "graf.op: transpose — dangling edge '%s' -> '%s' in [graf '%s'] skipped",
                            un->id->s_name, v->s_name, a->s_name);
                continue;
            }

            // note the swap: v -> u
            if (graf_op_write_edge(x, gt, target, v, un->id, un->edge_weights[e]) != MAX_ERR_NONE)
                return MAX_ERR_GENERIC;
        }
    }

    return MAX_ERR_NONE;
}

/**
 * graf_op_do_subgraph — target := induced subgraph of a on the given id list.
 *
 * Nodes: the listed ids that actually exist in a. An id that doesn't exist
 * is a soft issue — object_warn and skip, don't abort (the rest of the list
 * is still meaningful). Duplicate ids in the list are harmless
 * (graf_ensure_node is find-or-create).
 *
 * Edges: copied from a wherever BOTH endpoints made it into the target —
 * the definition of an induced subgraph: keep every relation of a whose
 * endpoints are both in the chosen vertex subset.
 *
 * Ids are canonicalized through graf_atom_to_id, so numeric ids (MIDI
 * pitches from graf.observe, ints from message boxes) work here exactly
 * like everywhere else in the family.
 *
 * Java analogy: Set<V> keep = ...; result = graph.inducedSubgraph(keep) —
 * i.e. filter vertices, then filter edges on containsAll(endpoints).
 *
 * @param idc  number of id atoms
 * @param idv  the id atoms (message args after <a> <target>)
 */
static t_max_err graf_op_do_subgraph(t_graf_op *x, t_symbol *a, t_symbol *target,
                                     long idc, const t_atom *idv)
{
    t_graf *ga, *gt;
    long    i, e;
    long    kept = 0;

    // 2. look up both named instances
    ga = graf_op_resolve(x, a);      if (!ga) return MAX_ERR_GENERIC;
    gt = graf_op_resolve(x, target); if (!gt) return MAX_ERR_GENERIC;

    // 3. target must not alias the source
    if (target == a) {
        object_error((t_object *)x,
                     "graf.op: target must be a different graf instance than the source(s)");
        return MAX_ERR_GENERIC;
    }

    // 4. clear, then write
    graf_op_clear_target(gt);

    // node phase: listed ids that exist in a (unknown ids warn + skip)
    for (i = 0; i < idc; i++) {
        t_symbol *id = graf_atom_to_id(&idv[i]);

        if (!id) {
            object_warn((t_object *)x,
                        "graf.op: subgraph — argument %ld is not a valid node id, skipped",
                        i + 3);   // human-readable position within the full message
            continue;
        }
        if (!graf_find_node(ga, id)) {
            object_warn((t_object *)x,
                        "graf.op: subgraph — node '%s' not found in [graf '%s'], skipped",
                        id->s_name, a->s_name);
            continue;
        }
        if (!graf_ensure_node(gt, id, NULL)) {
            object_error((t_object *)x,
                         "graf.op: out of memory adding node '%s' to [graf '%s']",
                         id->s_name, target->s_name);
            return MAX_ERR_OUT_OF_MEM;
        }
        kept++;
    }

    if (kept == 0) {
        object_warn((t_object *)x,
                    "graf.op: subgraph — no listed node exists in [graf '%s']; "
                    "[graf '%s'] is now empty", a->s_name, target->s_name);
        // still a successful (empty) result — fall through to edge phase (no-op)
    }

    // edge phase: a's edges where both endpoints are in the target.
    // Iterate the TARGET's node list (the survivors) and read each node's
    // edges from a — safe because the edge phase never inserts nodes, so
    // gt->nodes cannot reallocate under us (see graf_op_write_edge).
    for (i = 0; i < gt->node_count; i++) {
        t_graf_node *un = graf_find_node(ga, gt->nodes[i].id);

        if (!un) continue;  // cannot happen — survivor came from a — but stay safe

        for (e = 0; e < un->edge_count; e++) {
            t_symbol *v = un->edges_to[e];

            if (!graf_find_node(gt, v)) continue;   // endpoint not in the subset

            if (graf_op_write_edge(x, gt, target, un->id, v, un->edge_weights[e]) != MAX_ERR_NONE)
                return MAX_ERR_GENERIC;
        }
    }

    return MAX_ERR_NONE;
}


//////////////////////////////////////////////////////////////////////////
// Dispatch

/**
 * graf_op_dispatch — validate message shape and route to the operation.
 *
 * This is safety check 1 (right number/type of arguments per operation);
 * checks 2–4 (instance lookup, target aliasing, clear-then-write) live in
 * the graf_op_do_* functions so they run on bang replays too.
 *
 * Called from two places with the SAME semantics:
 *   - graf_op_msg    — a fresh incoming message
 *   - graf_op_bang   — replaying the stored copy of the last message
 * so validation and execution logic is never duplicated.
 *
 * Fires the outlets on success, right-to-left (done bang first, then the
 * target name on the left outlet) — so a trigger wired to the left outlet
 * already has the bang available when it fires.
 *
 * Java analogy: a switch on the command name in a Command-pattern executor.
 */
static t_max_err graf_op_dispatch(t_graf_op *x, t_symbol *s, long argc, t_atom *argv)
{
    t_max_err err         = MAX_ERR_GENERIC;
    t_symbol *target_name = NULL;   // remembered per branch for the success output

    if (s == gensym("union") || s == gensym("intersect") || s == gensym("difference")) {
        // shape: exactly <a> <b> <target>, all symbols
        t_symbol *a, *b, *t;

        if (argc != 3) {
            object_error((t_object *)x,
                         "graf.op: %s requires 3 arguments: <a> <b> <target>", s->s_name);
            return MAX_ERR_GENERIC;
        }
        a = graf_op_arg_sym(x, s, &argv[0], "source a");  if (!a) return MAX_ERR_GENERIC;
        b = graf_op_arg_sym(x, s, &argv[1], "source b");  if (!b) return MAX_ERR_GENERIC;
        t = graf_op_arg_sym(x, s, &argv[2], "target");    if (!t) return MAX_ERR_GENERIC;
        target_name = t;

        if (s == gensym("union"))          err = graf_op_do_union(x, a, b, t);
        else if (s == gensym("intersect")) err = graf_op_do_intersect(x, a, b, t);
        else                               err = graf_op_do_difference(x, a, b, t);

    } else if (s == gensym("complement")) {
        // shape: <a> <target> [weight] — weight is an optional number
        t_symbol *a, *t;
        double    weight = 1.0;

        if (argc < 2 || argc > 3) {
            object_error((t_object *)x,
                         "graf.op: complement requires 2 or 3 arguments: <a> <target> [weight]");
            return MAX_ERR_GENERIC;
        }
        a = graf_op_arg_sym(x, s, &argv[0], "source a");  if (!a) return MAX_ERR_GENERIC;
        t = graf_op_arg_sym(x, s, &argv[1], "target");    if (!t) return MAX_ERR_GENERIC;
        target_name = t;

        if (argc == 3) {
            long type = atom_gettype(&argv[2]);
            if (type != A_FLOAT && type != A_LONG) {
                object_error((t_object *)x,
                             "graf.op: complement — weight must be a number");
                return MAX_ERR_GENERIC;
            }
            weight = atom_getfloat(&argv[2]);   // atom_getfloat coerces A_LONG too
        }

        err = graf_op_do_complement(x, a, t, weight);

    } else if (s == gensym("transpose")) {
        // shape: exactly <a> <target>, both symbols
        t_symbol *a, *t;

        if (argc != 2) {
            object_error((t_object *)x,
                         "graf.op: transpose requires 2 arguments: <a> <target>");
            return MAX_ERR_GENERIC;
        }
        a = graf_op_arg_sym(x, s, &argv[0], "source a");  if (!a) return MAX_ERR_GENERIC;
        t = graf_op_arg_sym(x, s, &argv[1], "target");    if (!t) return MAX_ERR_GENERIC;
        target_name = t;

        err = graf_op_do_transpose(x, a, t);

    } else if (s == gensym("subgraph")) {
        // shape: <a> <target> <id> [<id> ...] — at least one id
        t_symbol *a, *t;

        if (argc < 3) {
            object_error((t_object *)x,
                         "graf.op: subgraph requires at least 3 arguments: <a> <target> <id> ...");
            return MAX_ERR_GENERIC;
        }
        a = graf_op_arg_sym(x, s, &argv[0], "source a");  if (!a) return MAX_ERR_GENERIC;
        t = graf_op_arg_sym(x, s, &argv[1], "target");    if (!t) return MAX_ERR_GENERIC;
        target_name = t;

        err = graf_op_do_subgraph(x, a, t, argc - 2, argv + 2);

    } else {
        // unreachable — only registered selectors arrive here
        object_error((t_object *)x, "graf.op: unknown operation '%s'", s->s_name);
        return MAX_ERR_GENERIC;
    }

    if (err != MAX_ERR_NONE) return err;

    // success — fire right-to-left: done bang first, target name second,
    // so a [trigger] on the left outlet already has the bang available
    outlet_bang(x->outlet_done);
    outlet_anything(x->outlet_target, target_name, 0, NULL);

    return MAX_ERR_NONE;
}


//////////////////////////////////////////////////////////////////////////
// Max message handlers

/**
 * graf_op_msg — shared trampoline for all six operation messages.
 *
 * Max passes the selector as s, so one handler serves union / intersect /
 * difference / complement / transpose / subgraph. Snapshot the message
 * FIRST (before validation, per the replay contract — bang re-runs the
 * most recent attempt, valid or not), then dispatch.
 */
void graf_op_msg(t_graf_op *x, t_symbol *s, long argc, t_atom *argv)
{
    graf_op_store_last(x, s, argc, argv);
    graf_op_dispatch(x, s, argc, argv);
}

/**
 * graf_op_bang — re-run the last operation with the same operands.
 *
 * Replays the stored copy through the SAME dispatch path as a fresh
 * message — same validation, same execution, no duplicated logic. Does NOT
 * re-store the copy: freeing last_argv and then reading it as the replay
 * arguments would be a use-after-free.
 *
 * Warns and does nothing if no operation has been sent yet.
 *
 * Java analogy: executor.redo() on a single-slot command history.
 */
void graf_op_bang(t_graf_op *x)
{
    if (!x->last_selector) {
        object_warn((t_object *)x,
                    "graf.op: nothing to replay — send an operation first");
        return;
    }

    graf_op_dispatch(x, x->last_selector, x->last_argc, x->last_argv);
}