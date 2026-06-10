/**
 * graf.h — Shared type definitions for the graf external family.
 *
 * Include this header in any external that needs direct read access to
 * graf internals: graf.traverse, graf.visualizer, etc.
 *
 * THIS IS THE CANONICAL DEFINITION of t_graf_node and t_graf.
 * Remove those struct definitions from graf.c and include this instead.
 *
 * If you change these structs, rebuild all externals in the family.
 *
 * C/Java note: there is no true header/interface system in C. This header
 * is the closest equivalent to a Java interface that both graf.c and
 * graf.traverse.c "implement" — except here we're sharing data layout
 * rather than method signatures.
 */

#ifndef GRAF_H
#define GRAF_H

#include "ext.h"
#include "ext_obex.h"

//////////////////////////////////////////////////////////////////////////
// t_graf_node — one node in the directed graph

/**
 * Represents a single node.
 *
 * Java equivalent: one entry V in AdjacencySet's vertices Set, plus
 * its row in the adjacencyList Map — but here directed and weighted.
 *
 * Memory: edges_to and edge_weights are parallel dynamic arrays
 * (like two ArrayLists kept in sync). Allocated lazily on first edge.
 * payload is a dynamic t_atom array (Max's variant type).
 */
typedef struct _graf_node {
    t_symbol   *id;             // node identifier — interned by Max, use pointer equality
    t_atom     *payload;        // optional Max atoms attached to this node (may be NULL)
    long        payload_count;
    t_symbol  **edges_to;       // dynamic array of outgoing edge target IDs (may be NULL)
    double     *edge_weights;   // weight per edge, parallel to edges_to (default 0.0)
    long        edge_count;
} t_graf_node;

//////////////////////////////////////////////////////////////////////////
// t_graf — the graph object itself

/**
 * The Max object struct for [graf].
 *
 * Java equivalent: AdjacencySet<V>, but directed, weighted, and
 * embedded in a Max object rather than a plain Java class.
 *
 * The t_object header MUST be first — Max uses it as a vtable /
 * object header, like how every Java class implicitly extends Object.
 *
 * nodes is a flat dynamic array of t_graf_node structs (not pointers),
 * like an ArrayList<Node> where Node is a value type (C struct by value).
 * Capacity doubles on overflow, same as Java's ArrayList growth strategy.
 */
typedef struct _graf {
    t_object    ob;             // MUST be first — Max object header
    void       *outlet;         // single outlet: node id + payload on bang/next
    t_symbol   *name;           // registered name for object_findregistered lookup
    t_graf_node *nodes;         // dynamic flat array of nodes
    long        node_count;     // number of nodes currently in use
    long        node_capacity;  // total allocated slots in nodes array
    long        next_node_id;   // auto-increment counter for anonymous node IDs
    t_symbol   *current;        // current traversal position (NULL if unset)
} t_graf;

//////////////////////////////////////////////////////////////////////////
// Shared inline helpers
//
// These are static inline — each translation unit that includes this
// header gets its own copy of the compiled function. In Java terms,
// think of these as static utility methods on a helper class, inlined
// by the JIT. The 'static' keyword here means "local to this translation
// unit" (not global), preventing linker errors from duplicate symbols.

/**
 * graf_find — look up a named graf instance from any external.
 *
 * Uses Max's object registration system. [graf name] calls
 * object_register(CLASS_BOX, name, self) on creation and
 * object_unregister(self) on deletion.
 *
 * Java equivalent: a named object registry lookup, like
 *   registry.get("my_graph")
 * but typed as t_graf* via an explicit cast (Max's registry is untyped).
 *
 * @param name  The symbol name registered by [graf name]
 * @return      Pointer to the t_graf, or NULL if not found
 */
static inline t_graf *graf_find(t_symbol *name)
{
    return (t_graf *)object_findregistered(CLASS_BOX, name);
}

/**
 * graf_find_node — find a node in a graf by its ID symbol. O(n) scan.
 *
 * Safe for sequencer-scale graphs (typically < 100 nodes).
 * t_symbol* pointer equality is valid: Max interns all symbols via
 * gensym() so the same string always maps to the same pointer address —
 * like Java's String.intern(), except it's guaranteed for all t_symbol*.
 *
 * Java equivalent: adjacencyList.get(id) in AdjacencySet.
 *
 * @param g   The graf instance to search
 * @param id  The node's symbol ID
 * @return    Pointer to the t_graf_node in place (NOT a copy), or NULL
 */
static inline t_graf_node *graf_find_node(t_graf *g, t_symbol *id)
{
    for (long i = 0; i < g->node_count; i++) {
        if (g->nodes[i].id == id)   // pointer equality — valid for t_symbol*
            return &g->nodes[i];
    }
    return NULL;
}

#endif /* GRAF_H */