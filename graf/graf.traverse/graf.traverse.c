/**
 * graf.traverse.c — Traversal algorithms over named [graf] instances.
 *
 * Part of the graf external family for Max/MSP.
 * Provides step-by-step weighted random walk, DFS, and BFS over a
 * directed weighted graph stored in a [graf] object identified by name.
 *
 * -------------------------------------------------------------------------
 * USAGE
 *   [graf.traverse my_graph]
 *     where "my_graph" matches the argument given to a [graf my_graph] object
 *     in the same patcher (or any loaded patcher).
 *
 * MESSAGES IN
 *   step                 advance one step; output new node on left outlet
 *   reset [<id> [<tgt>]] reset traversal; 1st symbol = start, 2nd = target (dijkstra)
 *   mode random          weighted random walk (default)
 *   mode dfs             depth-first search
 *   mode bfs             breadth-first search
 *   mode dijkstra        weighted shortest path from start to target
 *   from <id>            set start node for future resets (no immediate reset)
 *   to <id>              set target node for shortest-path search
 *   bang                 re-output current node without advancing
 *
 * OUTLETS
 *   0  (left)  — node id + payload on each step (symbol or list)
 *   1  (right) — bang when traversal is complete or hits a dead end
 *
 * -------------------------------------------------------------------------
 * C / JAVA MAPPING
 *
 *   This object parallels GraphSearch.java (BFS/DFS connectivity check)
 *   but is step-by-step and operates by reference on a separate t_graf
 *   found by name — rather than being constructed with a graph reference
 *   like: new GraphSearch(graph).
 *
 *   Key C patterns used here that have no direct Java equivalent:
 *
 *   - struct _graf_traverse: like a Java class, but the compiler lays out
 *     the fields sequentially in memory. No vtable unless you build one
 *     manually. Max injects the t_object header as the "base class" by
 *     requiring it to be the first field.
 *
 *   - static t_class *s_graf_traverse_class: like a Java Class<?> object,
 *     but you build it manually in ext_main() by registering methods.
 *
 *   - (method) casts: Max's method registration uses void* for all function
 *     pointers. The cast is like casting to a functional interface in Java —
 *     you're telling the compiler "trust me, this matches the signature."
 *     If the signature is wrong, it's a silent runtime crash (no generics
 *     to catch it at compile time).
 *
 *   - static inline helpers in graf.h: like Java static utility methods
 *     on a helper class. 'static' in C file scope means "private to this
 *     translation unit" — not the same as Java's static (which is per-class).
 */

#include "ext.h"
#include "ext_obex.h"
#include "graf.h"           // t_graf_node, t_graf, graf_find(), graf_find_node()
#include <math.h>           // INFINITY — Dijkstra distance initialisation


//////////////////////////////////////////////////////////////////////////
// Constants

#define GRAF_TRAVERSE_RANDOM    0   // weighted random walk (stateless)
#define GRAF_TRAVERSE_DFS       1   // depth-first search
#define GRAF_TRAVERSE_BFS       2   // breadth-first search
#define GRAF_TRAVERSE_DIJKSTRA  3   // weighted shortest path (precomputed, then walked)

#define GRAF_TRAVERSE_INIT_CAP  16  // initial capacity for visited/work arrays


//////////////////////////////////////////////////////////////////////////
// Object struct

/**
 * t_graf_traverse — state for one graf.traverse instance.
 *
 * Java equivalent: an instance of GraphSearch, but with explicit step
 * state stored as fields rather than on the call stack.
 *
 * For DFS, `work` is used as a stack: push/pop at the tail (work_count).
 * For BFS, `work` is used as a queue: enqueue at tail, dequeue from
 * work_head (which advances forward). Consumed slots are never reused
 * within a traversal — the queue grows forward. On reset, both pointers
 * return to 0 and the same memory is reused from the start.
 *
 * This means BFS work[] is used like a FIFO tape rather than a true
 * circular buffer. Acceptable because graphs in sequencer use are small
 * (< 100 nodes) — the waste is a few hundred bytes at most.
 */
typedef struct _graf_traverse {
    t_object    ob;

    void       *outlet_node;        // left outlet (created 2nd): node id + payload
    void       *outlet_done;        // right outlet (created 1st): bang when done

    t_symbol   *graf_name;          // name of the [graf] instance to traverse
    int         mode;               // GRAF_TRAVERSE_RANDOM / DFS / BFS
    t_symbol   *start;              // explicit start node; NULL = fall back to graf->current
    t_symbol   *current;            // current position in the traversal

    /* DFS / BFS state */
    t_symbol  **visited;            // set of visited node IDs (linear scan, acceptable for small n)
    long        visited_count;
    long        visited_cap;

    t_symbol  **work;               // stack (DFS) or queue tape (BFS)
    long        work_cap;           // total allocated slots
    long        work_count;         // DFS: stack depth; BFS: next enqueue index
    long        work_head;          // BFS only: dequeue pointer (never decrements)

    long        initialized;        // 1 after reset or first auto-init, 0 after mode change
    long        done;               // 1 when traversal is exhausted

    /* Dijkstra / shortest-path state.
     *
     * Dijkstra is not a step-by-step exploration like DFS/BFS — it is computed
     * in full at init time, producing a complete start->target path that 'step'
     * then walks one node at a time. So these arrays hold the SOLVER state
     * (dist/prev/settled, indexed parallel to graf->nodes) and the RESULT
     * (path[], the reconstructed node sequence).
     *
     * Java analogy: like caching the int[] result of Dijkstra.shortestPath(a, b)
     * and then iterating over it.
     */
    t_symbol   *target;             // goal node for shortest-path modes (set via 'to')
    double     *dist;               // dist[i]    = shortest known cost to graf node i
    long       *prev;               // prev[i]    = predecessor index on shortest path (-1 = none)
    char       *settled;            // settled[i] = 1 once node i's distance is final
    long        dijkstra_cap;       // allocated length of dist/prev/settled/path
    t_symbol  **path;               // reconstructed path, ordered start -> target
    long        path_len;           // number of nodes in path
    long        path_pos;           // index of next path node to emit on 'step'
} t_graf_traverse;


//////////////////////////////////////////////////////////////////////////
// Class variable

static t_class *s_graf_traverse_class = NULL;


//////////////////////////////////////////////////////////////////////////
// Prototypes

/* lifecycle */
void *graf_traverse_new(t_symbol *s, long argc, t_atom *argv);
void  graf_traverse_free(t_graf_traverse *x);

/* graf lookup */
t_graf *graf_traverse_get_graf(t_graf_traverse *x);

/* traversal state */
t_max_err graf_traverse_init(t_graf_traverse *x, t_symbol *start_id);
void      graf_traverse_clear_state(t_graf_traverse *x);
long      graf_traverse_is_visited(t_graf_traverse *x, t_symbol *id);
t_max_err graf_traverse_mark_visited(t_graf_traverse *x, t_symbol *id);
t_max_err graf_traverse_work_push(t_graf_traverse *x, t_symbol *id);
t_symbol *graf_traverse_work_pop(t_graf_traverse *x);
t_symbol *graf_traverse_work_dequeue(t_graf_traverse *x);
long      graf_traverse_work_empty(t_graf_traverse *x);

//TODO: keep track of visited nodes (hammiltonian path support?) and/or visited edges (eulerian path support?)
//TODO:

/* output */
void graf_traverse_output_node(t_graf_traverse *x, t_graf *g, t_symbol *id);

/* algorithms */
void graf_traverse_step_random(t_graf_traverse *x, t_graf *g);
void graf_traverse_step_dfs(t_graf_traverse *x, t_graf *g);
void graf_traverse_step_bfs(t_graf_traverse *x, t_graf *g);
void graf_traverse_step_dijkstra(t_graf_traverse *x, t_graf *g);

/* shortest-path solver (Dijkstra) */
static long graf_traverse_node_index(t_graf *g, t_symbol *id);
t_max_err   graf_traverse_ensure_dijkstra(t_graf_traverse *x, long n);
t_max_err   graf_traverse_compute_dijkstra(t_graf_traverse *x, t_graf *g,
                                           t_symbol *start, t_symbol *target);

/* message handlers */
void graf_traverse_step(t_graf_traverse *x);
void graf_traverse_reset(t_graf_traverse *x, t_symbol *s, long argc, t_atom *argv);
void graf_traverse_mode(t_graf_traverse *x, t_symbol *s, long argc, t_atom *argv);
void graf_traverse_from(t_graf_traverse *x, t_symbol *id);
void graf_traverse_to(t_graf_traverse *x, t_symbol *id);
void graf_traverse_bang(t_graf_traverse *x);


//////////////////////////////////////////////////////////////////////////
// ext_main — class registration

/**
 * ext_main — called once when Max loads the external.
 *
 * Equivalent to a static initializer block in Java, or registering
 * method handlers on a class object. We build the t_class by associating
 * message names with C function pointers.
 *
 * A_GIMME means "pass all arguments as (t_symbol *s, long argc, t_atom *argv)".
 * A_SYM means "the next argument is a t_symbol*".
 * 0 terminates the argument type list.
 */
void ext_main(void *r)
{
    t_class *c = class_new("graf.traverse",
                           (method)graf_traverse_new,
                           (method)graf_traverse_free,
                           sizeof(t_graf_traverse),
                           NULL,            // unused interface type (Max 5+)
                           A_GIMME,         // new() receives argc/argv for the graf name
                           0);

    class_addmethod(c, (method)graf_traverse_bang,  "bang",   0);
    class_addmethod(c, (method)graf_traverse_step,  "step",   0);
    class_addmethod(c, (method)graf_traverse_reset, "reset",  A_GIMME, 0);
    class_addmethod(c, (method)graf_traverse_mode,  "mode",   A_GIMME, 0);
    class_addmethod(c, (method)graf_traverse_from,  "from",   A_SYM,   0);
    class_addmethod(c, (method)graf_traverse_to,    "to",     A_SYM,   0);

    class_register(CLASS_BOX, c);
    s_graf_traverse_class = c;

    post("graf.traverse: loaded");
}


////////////////////////// object lifecycle

/**
 * graf_traverse_new — create a new graf.traverse instance.
 *
 * Requires exactly one symbol argument: the name of the [graf] to traverse.
 * The [graf] does not need to exist yet at creation time — it is looked up
 * on each step via object_findregistered().
 *
 * Java equivalent: new GraphSearch(String graphName) — but lazy lookup.
 *
 * Outlet ordering: Max creates outlets right-to-left visually.
 * First outlet_new call = rightmost outlet in the patcher.
 * We create outlet_done first (right = done bang),
 * then outlet_node second (left = data output).
 */
void *graf_traverse_new(t_symbol *s, long argc, t_atom *argv)
{
    t_graf_traverse *x = (t_graf_traverse *)object_alloc(s_graf_traverse_class);
    if (!x) return NULL;

    /* Validate argument: need exactly one symbol (the graf name) */
    if (argc < 1 || atom_gettype(argv) != A_SYM) {
        object_error((t_object *)x,
                     "graf.traverse: requires a graf name as argument. "
                     "Usage: [graf.traverse my_graph]");
        object_free(x);
        return NULL;
    }

    /* Outlets: right-to-left creation order */
    x->outlet_done = bangout(x);                // right outlet: bang when traversal done
    x->outlet_node = outlet_new(x, NULL);       // left outlet: node id + payload

    /* Initialize all fields */
    x->graf_name     = atom_getsym(argv);
    x->mode          = GRAF_TRAVERSE_RANDOM;
    x->start         = NULL;
    x->current       = NULL;
    x->visited       = NULL;
    x->visited_count = 0;
    x->visited_cap   = 0;
    x->work          = NULL;
    x->work_cap      = 0;
    x->work_count    = 0;
    x->work_head     = 0;
    x->initialized   = 0;
    x->done          = 0;

    x->target        = NULL;
    x->dist          = NULL;
    x->prev          = NULL;
    x->settled       = NULL;
    x->dijkstra_cap  = 0;
    x->path          = NULL;
    x->path_len      = 0;
    x->path_pos      = 0;

    return x;
}

/**
 * graf_traverse_free — called by Max when this object is deleted.
 *
 * Java equivalent: finalize() / AutoCloseable.close() — but actually
 * guaranteed to be called in C (no GC uncertainty).
 *
 * We only free what we allocated: visited and work arrays.
 * Max handles freeing the outlets and the object struct itself.
 */
void graf_traverse_free(t_graf_traverse *x)
{
    if (x->visited) { sysmem_freeptr(x->visited); x->visited = NULL; }
    if (x->work)    { sysmem_freeptr(x->work);    x->work    = NULL; }
    if (x->dist)    { sysmem_freeptr(x->dist);    x->dist    = NULL; }
    if (x->prev)    { sysmem_freeptr(x->prev);    x->prev    = NULL; }
    if (x->settled) { sysmem_freeptr(x->settled); x->settled = NULL; }
    if (x->path)    { sysmem_freeptr(x->path);    x->path    = NULL; }
}


////////////////////////// graph lookup

/**
 * graf_traverse_get_graf — find the target [graf] instance by registered name.
 *
 * Called on every step/reset so that the [graf] object can be freely
 * deleted and recreated in the patcher without breaking this object.
 *
 * Java equivalent: serviceLocator.get("my_graph"), cast to Graf.
 * The cast (t_graf *) is safe as long as the registered object is
 * actually a [graf] instance — Max's registry is untyped (void*).
 *
 * @return  Pointer to t_graf, or NULL with error message if not found
 */
t_graf *graf_traverse_get_graf(t_graf_traverse *x)
{
    t_graf *g = graf_find(x->graf_name);
    if (!g) {
        object_error((t_object *)x,
                     "graf.traverse: no [graf] named '%s' found",
                     x->graf_name->s_name);
    }
    return g;
}


////////////////////////// travel manager

/**
 * graf_traverse_clear_state — reset traversal counters without freeing memory.
 *
 * Resets visited_count, work_count, work_head, initialized, and done to zero.
 * Keeps the allocated arrays in place so that subsequent inits reuse them.
 * This avoids repeated malloc/free cycles across resets.
 *
 * For BFS: both work_count (next enqueue index) and work_head (dequeue pointer)
 * return to 0, so the queue tape is effectively rewound. Old data in the array
 * is silently overwritten as new items are enqueued.
 */
void graf_traverse_clear_state(t_graf_traverse *x)
{
    x->visited_count = 0;
    x->work_count    = 0;
    x->work_head     = 0;
    x->path_len      = 0;
    x->path_pos      = 0;
    x->initialized   = 0;
    x->done          = 0;
    /* current is NOT reset here — keep last known position for bang */
}

/**
 * graf_traverse_init — initialize traversal from a start node.
 *
 * Resolves start node by priority:
 *   1. start_id argument (if not NULL) — explicitly provided by reset message
 *   2. x->start — last set via 'from' message or a prior 'reset <id>'
 *   3. g->current — the [graf] object's own current position
 *
 * Mode-specific setup:
 *   RANDOM: stateless — just sets x->current. No arrays needed.
 *   DFS:    pushes start onto work stack. Marks visited on pop (not here).
 *   BFS:    enqueues start and marks it visited immediately (prevents
 *           start from being enqueued again from a neighbor's perspective).
 *
 * Java equivalent: GraphSearch.connected() setup — but here deferred
 * to be step-by-step rather than run to completion.
 *
 * @param start_id  Explicit start node symbol, or NULL to use fallback chain
 * @return          MAX_ERR_NONE on success, MAX_ERR_GENERIC on failure
 */
t_max_err graf_traverse_init(t_graf_traverse *x, t_symbol *start_id)
{
    t_graf *g = graf_traverse_get_graf(x);
    if (!g) return MAX_ERR_GENERIC;

    /* Resolve start node */
    t_symbol *sid = start_id;
    if (!sid) sid = x->start;
    if (!sid) sid = g->current;
    if (!sid) {
        object_error((t_object *)x,
                     "graf.traverse: no start node set and [graf '%s'] has no current position. "
                     "Use 'from <id>' or 'reset <id>' to specify a start node.",
                     x->graf_name->s_name);
        return MAX_ERR_GENERIC;
    }

    /* Verify start node exists in the graf */
    if (!graf_find_node(g, sid)) {
        object_error((t_object *)x,
                     "graf.traverse: start node '%s' not found in [graf '%s']",
                     sid->s_name, x->graf_name->s_name);
        return MAX_ERR_GENERIC;
    }

    graf_traverse_clear_state(x);
    x->current = sid;

    /* Random walk is stateless — no arrays needed */
    if (x->mode == GRAF_TRAVERSE_RANDOM) {
        x->initialized = 1;
        return MAX_ERR_NONE;
    }

    /*
     * Dijkstra is computed in full here, not stepped. It needs a target node.
     * Resolve and validate it, run the solver, store the resulting path.
     * 'step' then walks path[] node by node.
     */
    if (x->mode == GRAF_TRAVERSE_DIJKSTRA) {
        if (!x->target) {
            object_error((t_object *)x,
                         "graf.traverse: dijkstra needs a target — use 'to <id>' "
                         "or 'reset <start> <target>'");
            return MAX_ERR_GENERIC;
        }
        if (!graf_find_node(g, x->target)) {
            object_error((t_object *)x,
                         "graf.traverse: target node '%s' not found in [graf '%s']",
                         x->target->s_name, x->graf_name->s_name);
            return MAX_ERR_GENERIC;
        }
        if (graf_traverse_compute_dijkstra(x, g, sid, x->target) != MAX_ERR_NONE)
            return MAX_ERR_GENERIC;
        x->initialized = 1;
        return MAX_ERR_NONE;
    }

    /* Allocate visited array if needed (or reuse existing) */
    if (!x->visited) {
        x->visited = (t_symbol **)sysmem_newptr(GRAF_TRAVERSE_INIT_CAP * sizeof(t_symbol *));
        if (!x->visited) {
            object_error((t_object *)x, "graf.traverse: out of memory allocating visited set");
            return MAX_ERR_OUT_OF_MEM;
        }
        x->visited_cap = GRAF_TRAVERSE_INIT_CAP;
    }

    /* Allocate work array if needed (or reuse existing) */
    if (!x->work) {
        x->work = (t_symbol **)sysmem_newptr(GRAF_TRAVERSE_INIT_CAP * sizeof(t_symbol *));
        if (!x->work) {
            object_error((t_object *)x, "graf.traverse: out of memory allocating work array");
            return MAX_ERR_OUT_OF_MEM;
        }
        x->work_cap = GRAF_TRAVERSE_INIT_CAP;
    }

    if (x->mode == GRAF_TRAVERSE_DFS) {
        /*
         * DFS: push start onto stack. Do NOT mark as visited yet.
         * Visited is marked when a node is popped and processed, so
         * that duplicate stack entries (reachable via multiple paths)
         * are correctly skipped at pop time.
         */
        graf_traverse_work_push(x, sid);

    } else { /* BFS */
        /*
         * BFS: enqueue start and immediately mark visited.
         * Marking on enqueue (not on dequeue) prevents the same node
         * from being enqueued multiple times when multiple predecessors
         * try to enqueue it.
         * Java equivalent: visited.add(u) before the while loop in bsf().
         */
        graf_traverse_work_push(x, sid);
        graf_traverse_mark_visited(x, sid);
    }

    x->initialized = 1;
    return MAX_ERR_NONE;
}

/**
 * graf_traverse_is_visited — check if a node is in the visited set.
 *
 * Linear scan — O(n). Acceptable: in sequencer graphs n < 100,
 * and this is called at most once per step per neighbor.
 *
 * Java equivalent: visited.contains(id) using HashSet (our version is
 * slower but allocates no hash table overhead for small n).
 *
 * t_symbol* pointer equality is valid — Max guarantees symbol interning.
 */
long graf_traverse_is_visited(t_graf_traverse *x, t_symbol *id)
{
    for (long i = 0; i < x->visited_count; i++) {
        if (x->visited[i] == id) return 1;
    }
    return 0;
}

/**
 * graf_traverse_mark_visited — add a node to the visited set.
 *
 * Grows the visited array by doubling capacity if full
 * (same strategy as Java's ArrayList).
 *
 * Java equivalent: visited.add(id).
 */
t_max_err graf_traverse_mark_visited(t_graf_traverse *x, t_symbol *id)
{
    if (x->visited_count >= x->visited_cap) {
        long new_cap = x->visited_cap * 2;
        t_symbol **grown = (t_symbol **)sysmem_resizeptr(x->visited,
                                         new_cap * sizeof(t_symbol *));
        if (!grown) {
            object_error((t_object *)x, "graf.traverse: out of memory growing visited set");
            return MAX_ERR_OUT_OF_MEM;
        }
        x->visited     = grown;
        x->visited_cap = new_cap;
    }
    x->visited[x->visited_count++] = id;
    return MAX_ERR_NONE;
}

/**
 * graf_traverse_work_push — push a node onto the tail of the work array.
 *
 * Used as:
 *   DFS → stack push (last in, first out via work_pop)
 *   BFS → queue enqueue (first in, first out via work_dequeue from head)
 *
 * Allocates the array on first use (when work is NULL) and doubles
 * capacity when full.
 *
 * Note: sysmem_resizeptr requires a previously-allocated pointer.
 * We use sysmem_newptr for the initial allocation and only call
 * sysmem_resizeptr for subsequent growths.
 */
t_max_err graf_traverse_work_push(t_graf_traverse *x, t_symbol *id)
{
    if (!x->work || x->work_count >= x->work_cap) {
        long new_cap = (x->work_cap > 0) ? (x->work_cap * 2) : GRAF_TRAVERSE_INIT_CAP;
        t_symbol **grown;

        if (!x->work) {
            grown = (t_symbol **)sysmem_newptr(new_cap * sizeof(t_symbol *));
        } else {
            grown = (t_symbol **)sysmem_resizeptr(x->work, new_cap * sizeof(t_symbol *));
        }

        if (!grown) {
            object_error((t_object *)x, "graf.traverse: out of memory growing work array");
            return MAX_ERR_OUT_OF_MEM;
        }
        x->work     = grown;
        x->work_cap = new_cap;
    }
    x->work[x->work_count++] = id;
    return MAX_ERR_NONE;
}

/**
 * graf_traverse_work_pop — pop from the tail of the work array (DFS stack).
 *
 * Java equivalent: stack.pop() in an iterative DFS.
 * Returns NULL if the stack is empty (caller checks).
 */
t_symbol *graf_traverse_work_pop(t_graf_traverse *x)
{
    if (x->work_count == 0) return NULL;
    return x->work[--x->work_count];
}

/**
 * graf_traverse_work_dequeue — remove and return from the front (BFS queue).
 *
 * Advances work_head; the vacated slot is never reused within this traversal.
 * The queue is logically empty when work_head >= work_count.
 *
 * Java equivalent: queue.remove() (LinkedList used as Queue in bsf()).
 * Returns NULL if the queue is empty.
 */
t_symbol *graf_traverse_work_dequeue(t_graf_traverse *x)
{
    if (x->work_head >= x->work_count) return NULL;
    return x->work[x->work_head++];
}

/**
 * graf_traverse_work_empty — 1 if the stack/queue has no more items.
 */
long graf_traverse_work_empty(t_graf_traverse *x)
{
    if (x->mode == GRAF_TRAVERSE_DFS)
        return (x->work_count == 0);
    else
        return (x->work_head >= x->work_count);
}


////////////////////////// output control

/**
 * graf_traverse_output_node — emit a node's id and payload to the left outlet.
 *
 * No payload: outputs the symbol id alone via outlet_symbol.
 * With payload: outputs as a list message — selector is the node id,
 *               atoms are the payload — via outlet_anything.
 *
 * In Max patching terms:
 *   No payload  →  plain symbol "a"
 *   With payload →  message "a 60 0.5 127" (id + atoms as list body)
 *
 * Java equivalent: "process(node)" in GraphSearch — here that means
 * emitting the node to the Max outlet rather than running a callback.
 *
 * @param g   The containing graf (needed to read node payload)
 * @param id  The node ID symbol to output
 */
void graf_traverse_output_node(t_graf_traverse *x, t_graf *g, t_symbol *id)
{
    t_graf_node *node = graf_find_node(g, id);
    if (!node) {
        object_error((t_object *)x,
                     "graf.traverse: cannot output node '%s' — not found in [graf '%s']",
                     id->s_name, x->graf_name->s_name);
        return;
    }

    if (node->payload_count == 0) {
        outlet_anything(x->outlet_node, id, 0, NULL);
    } else {
        outlet_anything(x->outlet_node, id, (short)node->payload_count, node->payload);
    }
}


////////////////////////// shortest-path solver (Dijkstra)

/**
 * graf_traverse_node_index — find a node's array index within a graf.
 *
 * Dijkstra's bookkeeping arrays (dist/prev/settled) are indexed in parallel
 * with graf->nodes[], so we need the integer index of a node, not a pointer.
 *
 * O(n) linear scan — fine at sequencer scale (n < ~100). Pointer equality on
 * the interned symbol id is valid, same as graf_find_node().
 *
 * Java analogy: List.indexOf(id), returning -1 when absent.
 */
static long graf_traverse_node_index(t_graf *g, t_symbol *id)
{
    long i;
    for (i = 0; i < g->node_count; i++) {
        if (g->nodes[i].id == id)
            return i;
    }
    return -1;
}

/**
 * graf_traverse_ensure_dijkstra — guarantee the solver arrays hold >= n slots.
 *
 * Allocates dist/prev/settled/path on first use and grows them (doubling) when
 * the graph has gained nodes since the last solve. All four arrays are kept the
 * same length so they stay parallel-indexable with graf->nodes[].
 *
 * Java analogy: ensureCapacity() on four parallel ArrayLists at once.
 *
 * Note: sysmem_resizeptr needs a previously-allocated pointer, so each array
 * uses sysmem_newptr on first allocation and sysmem_resizeptr thereafter.
 */
t_max_err graf_traverse_ensure_dijkstra(t_graf_traverse *x, long n)
{
    if (n <= x->dijkstra_cap) return MAX_ERR_NONE;

    long cap = (x->dijkstra_cap > 0) ? x->dijkstra_cap : GRAF_TRAVERSE_INIT_CAP;
    while (cap < n) cap *= 2;

    double    *nd    = x->dist
                       ? (double *)sysmem_resizeptr(x->dist,    cap * sizeof(double))
                       : (double *)sysmem_newptr(cap * sizeof(double));
    long      *np    = x->prev
                       ? (long *)sysmem_resizeptr(x->prev,      cap * sizeof(long))
                       : (long *)sysmem_newptr(cap * sizeof(long));
    char      *ns    = x->settled
                       ? (char *)sysmem_resizeptr(x->settled,   cap * sizeof(char))
                       : (char *)sysmem_newptr(cap * sizeof(char));
    t_symbol **npath = x->path
                       ? (t_symbol **)sysmem_resizeptr(x->path, cap * sizeof(t_symbol *))
                       : (t_symbol **)sysmem_newptr(cap * sizeof(t_symbol *));

    /* Keep any allocation that succeeded so free() can release it later. */
    if (nd)    x->dist    = nd;
    if (np)    x->prev    = np;
    if (ns)    x->settled = ns;
    if (npath) x->path    = npath;

    if (!nd || !np || !ns || !npath) {
        object_error((t_object *)x,
                     "graf.traverse: out of memory allocating dijkstra state");
        return MAX_ERR_OUT_OF_MEM;
    }

    x->dijkstra_cap = cap;
    return MAX_ERR_NONE;
}

/**
 * graf_traverse_compute_dijkstra — single-source shortest path, start -> target.
 *
 * Standard Dijkstra. Because sequencer graphs are tiny (n < ~100) the priority
 * queue is just a linear scan for the minimum-distance unsettled node — O(n^2)
 * overall, which is irrelevant at this scale and far simpler than a binary heap.
 * (ext_hashtab / a real heap stay a future optimisation, not needed here.)
 *
 * EDGE WEIGHT SEMANTICS — important: here edge weight means COST (lower = better),
 * the OPPOSITE of the weighted random walk where weight means likelihood
 * (higher = more probable). Populate weights as distances/costs for meaningful
 * results. Dijkstra also assumes non-negative weights; a negative weight is
 * flagged once and the result may be wrong. All-zero weights still yield a valid
 * path, but an essentially arbitrary one (every route costs the same).
 *
 * On success the reconstructed path (start..target) is stored in x->path with
 * x->path_len set and x->path_pos reset to 0.
 *
 * Java analogy: the body of Dijkstra.shortestPath(start, target) returning the
 * node list, with dist[]/prev[] as the usual working arrays.
 *
 * @return MAX_ERR_NONE on success; MAX_ERR_GENERIC if no path exists.
 */
t_max_err graf_traverse_compute_dijkstra(t_graf_traverse *x, t_graf *g,
                                         t_symbol *start, t_symbol *target)
{
    long n = g->node_count;
    if (n == 0) {
        object_error((t_object *)x, "graf.traverse: dijkstra on empty graph");
        return MAX_ERR_GENERIC;
    }

    long src = graf_traverse_node_index(g, start);
    long dst = graf_traverse_node_index(g, target);
    if (src < 0 || dst < 0) {
        object_error((t_object *)x,
                     "graf.traverse: dijkstra start/target not in graph");
        return MAX_ERR_GENERIC;
    }

    if (graf_traverse_ensure_dijkstra(x, n) != MAX_ERR_NONE)
        return MAX_ERR_GENERIC;

    long i;
    for (i = 0; i < n; i++) {
        x->dist[i]    = INFINITY;
        x->prev[i]    = -1;
        x->settled[i] = 0;
    }
    x->dist[src] = 0.0;

    long warned_negative = 0;

    for (;;) {
        /* Priority queue as a linear scan: pick the unsettled node of min dist. */
        long   u    = -1;
        double best = INFINITY;
        for (i = 0; i < n; i++) {
            if (!x->settled[i] && x->dist[i] < best) {
                best = x->dist[i];
                u    = i;
            }
        }

        if (u < 0)    break;    /* nothing reachable remains */
        if (u == dst) break;    /* target finalised — its distance is now final */

        x->settled[u] = 1;

        /* Relax every outgoing edge of u. */
        t_graf_node *un = &g->nodes[u];
        long e;
        for (e = 0; e < un->edge_count; e++) {
            double w = un->edge_weights[e];

            if (w < 0.0 && !warned_negative) {
                object_warn((t_object *)x,
                            "graf.traverse: negative edge weight — dijkstra result may be incorrect");
                warned_negative = 1;
            }

            long v = graf_traverse_node_index(g, un->edges_to[e]);
            if (v < 0 || x->settled[v]) continue;   /* dangling target or already final */

            double nd = x->dist[u] + w;
            if (nd < x->dist[v]) {
                x->dist[v] = nd;
                x->prev[v] = u;
            }
        }
    }

    if (x->dist[dst] == INFINITY) {
        object_warn((t_object *)x,
                    "graf.traverse: no path from '%s' to '%s'",
                    start->s_name, target->s_name);
        return MAX_ERR_GENERIC;
    }

    /*
     * Reconstruct the path: walk prev[] backward from target to start, then
     * reverse in place so it reads start -> target. Length is bounded by n
     * (a shortest path visits each node at most once).
     */
    long len = 0;
    long cur = dst;
    while (cur != -1 && len < n) {
        x->path[len++] = g->nodes[cur].id;
        if (cur == src) break;
        cur = x->prev[cur];
    }
    for (i = 0; i < len / 2; i++) {
        t_symbol *tmp        = x->path[i];
        x->path[i]           = x->path[len - 1 - i];
        x->path[len - 1 - i] = tmp;
    }

    x->path_len = len;
    x->path_pos = 0;

    post("graf.traverse '%s': dijkstra '%s' -> '%s' — %ld nodes, cost %.4f",
         x->graf_name->s_name, start->s_name, target->s_name, len, x->dist[dst]);

    return MAX_ERR_NONE;
}


////////////////////////// Travel algorithms

/**
 * graf_traverse_step_random — one weighted random step.
 *
 * Picks a neighbor of the current node using edge weights as unnormalized
 * probabilities (like a loaded die). If all weights are 0.0, falls back
 * to uniform random — same behavior as [graf]'s built-in 'next' message.
 *
 * Algorithm:
 *   1. Sum all edge weights.
 *   2. If sum is 0, pick uniform random index.
 *   3. Otherwise, draw r uniformly from [0, sum) and scan cumulative
 *      weights to find which bucket r lands in.
 *
 * This is a stateless walk — nodes can be revisited freely. No visited
 * set is maintained. Signals done only on a dead end (no outgoing edges).
 *
 * Java: no direct equivalent in GraphSearch.java — the Java version
 * only checks connectivity. This is a Markov chain step.
 */
void graf_traverse_step_random(t_graf_traverse *x, t_graf *g)
{
    t_graf_node *node = graf_find_node(g, x->current);
    if (!node) {
        object_error((t_object *)x,
                     "graf.traverse: current node '%s' not found in [graf '%s']",
                     x->current->s_name, x->graf_name->s_name);
        return;
    }

    if (node->edge_count == 0) {
        object_warn((t_object *)x,
                    "graf.traverse: dead end at '%s' — no outgoing edges",
                    x->current->s_name);
        x->done = 1;
        outlet_bang(x->outlet_done);
        return;
    }

    long chosen = 0;

    /* Sum weights to determine if any are non-zero */
    double total = 0.0;
    for (long i = 0; i < node->edge_count; i++)
        total += node->edge_weights[i];

    if (total <= 0.0) {
        /* All weights zero — uniform random fallback */
        chosen = rand() % node->edge_count;
    } else {
        /*
         * Weighted random selection via cumulative distribution.
         * Draw r from [0, total) and find the edge whose cumulative
         * weight range contains r.
         *
         * Example: weights [1, 2, 1], total=4
         *   edge 0: [0,   1)  — 25% chance
         *   edge 1: [1,   3)  — 50% chance
         *   edge 2: [3,   4)  — 25% chance
         */
        double r = ((double)rand() / (double)RAND_MAX) * total;
        double cumulative = 0.0;
        chosen = node->edge_count - 1;  // fallback: last edge (floating-point safety)
        for (long i = 0; i < node->edge_count; i++) {
            cumulative += node->edge_weights[i];
            if (r < cumulative) {
                chosen = i;
                break;
            }
        }
    }

    x->current = node->edges_to[chosen];
    graf_traverse_output_node(x, g, x->current);
}

/**
 * graf_traverse_step_dfs — one step of iterative depth-first search.
 *
 * Algorithm per step:
 *   1. Pop from the stack until an unvisited node is found (skip
 *      nodes that landed on the stack multiple times via different paths).
 *   2. Mark the popped node as visited.
 *   3. Output it.
 *   4. Push all unvisited neighbors in REVERSE order — because the
 *      stack is LIFO, pushing in reverse means the first edge in
 *      edges_to[] is visited first (natural order).
 *   5. If stack is exhausted without finding an unvisited node: done.
 *
 * Java equivalent: one iteration of the while loop in an iterative
 * version of GraphSearch.dsf() — the Java version is recursive, so the
 * call stack serves as the explicit stack here.
 *
 * Key difference from recursive DFS: the same node can appear on the
 * stack multiple times (once per unvisited predecessor that pushed it).
 * The is_visited check at pop time handles this correctly — extra copies
 * are silently discarded.
 */
void graf_traverse_step_dfs(t_graf_traverse *x, t_graf *g)
{
    t_symbol *node_id = NULL;

    /*
     * Pop until we find an unvisited node.
     * Nodes may appear multiple times on the stack if reachable via
     * multiple unvisited predecessors — skip the duplicates.
     */
    while (!graf_traverse_work_empty(x)) {
        t_symbol *candidate = graf_traverse_work_pop(x);
        if (!graf_traverse_is_visited(x, candidate)) {
            node_id = candidate;
            break;
        }
    }

    if (!node_id) {
        /* Stack exhausted — DFS complete */
        x->done = 1;
        outlet_bang(x->outlet_done);
        return;
    }

    /* Mark visited, update position, output */
    graf_traverse_mark_visited(x, node_id);
    x->current = node_id;
    graf_traverse_output_node(x, g, node_id);

    /* Push unvisited neighbors in reverse order for natural visit order */
    t_graf_node *node = graf_find_node(g, node_id);
    if (!node) return;

    for (long i = node->edge_count - 1; i >= 0; i--) {
        if (!graf_traverse_is_visited(x, node->edges_to[i])) {
            graf_traverse_work_push(x, node->edges_to[i]);
        }
    }
}

/**
 * graf_traverse_step_bfs — one step of breadth-first search.
 *
 * Algorithm per step:
 *   1. If queue is empty: done.
 *   2. Dequeue the front node.
 *   3. Output it.
 *   4. For each neighbor: if not yet visited, mark visited and enqueue.
 *      (Marking on enqueue prevents duplicate queue entries.)
 *
 * Java equivalent: one iteration of GraphSearch.bsf()'s while loop.
 * We use work_head advancing forward instead of Java's queue.remove().
 *
 * Start node is marked visited in init (not here) to prevent it from
 * being re-enqueued when a neighbor points back to it.
 */
void graf_traverse_step_bfs(t_graf_traverse *x, t_graf *g)
{
    if (graf_traverse_work_empty(x)) {
        x->done = 1;
        outlet_bang(x->outlet_done);
        return;
    }

    t_symbol *node_id = graf_traverse_work_dequeue(x);
    x->current = node_id;
    graf_traverse_output_node(x, g, node_id);

    /* Enqueue unvisited neighbors (mark visited on enqueue) */
    t_graf_node *node = graf_find_node(g, node_id);
    if (!node) return;

    for (long i = 0; i < node->edge_count; i++) {
        t_symbol *neighbor = node->edges_to[i];
        if (!graf_traverse_is_visited(x, neighbor)) {
            graf_traverse_mark_visited(x, neighbor);
            graf_traverse_work_push(x, neighbor);
        }
    }
}

/**
 * graf_traverse_step_dijkstra — emit the next node of the precomputed path.
 *
 * Unlike DFS/BFS, no searching happens here: the full start->target path was
 * computed in init (graf_traverse_compute_dijkstra). Each 'step' just walks one
 * node forward along path[]. When the path is exhausted, bang done.
 *
 * This reuses the same left-outlet output as every other mode, so the existing
 * [graf.traverse] -> [prepend goto] -> [graf] wiring highlights each node of the
 * shortest path in graf.affiche as you step through it.
 */
void graf_traverse_step_dijkstra(t_graf_traverse *x, t_graf *g)
{
    if (x->path_pos >= x->path_len) {
        x->done = 1;
        outlet_bang(x->outlet_done);
        return;
    }

    t_symbol *node_id = x->path[x->path_pos++];
    x->current = node_id;
    graf_traverse_output_node(x, g, node_id);
}

//TODO: implement A* search — Dijkstra plus an admissible heuristic h(n).
//      Natural music heuristic: payload-based distance to target (e.g. pitch
//      difference). Open question is the heuristic policy and whether it stays
//      admissible w.r.t. how edge costs are defined.


////////////////////////// max messages handlers

/**
 * graf_traverse_step — advance the traversal by one node and output it.
 *
 * Dispatches to the appropriate algorithm based on current mode.
 * Auto-initializes on first call by falling back to graf->current.
 *
 * If traversal is already done, re-bangs the done outlet and returns.
 * Call 'reset' to begin a new traversal.
 */
void graf_traverse_step(t_graf_traverse *x)
{
    if (x->done) {
        outlet_bang(x->outlet_done);
        return;
    }

    if (!x->initialized) {
        if (graf_traverse_init(x, NULL) != MAX_ERR_NONE) return;
    }

    t_graf *g = graf_traverse_get_graf(x);
    if (!g) return;

    switch (x->mode) {
        case GRAF_TRAVERSE_RANDOM:   graf_traverse_step_random(x, g);   break;
        case GRAF_TRAVERSE_DFS:      graf_traverse_step_dfs(x, g);      break;
        case GRAF_TRAVERSE_BFS:      graf_traverse_step_bfs(x, g);      break;
        case GRAF_TRAVERSE_DIJKSTRA: graf_traverse_step_dijkstra(x, g); break;
        default:
            object_error((t_object *)x, "graf.traverse: internal error — unknown mode %d", x->mode);
    }
}

/**
 * graf_traverse_reset — reset traversal and optionally set start node.
 *
 * With no argument:    restart from x->start (if set) or graf->current.
 * With symbol arg:     restart from that node, and remember it as the
 *                      new default start for future reset calls.
 *
 * Usage:
 *   reset            — restart from existing start
 *   reset a          — restart from node 'a', remember 'a' as start
 *   reset a b        — restart from 'a' with shortest-path target 'b' (dijkstra)
 */
void graf_traverse_reset(t_graf_traverse *x, t_symbol *s, long argc, t_atom *argv)
{
    t_symbol *start_id = NULL;

    if (argc >= 1 && atom_gettype(argv) == A_SYM) {
        start_id = atom_getsym(argv);
        x->start = start_id;    // remember as default for future resets
    }

    /* Optional second symbol sets the shortest-path target (dijkstra). */
    if (argc >= 2 && atom_gettype(argv + 1) == A_SYM) {
        x->target = atom_getsym(argv + 1);
    }

    graf_traverse_init(x, start_id);
}

/**
 * graf_traverse_mode — switch traversal algorithm.
 *
 * Clears all traversal state; the next 'step' will re-initialize.
 * Does not change x->start — the same start node will be used.
 *
 * Valid mode symbols: random, dfs, bfs
 *
 * Usage:
 *   mode random
 *   mode dfs
 *   mode bfs
 */
void graf_traverse_mode(t_graf_traverse *x, t_symbol *s, long argc, t_atom *argv)
{
    if (argc < 1 || atom_gettype(argv) != A_SYM) {
        object_error((t_object *)x,
                     "graf.traverse: mode requires one symbol argument: random, dfs, bfs, or dijkstra");
        return;
    }

    t_symbol *mode_sym = atom_getsym(argv);
    int new_mode;

    /*
     * Symbol pointer comparison via gensym() — valid because Max interns
     * all symbols. gensym("dfs") always returns the same pointer address.
     * Java equivalent: mode_sym.equals("dfs") — but here by identity.
     */
    if (mode_sym == gensym("random"))         new_mode = GRAF_TRAVERSE_RANDOM;
    else if (mode_sym == gensym("dfs"))       new_mode = GRAF_TRAVERSE_DFS;
    else if (mode_sym == gensym("bfs"))       new_mode = GRAF_TRAVERSE_BFS;
    else if (mode_sym == gensym("dijkstra"))  new_mode = GRAF_TRAVERSE_DIJKSTRA;
    else {
        object_error((t_object *)x,
                     "graf.traverse: unknown mode '%s' — valid: random, dfs, bfs, dijkstra",
                     mode_sym->s_name);
        return;
    }

    x->mode = new_mode;
    graf_traverse_clear_state(x);
    post("graf.traverse '%s': mode -> %s", x->graf_name->s_name, mode_sym->s_name);
}

/**
 * graf_traverse_from — set the start node without resetting.
 *
 * Useful for pre-configuring the start node before the first step,
 * or to change the start position for the NEXT reset without
 * interrupting an active traversal.
 *
 * Usage:  from a
 */
void graf_traverse_from(t_graf_traverse *x, t_symbol *id)
{
    x->start = id;
}

/**
 * graf_traverse_to — set the shortest-path target node (dijkstra mode).
 *
 * Symmetric to 'from': 'from' sets where a traversal starts, 'to' sets where a
 * shortest-path search should end. Has no effect on random/dfs/bfs, which have
 * no notion of a destination. Takes effect on the next reset/step.
 *
 * Usage:  to b
 */
void graf_traverse_to(t_graf_traverse *x, t_symbol *id)
{
    x->target = id;
}

/**
 * graf_traverse_bang — re-output the current node without advancing.
 *
 * Useful for querying current position mid-traversal.
 * Does nothing (with a warning) if no current position is set.
 */
void graf_traverse_bang(t_graf_traverse *x)
{
    if (!x->current) {
        object_warn((t_object *)x,
                    "graf.traverse: no current node — send 'step' or 'reset' first");
        return;
    }

    t_graf *g = graf_traverse_get_graf(x);
    if (!g) return;

    graf_traverse_output_node(x, g, x->current);
}