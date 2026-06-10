/**
 * @file graf.h
 * Shared header for the graf external family.
 *
 * Include this in every graf.* external. Defines the canonical data structures
 * and provides two static inline lookup helpers that are compiled once per
 * translation unit (avoiding linker collisions across externals).
 *
 * Java analogy: this is like a shared interface/model package that both the
 * data-store class (graf) and its view class (graf.affiche) import.
 */

#pragma once

#include "ext.h"
#include "ext_obex.h"


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
    t_symbol   *id;             /* node identifier (interned symbol, pointer equality valid) */
    t_atom     *payload;        /* optional Max atoms attached to this node (int/float/symbol) */
    long        payload_count;
    t_symbol  **edges_to;       /* dynamic array: target node IDs for outgoing edges */
    double     *edge_weights;   /* parallel array: weight for each outgoing edge */
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
    t_symbol   *name;           /* registered instance name */
    t_graf_node *nodes;         /* dynamic array (capacity doubles on overflow) */
    long        node_count;
    long        node_capacity;
    long        next_node_id;   /* auto-increment counter for anonymous node IDs */
    t_symbol   *current;        /* current traversal position (NULL if unset) */
} t_graf;


////////////////////////// inline lookup helpers

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