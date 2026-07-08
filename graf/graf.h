/**
 * @file graf.h
 * Shared header for the graf external family.
 *
 * Include this in every graf.* external. Defines the canonical data structures
 * and provides static inline helpers that are compiled once per translation
 * unit (avoiding linker collisions across externals).
 *
 * Java analogy: this is like a shared interface/model package that both the
 * data-store class (graf) and its view class (graf.affiche) import.
 *
 * Helpers:
 *   - graf_find()           lookup a named graf instance (read)
 *   - graf_find_node()      lookup a node by id (read)
 *   - graf_atom_to_id()     canonical atom -> node-id symbol conversion
 *   - graf_ensure_node()    find-or-create a node (write — used by graf.observe)
 *   - graf_increment_edge() add-or-increment an edge weight (write — graf.observe)
 *
 * The write helpers exist because graf.c's internal mutation functions
 * (graf_addnode_quiet, graf_addedge_quiet) are `static` — private to the
 * graf external's binary. Other externals in the family cannot link to
 * them, so the minimal shared mutation surface lives here instead.
 * NOTE: graf.c currently keeps its own private copies of this logic; the
 * two must stay behaviourally identical (future refactor: make graf.c call
 * these helpers too).
 */

#pragma once

#include "ext.h"
#include "ext_obex.h"
#include <stdio.h>      /* snprintf — graf_atom_to_id */


////////////////////////// data structures

/**
 * A single node in the graph.
 *
 * Equivalent to a vertex V in IGraph<V>.
 * Outgoing edges are stored as parallel arrays (edges_to[i] / edge_weights[i]),
 * allocated lazily — both are NULL until the first edge is added.
 *
 * t_symbol* pointer equality is valid for ID comparison because Max interns
 * every symbol via gensym() — the same string always returns the same pointer.
 * This is identical to Java's String.intern() / identity comparison on interned strings.
 */
typedef struct _graf_node {
    t_symbol   *id;             // node identifier (interned symbol, pointer equality valid)
    t_atom     *payload;        // optional Max atoms attached to this node (int/float/symbol)
    long        payload_count;
    t_symbol  **edges_to;       // dynamic array: target node IDs for outgoing edges
    double     *edge_weights;   // parallel array: weight for each outgoing edge
    long        edge_count;
} t_graf_node;

/**
 * The graf object itself.
 *
 * Holds a dynamic array of nodes (doubles in capacity like Java ArrayList)
 * and a current traversal position pointer.
 * Registered by name in the Max object registry so other objects
 * (graf.traverse, graf.affiche) can find it via object_findregistered().
 */
typedef struct _graf {
    t_object    ob;
    void       *outlet;
    t_symbol   *name;           // registered instance name 
    t_graf_node *nodes;         // dynamic array (capacity doubles on overflow) 
    long        node_count;
    long        node_capacity;
    long        next_node_id;   // auto-increment counter for anonymous node IDs 
    t_symbol   *current;        // current traversal position (NULL if unset) 
} t_graf;


////////////////////////// inline lookup helpers (read)

/**
 * Find a named graf instance in the Max object registry.
 *
 * Uses the same CLASS_BOX namespace that [graf my_graph] registers under.
 * Returns NULL if no instance with that name exists yet.
 *
 * Java analogy: like a service locator / registry lookup —
 *   GrafRegistry.find("my_graph")
 */
static inline t_graf *graf_find(t_symbol *name)
{
    return (t_graf *)object_findregistered(CLASS_BOX, name);
}

/**
 * Linear scan for a node by symbol ID within a graf instance.
 *
 * Returns a pointer into x->nodes[], or NULL if not found.
 * O(n) — acceptable for sequencer-scale graphs (< ~100 nodes).
 *
 * Java analogy: like ArrayList<Node>.stream().filter(n -> n.id == id).findFirst()
 * but using pointer equality instead of .equals() because Max interns symbols.
 */
static inline t_graf_node *graf_find_node(t_graf *x, t_symbol *id)
{
    long i;
    for (i = 0; i < x->node_count; i++) {
        if (x->nodes[i].id == id)
            return &x->nodes[i];
    }
    return NULL;
}


////////////////////////// canonical id conversion

/**
 * graf_atom_to_id — convert ANY incoming atom to an interned node-id symbol.
 *
 * This is the ONE canonical conversion for the whole family. Node ids are
 * always symbols internally, but users legitimately send numbers: message
 * boxes type "removenode 0" as an int atom, MIDI pitches arrive as ints,
 * pitch trackers emit floats. Max's typechecker even rejects int atoms
 * outright on A_SYM-typed messages before the handler runs — which is why
 * every id-taking message in graf.c is registered A_GIMME and parses
 * through this helper instead.
 *
 * Formats deliberately match graf.c's CSV writer (graf_atom_to_str) and
 * graf.observe's record conversion:
 *   A_SYM   → passed through unchanged
 *   A_LONG  → "%ld"   (60      → "60")
 *   A_FLOAT → "%.10g" (0.5     → "0.5", 60.0 → "60")
 * so a node created by [graf.observe] from the int 250, referenced from a
 * message box as 250, and round-tripped through CSV as the token "250" all
 * intern to the SAME t_symbol — pointer equality holds everywhere.
 *
 * Returns NULL for unhandled atom types; callers report the error with
 * their own object context.
 *
 * Java analogy: a static factory NodeId.of(Object o) that canonicalises
 * every input to one interned String representation.
 */
static inline t_symbol *graf_atom_to_id(const t_atom *a)
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
            return NULL;
    }
}


////////////////////////// inline mutation helpers (write)

/**
 * graf_ensure_node — find a node by id, creating it (payload-free) if absent.
 *
 * g       — target graf instance
 * id      — interned node id
 * created — optional out-parameter: set to 1 if the node was newly created,
 *           0 if it already existed. Pass NULL if you don't care.
 *
 * Returns a pointer to the node, or NULL on allocation failure.
 *
 * IMPORTANT: the returned pointer points into g->nodes[] and is INVALIDATED
 * by any subsequent node insertion (the array may be reallocated and move).
 * Use the pointer immediately or re-look-up by id. This is the classic C
 * dynamic-array pitfall — Java references never move, C pointers into a
 * resizable array do.
 *
 * Growth strategy mirrors graf.c's graf_ensure_capacity: capacity doubles,
 * starting from 16 if the array was never allocated.
 *
 * Java analogy: Map.computeIfAbsent(id, k -> new Node(k)).
 */
static inline t_graf_node *graf_ensure_node(t_graf *g, t_symbol *id, long *created)
{
    t_graf_node *n = graf_find_node(g, id);

    if (created) *created = 0;
    if (n) return n;

    /* grow the nodes array if needed (double, like Java ArrayList) */
    if (g->node_count >= g->node_capacity) {
        long newcap = (g->node_capacity > 0) ? g->node_capacity * 2 : 16;
        t_graf_node *grown;

        if (g->nodes == NULL)
            grown = (t_graf_node *)sysmem_newptr(newcap * sizeof(t_graf_node));
        else
            grown = (t_graf_node *)sysmem_resizeptr(g->nodes,
                                                    newcap * sizeof(t_graf_node));
        if (!grown) return NULL;

        g->nodes         = grown;
        g->node_capacity = newcap;
    }

    n                = &g->nodes[g->node_count];
    n->id            = id;
    n->payload       = NULL;
    n->payload_count = 0;
    n->edges_to      = NULL;    /* allocated lazily on first edge */
    n->edge_weights  = NULL;
    n->edge_count    = 0;

    g->node_count++;
    if (created) *created = 1;
    return n;
}

/**
 * graf_increment_edge — add `amount` to the weight of edge u->v,
 * creating the edge (initial weight = amount) if it does not exist.
 *
 * Both u and v must already exist as nodes (call graf_ensure_node first).
 *
 * Returns the NEW weight of the edge on success, or -1.0 on error
 * (missing node, or allocation failure). -1.0 is unambiguous as an error
 * sentinel here because observed counts are always >= 0 — but note this
 * helper is general: nothing stops a caller passing a negative amount,
 * in which case the sentinel would be ambiguous. graf.observe only ever
 * passes +1.0.
 *
 * This is THE core primitive of graf.observe: edge weight as raw transition
 * count. Counts (not probabilities) are stored so that observing is additive
 * across sessions — record more material later and the counts just grow.
 *
 * Java analogy: edgeWeights.merge(v, amount, Double::sum).
 */
static inline double graf_increment_edge(t_graf *g, t_symbol *u,
                                         t_symbol *v, double amount)
{
    t_graf_node *src = graf_find_node(g, u);
    t_graf_node *dst = graf_find_node(g, v);
    long i;

    if (!src || !dst) return -1.0;

    // existing edge — just accumulate
    for (i = 0; i < src->edge_count; i++) {
        if (src->edges_to[i] == v) {
            src->edge_weights[i] += amount;
            return src->edge_weights[i];
        }
    }

    // new edge — grow the parallel arrays (mirrors graf_addedge_quiet)
    {
        long new_count = src->edge_count + 1;

        if (src->edges_to == NULL) {
            src->edges_to     = (t_symbol **)sysmem_newptr(
                                 new_count * sizeof(t_symbol *));
            src->edge_weights = (double *)   sysmem_newptr(
                                 new_count * sizeof(double));
        } else {
            src->edges_to     = (t_symbol **)sysmem_resizeptr(
                                 src->edges_to, new_count * sizeof(t_symbol *));
            src->edge_weights = (double *)   sysmem_resizeptr(
                                 src->edge_weights, new_count * sizeof(double));
        }
        if (!src->edges_to || !src->edge_weights) return -1.0;

        src->edges_to[src->edge_count]     = v;
        src->edge_weights[src->edge_count] = amount;
        src->edge_count++;
        return amount;
    }
}