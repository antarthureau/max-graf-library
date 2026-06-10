/**
    @file
    graf - directed weighted graph data structure for Max/MSP
    antoine hureau-parreira

    Message interface:
        addnode [id] [payload...]   — add a node (id optional, auto-increments if omitted)
        removenode <id>             — remove a node and all its edges
        addedge <u> <v> [weight]   — add directed edge u->v (weight defaults to 0.0)
        removeedge <u> <v>         — remove directed edge u->v
        goto <id>                  — set current node (teleport, ignores edges)
        next                       — move to a random neighbour of current node
        bang                       — output current node id + payload
        hasnode <id>               — output 1 if node exists, 0 otherwise
        neighbours <id>            — output all direct neighbours of a node
        adjacent <u> <v>           — output 1 if edge u->v exists, 0 otherwise
        size                       — output number of nodes
        print                      — dump full graph to Max console

    Instance naming:
        graf my_graph              — named instance, referenceable by other objects
        graf                       — auto-named (graf_0, graf_1, ...), still functional

    Basic sequencer usage:
        - addnode to build states (nodes carry pitch/duration/velocity as payload)
        - addedge to define possible transitions
        - goto to manually place the playhead at any node
        - next to let the graph autonomously move to a neighbour
        - bang to trigger the current state (outputs to outlet)
*/

#include "ext.h"
#include "ext_obex.h"
#include <string.h>

/* Initial capacity for the nodes array.
   Will double automatically when exceeded (like Java ArrayList). */
#define GRAF_INIT_CAPACITY 16

/* Prefix used for auto-generated instance names when user doesn't specify one */
#define GRAF_AUTO_NAME_PREFIX "graf_"

/* Prefix used for auto-generated node IDs when user doesn't specify one */
#define GRAF_AUTO_NODE_PREFIX "node"

/* Global counter for auto-naming graf instances */
static long graf_instance_count = 0;


////////////////////////// data structures

/**
 * A single node in the graph.
 * Equivalent to a vertex V in IGraph<V>.
 *
 * The node holds:
 *   - an identifier (t_symbol*, compared by pointer like .equals() in Java)
 *   - an optional payload (any sequence of Max atoms: ints, floats, symbols)
 *   - a dynamic list of outgoing directed edges with weights
 */
typedef struct _graf_node {
    t_symbol   *id;             // node identifier
    t_atom     *payload;        // optional data attached to this node
    long        payload_count;
    t_symbol  **edges_to;       // array of target node IDs (outgoing edges only)
    double     *edge_weights;   // weight for each outgoing edge
    long        edge_count;
} t_graf_node;

/**
 * The graf object itself.
 * Holds a dynamic array of nodes and a current traversal position.
 * Can be registered by name so other objects (e.g. graf.traverse) can find it.
 */
typedef struct _graf {
    t_object    ob;
    void       *outlet;
    t_symbol   *name;           // registered instance name
    t_graf_node *nodes;         // dynamic array (grows by doubling like ArrayList)
    long        node_count;
    long        node_capacity;
    long        next_node_id;   // auto-increment counter for anonymous nodes
    t_symbol   *current;        // currently selected node (for traversal)
} t_graf;


////////////////////////// function prototypes

void *graf_new(t_symbol *s, long argc, t_atom *argv);
void  graf_free(t_graf *x);
void  graf_assist(t_graf *x, void *b, long m, long a, char *s);

// message handlers
void  graf_bang(t_graf *x);
void  graf_addnode(t_graf *x, t_symbol *s, long argc, t_atom *argv);
void  graf_removenode(t_graf *x, t_symbol *id);
void  graf_addedge(t_graf *x, t_symbol *s, long argc, t_atom *argv);
void  graf_removeedge(t_graf *x, t_symbol *s, long argc, t_atom *argv);
void  graf_goto(t_graf *x, t_symbol *id);
void  graf_next(t_graf *x);
void  graf_hasnode(t_graf *x, t_symbol *id);
void  graf_neighbours(t_graf *x, t_symbol *id);
void  graf_adjacent(t_graf *x, t_symbol *s, long argc, t_atom *argv);
void  graf_size(t_graf *x);
void  graf_print(t_graf *x);

// internal helpers
t_graf_node *graf_find_node(t_graf *x, t_symbol *id);
long         graf_find_node_index(t_graf *x, t_symbol *id);
void         graf_ensure_capacity(t_graf *x);

// global class pointer
void *graf_class;


////////////////////////// main and class registration

void ext_main(void *r)
{
    t_class *c;

    c = class_new("graf",
                  (method)graf_new,
                  (method)graf_free,
                  (long)sizeof(t_graf),
                  0L, A_GIMME, 0);

    // inlet/outlet tooltip
    class_addmethod(c, (method)graf_assist,     "assist",      A_CANT,  0);

    // output current node
    class_addmethod(c, (method)graf_bang,        "bang",        0);

    // graph structure operations
    class_addmethod(c, (method)graf_addnode,     "addnode",     A_GIMME, 0);
    class_addmethod(c, (method)graf_removenode,  "removenode",  A_SYM,   0);
    class_addmethod(c, (method)graf_addedge,     "addedge",     A_GIMME, 0);
    class_addmethod(c, (method)graf_removeedge,  "removeedge",  A_GIMME, 0);

    // traversal
    class_addmethod(c, (method)graf_goto,        "goto",        A_SYM,   0);
    class_addmethod(c, (method)graf_next,        "next",        0);

    // queries
    class_addmethod(c, (method)graf_hasnode,     "hasnode",     A_SYM,   0);
    class_addmethod(c, (method)graf_neighbours,  "neighbours",  A_SYM,   0);
    class_addmethod(c, (method)graf_adjacent,    "adjacent",    A_GIMME, 0);
    class_addmethod(c, (method)graf_size,        "size",        0);
    class_addmethod(c, (method)graf_print,       "print",       0);

    class_register(CLASS_BOX, c);
    graf_class = c;

    post("graf: loaded");
}


////////////////////////// internal helper functions

/**
 * Find a node by its symbol ID.
 * t_symbol pointer equality is valid in Max because gensym() interns all symbols —
 * the same string always returns the same pointer. Equivalent to .equals() in Java.
 * Returns NULL if not found (like hasVertice() returning false).
 */
t_graf_node *graf_find_node(t_graf *x, t_symbol *id)
{
    long i;
    for (i = 0; i < x->node_count; i++) {
        if (x->nodes[i].id == id)
            return &x->nodes[i];
    }
    return NULL;
}

/**
 * Find a node's index in the nodes array.
 * Returns -1 if not found.
 * Used when we need to shift the array after removal.
 */
long graf_find_node_index(t_graf *x, t_symbol *id)
{
    long i;
    for (i = 0; i < x->node_count; i++) {
        if (x->nodes[i].id == id)
            return i;
    }
    return -1;
}

/**
 * Grow the nodes array if we've hit capacity.
 * Doubles capacity each time — same strategy as Java's ArrayList.
 * sysmem_resizeptr is Max's equivalent of realloc().
 */
void graf_ensure_capacity(t_graf *x)
{
    if (x->node_count >= x->node_capacity) {
        x->node_capacity *= 2;
        x->nodes = (t_graf_node *)sysmem_resizeptr(x->nodes,
                    x->node_capacity * sizeof(t_graf_node));
    }
}


////////////////////////// object lifecycle

/**
 * Constructor — called when the user creates a [graf] or [graf my_name] object.
 *
 * If a name argument is given, the instance is registered under that name
 * so other objects can find it via object_findregistered().
 * If no name is given, an auto-name like "graf_0", "graf_1" is assigned.
 */
void *graf_new(t_symbol *s, long argc, t_atom *argv)
{
    t_graf *x = (t_graf *)object_alloc(graf_class);
    if (!x) return NULL;

    // determine instance name
    if (argc > 0 && atom_gettype(argv) == A_SYM) {
        // user provided a name: [graf my_graph]
        x->name = atom_getsym(argv);
    } else {
        // auto-generate a name: graf_0, graf_1, ...
        char auto_name[64];
        snprintf(auto_name, sizeof(auto_name), "%s%ld",
                 GRAF_AUTO_NAME_PREFIX, graf_instance_count++);
        x->name = gensym(auto_name);
    }

    // register this instance so other objects can find it by name
    object_register(CLASS_BOX, x->name, x);

    // initialize node storage
    x->node_count    = 0;
    x->node_capacity = GRAF_INIT_CAPACITY;
    x->next_node_id  = 0;
    x->nodes         = (t_graf_node *)sysmem_newptr(
                        x->node_capacity * sizeof(t_graf_node));
    x->current       = NULL;

    // single outlet: outputs node id + payload on bang, query results otherwise
    x->outlet = outlet_new(x, NULL);

    post("graf: created instance '%s'", x->name->s_name);
    return x;
}

/**
 * Destructor — free all dynamically allocated memory.
 * Each node owns its payload and edge arrays, so we free those first,
 * then free the nodes array itself.
 */
void graf_free(t_graf *x)
{
    long i;

    // unregister from the named object registry
    object_unregister(x);

    // free each node's internal arrays
    for (i = 0; i < x->node_count; i++) {
        t_graf_node *n = &x->nodes[i];
        if (n->payload)      sysmem_freeptr(n->payload);
        if (n->edges_to)     sysmem_freeptr(n->edges_to);
        if (n->edge_weights) sysmem_freeptr(n->edge_weights);
    }

    // free the nodes array itself
    if (x->nodes) sysmem_freeptr(x->nodes);
}

void graf_assist(t_graf *x, void *b, long m, long a, char *s)
{
    if (m == ASSIST_INLET)
        sprintf(s, "addnode, removenode, addedge, removeedge, goto, next, hasnode, neighbours, adjacent, size, print, bang");
    else
        sprintf(s, "node output: id + payload");
}


////////////////////////// message handlers and operations

/**
 * addnode [id] [payload...]
 *
 * Add a node to the graph.
 * - If no id is given, auto-assigns "node0", "node1", etc.
 * - If an id is given as first argument, uses that.
 * - Any additional arguments become the node's payload (any Max atoms).
 *
 * Examples:
 *   addnode              -> node0 (no payload)
 *   addnode foo          -> foo (no payload)
 *   addnode foo 60 0.5   -> foo with payload [60, 0.5]
 */
void graf_addnode(t_graf *x, t_symbol *s, long argc, t_atom *argv)
{
    t_symbol *id;

    if (argc < 1) {
        // no arguments: auto-generate id
        char auto_id[64];
        snprintf(auto_id, sizeof(auto_id), "%s%ld",
                 GRAF_AUTO_NODE_PREFIX, x->next_node_id++);
        id = gensym(auto_id);
    } else {
        // first argument is the node id
        if (atom_gettype(argv) == A_SYM) {
            id = atom_getsym(argv);
        } else {
            // numeric id: convert to symbol so pointer equality still works
            char id_str[64];
            snprintf(id_str, sizeof(id_str), "%ld", atom_getlong(argv));
            id = gensym(id_str);
        }
    }

    // check for duplicates (like hasVertice() in Java)
    if (graf_find_node(x, id)) {
        object_warn((t_object *)x, "addnode: node '%s' already exists", id->s_name);
        return;
    }

    graf_ensure_capacity(x);

    // initialize the new node at the end of the array
    t_graf_node *n  = &x->nodes[x->node_count];
    n->id           = id;
    n->edge_count   = 0;
    n->edges_to     = NULL;     // allocated lazily when first edge is added
    n->edge_weights = NULL;

    // store payload: all atoms after the id argument
    long payload_start = (argc < 1) ? 0 : 1;
    long payload_count = argc - payload_start;

    if (payload_count > 0) {
        n->payload_count = payload_count;
        n->payload = (t_atom *)sysmem_newptr(payload_count * sizeof(t_atom));
        sysmem_copyptr(argv + payload_start, n->payload,
                       payload_count * sizeof(t_atom));
    } else {
        n->payload       = NULL;
        n->payload_count = 0;
    }

    x->node_count++;
    post("graf: added node '%s'", id->s_name);
}

/**
 * removenode <id>
 *
 * Remove a node and clean up:
 *   1. Free the node's own memory (payload, edge arrays)
 *   2. Remove all edges pointing TO this node from other nodes
 *   3. Shift the nodes array to fill the gap
 *   4. Clear current pointer if it was pointing to this node
 */
void graf_removenode(t_graf *x, t_symbol *id)
{
    long idx = graf_find_node_index(x, id);
    if (idx < 0) {
        object_error((t_object *)x, "removenode: node '%s' not found", id->s_name);
        return;
    }

    // free node's own memory
    t_graf_node *n = &x->nodes[idx];
    if (n->payload)      sysmem_freeptr(n->payload);
    if (n->edges_to)     sysmem_freeptr(n->edges_to);
    if (n->edge_weights) sysmem_freeptr(n->edge_weights);

    // scan all other nodes and remove any edges pointing to the deleted node
    long i, j;
    for (i = 0; i < x->node_count; i++) {
        if (i == idx) continue;
        t_graf_node *other = &x->nodes[i];
        for (j = 0; j < other->edge_count; j++) {
            if (other->edges_to[j] == id) {
                // shift remaining edges left to fill the gap
                long k;
                for (k = j; k < other->edge_count - 1; k++) {
                    other->edges_to[k]     = other->edges_to[k + 1];
                    other->edge_weights[k] = other->edge_weights[k + 1];
                }
                other->edge_count--;
                j--; // re-check this index after shift
            }
        }
    }

    // shift the nodes array to fill the removed slot
    long shift;
    for (shift = idx; shift < x->node_count - 1; shift++)
        x->nodes[shift] = x->nodes[shift + 1];
    x->node_count--;

    // clear current pointer if it was this node
    if (x->current == id) x->current = NULL;

    post("graf: removed node '%s'", id->s_name);
}

/**
 * addedge <u> <v> [weight]
 *
 * Add a directed edge from node u to node v with an optional weight.
 * Default weight is 0.0 (no cost to traverse).
 * Both nodes must already exist.
 * Duplicate edges are rejected.
 *
 * Examples:
 *   addedge a b        -> a->b weight 0.0
 *   addedge a b 0.5    -> a->b weight 0.5
 */
void graf_addedge(t_graf *x, t_symbol *s, long argc, t_atom *argv)
{
    if (argc < 2) {
        object_error((t_object *)x, "addedge: requires source and target node ids");
        return;
    }

    t_symbol *u      = atom_getsym(argv);
    t_symbol *v      = atom_getsym(argv + 1);
    double    weight = (argc >= 3) ? atom_getfloat(argv + 2) : 0.0;

    t_graf_node *src = graf_find_node(x, u);
    t_graf_node *dst = graf_find_node(x, v);

    if (!src) { object_error((t_object *)x, "addedge: source '%s' not found", u->s_name); return; }
    if (!dst) { object_error((t_object *)x, "addedge: target '%s' not found", v->s_name); return; }

    // check for duplicate edge
    long i;
    for (i = 0; i < src->edge_count; i++) {
        if (src->edges_to[i] == v) {
            object_warn((t_object *)x, "addedge: edge '%s'->'%s' already exists",
                        u->s_name, v->s_name);
            return;
        }
    }

    // grow edge arrays — allocate on first edge, resize on subsequent ones
    long new_count = src->edge_count + 1;
    if (src->edges_to == NULL) {
        src->edges_to     = (t_symbol **)sysmem_newptr(new_count * sizeof(t_symbol *));
        src->edge_weights = (double *)   sysmem_newptr(new_count * sizeof(double));
    } else {
        src->edges_to     = (t_symbol **)sysmem_resizeptr(src->edges_to,
                             new_count * sizeof(t_symbol *));
        src->edge_weights = (double *)   sysmem_resizeptr(src->edge_weights,
                             new_count * sizeof(double));
    }

    src->edges_to[src->edge_count]     = v;
    src->edge_weights[src->edge_count] = weight;
    src->edge_count++;

    post("graf: added edge '%s' -> '%s' (weight: %.2f)", u->s_name, v->s_name, weight);
}

/**
 * removeedge <u> <v>
 *
 * Remove the directed edge from u to v.
 * Note: this only removes u->v, not v->u (directed graph).
 */
void graf_removeedge(t_graf *x, t_symbol *s, long argc, t_atom *argv)
{
    if (argc < 2) {
        object_error((t_object *)x, "removeedge: requires source and target node ids");
        return;
    }

    t_symbol *u = atom_getsym(argv);
    t_symbol *v = atom_getsym(argv + 1);

    t_graf_node *src = graf_find_node(x, u);
    if (!src) {
        object_error((t_object *)x, "removeedge: source '%s' not found", u->s_name);
        return;
    }

    long i;
    for (i = 0; i < src->edge_count; i++) {
        if (src->edges_to[i] == v) {
            // shift remaining edges left
            long k;
            for (k = i; k < src->edge_count - 1; k++) {
                src->edges_to[k]     = src->edges_to[k + 1];
                src->edge_weights[k] = src->edge_weights[k + 1];
            }
            src->edge_count--;
            post("graf: removed edge '%s' -> '%s'", u->s_name, v->s_name);
            return;
        }
    }
    object_error((t_object *)x, "removeedge: edge '%s'->'%s' not found",
                 u->s_name, v->s_name);
}

/**
 * goto <id>
 *
 * Set the current traversal position to the specified node.
 * This is a direct jump — edges are ignored, any node can be reached.
 * Think of it as the composer manually placing the playhead.
 * After this, bang will output that node's id and payload.
 */
void graf_goto(t_graf *x, t_symbol *id)
{
    if (!graf_find_node(x, id)) {
        object_error((t_object *)x, "goto: node '%s' not found", id->s_name);
        return;
    }
    x->current = id;
    post("graf: current -> '%s'", id->s_name);
}

/**
 * next
 *
 * Move to a random neighbour of the current node following a directed edge.
 * This is the simplest form of autonomous traversal — uniform random choice
 * among all outgoing edges, ignoring weights.
 *
 * After moving, outputs the new current node's id and payload (like bang).
 *
 * Errors if:
 *   - no current node is set (use goto first)
 *   - current node has no outgoing edges (dead end)
 *
 * Note: weight-aware traversal will be handled by graf.traverse later.
 */
void graf_next(t_graf *x)
{
    if (!x->current) {
        object_error((t_object *)x, "next: no current node — use 'goto' first");
        return;
    }

    t_graf_node *n = graf_find_node(x, x->current);
    if (!n) return;

    if (n->edge_count == 0) {
        object_error((t_object *)x, "next: node '%s' has no outgoing edges (dead end)",
                     x->current->s_name);
        return;
    }

    // uniform random choice among outgoing edges
    // rand() % n gives a value in [0, n-1]
    long chosen = (long)(rand() % n->edge_count);
    x->current = n->edges_to[chosen];

    // output the new current node immediately (like bang)
    t_graf_node *next_node = graf_find_node(x, x->current);
    if (!next_node) return;

    outlet_anything(x->outlet, next_node->id,
                    next_node->payload_count,
                    next_node->payload_count > 0 ? next_node->payload : NULL);
}

/**
 * hasnode <id>
 *
 * Output 1 if a node with the given id exists, 0 otherwise.
 * Equivalent to IGraph.hasVertice() in Java.
 */
void graf_hasnode(t_graf *x, t_symbol *id)
{
    t_atom result;
    atom_setlong(&result, graf_find_node(x, id) ? 1 : 0);
    outlet_anything(x->outlet, gensym("hasnode"), 1, &result);
}

/**
 * bang
 *
 * Output the current node's id and payload to the outlet.
 * Output format: the node id is the message selector,
 * and the payload atoms follow as arguments.
 *
 * Example outputs:
 *   node has no payload        -> symbol "foo" with no args
 *   node has payload 60 0.5    -> symbol "foo" with args [60, 0.5]
 */
void graf_bang(t_graf *x)
{
    if (!x->current) {
        object_error((t_object *)x, "bang: no current node — use 'goto' first");
        return;
    }

    t_graf_node *n = graf_find_node(x, x->current);
    if (!n) return;

    outlet_anything(x->outlet, n->id,
                    n->payload_count,
                    n->payload_count > 0 ? n->payload : NULL);
}

/**
 * neighbours <id>
 *
 * Output each direct neighbour of the given node as a separate message.
 * Each output is: "neighbour <node_id>"
 * Equivalent to IGraph.neighbours() in Java.
 */
void graf_neighbours(t_graf *x, t_symbol *id)
{
    t_graf_node *n = graf_find_node(x, id);
    if (!n) {
        object_error((t_object *)x, "neighbours: node '%s' not found", id->s_name);
        return;
    }

    if (n->edge_count == 0) {
        post("graf: node '%s' has no neighbours", id->s_name);
        return;
    }

    long i;
    t_atom a;
    for (i = 0; i < n->edge_count; i++) {
        atom_setsym(&a, n->edges_to[i]);
        outlet_anything(x->outlet, gensym("neighbour"), 1, &a);
    }
}

/**
 * adjacent <u> <v>
 *
 * Output 1 if there is a directed edge u->v, 0 otherwise.
 * Equivalent to IGraph.adjacent() in Java.
 * Note: only checks u->v, not v->u (directed graph).
 */
void graf_adjacent(t_graf *x, t_symbol *s, long argc, t_atom *argv)
{
    if (argc < 2) {
        object_error((t_object *)x, "adjacent: requires two node ids");
        return;
    }

    t_symbol *u = atom_getsym(argv);
    t_symbol *v = atom_getsym(argv + 1);

    t_graf_node *src = graf_find_node(x, u);
    if (!src) {
        object_error((t_object *)x, "adjacent: node '%s' not found", u->s_name);
        return;
    }

    t_atom result;
    long i;
    for (i = 0; i < src->edge_count; i++) {
        if (src->edges_to[i] == v) {
            atom_setlong(&result, 1);
            outlet_anything(x->outlet, gensym("adjacent"), 1, &result);
            return;
        }
    }
    atom_setlong(&result, 0);
    outlet_anything(x->outlet, gensym("adjacent"), 1, &result);
}

/**
 * size
 *
 * Output the number of nodes currently in the graph.
 * Equivalent to IGraph.size() in Java.
 */
void graf_size(t_graf *x)
{
    t_atom a;
    atom_setlong(&a, x->node_count);
    outlet_anything(x->outlet, gensym("size"), 1, &a);
}

/**
 * print
 *
 * Dump the entire graph structure to the Max console.
 * For each node: shows its id and all outgoing edges with weights.
 * Also shows the current traversal position.
 */
void graf_print(t_graf *x)
{
    long i, j;
    post("graf '%s': --- graph (%ld nodes) ---", x->name->s_name, x->node_count);

    for (i = 0; i < x->node_count; i++) {
        t_graf_node *n = &x->nodes[i];

        // build edge list string
        char edge_str[512] = "";
        for (j = 0; j < n->edge_count; j++) {
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "'%s'(%.2f) ",
                     n->edges_to[j]->s_name, n->edge_weights[j]);
            strncat(edge_str, tmp, sizeof(edge_str) - strlen(edge_str) - 1);
        }

        post("  node '%s' -> [%s]",
             n->id->s_name,
             n->edge_count > 0 ? edge_str : "no edges");
    }

    post("  current: %s",
         x->current ? x->current->s_name : "(none)");
}