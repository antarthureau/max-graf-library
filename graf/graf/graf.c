/**
    @file
    graf — directed weighted graph data structure for Max/MSP
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
        clear                      — remove all nodes and edges, reset state
        write [filename]           — save graph to CSV file (opens dialog if no filename)
        read  [filename]           — load graph from CSV file (opens dialog if no filename)

    Node ids:
        Ids are symbols internally, but every id-taking message ALSO accepts
        ints and floats — they are canonicalised via graf_atom_to_id() in
        graf.h ("%ld" / "%.10g", the same formats as the CSV writer and
        graf.observe's record conversion). So "removenode 0" from a message
        box, a node learned from the int 0 by graf.observe, and the CSV
        token "0" all refer to the same node. These messages are registered
        A_GIMME (not A_SYM) precisely so Max's typechecker does not reject
        numeric atoms before the handler runs.

    CSV file format:
        # comment lines (leading # after optional whitespace)
        n, id [, payload...]        — node declaration
        e, from, to [, weight]      — edge declaration (weight defaults to 0.0)

    Instance naming:
        graf my_graph               — named instance, referenceable by other objects
        graf                        — auto-named (graf_0, graf_1, ...), still functional

    Notifications:
        Every state-changing message (addnode, removenode, addedge, removeedge,
        goto, next, clear, read) calls:
            object_notify((t_object *)x, gensym("modified"), NULL)
        This is what allows graf.affiche to redraw automatically. Subscribers
        receive the notification via their notify() method. Equivalent to
        Observable.notifyObservers() in Java.

    Basic sequencer usage:
        - addnode to build states (nodes carry pitch/duration/velocity as payload)
        - addedge to define possible transitions
        - goto to manually place the playhead at any node
        - next to let the graph autonomously move to a neighbour
        - bang to trigger the current state (outputs to outlet)
        - write my_graph.csv to save; read my_graph.csv to restore
*/

#include "ext.h"
#include "ext_obex.h"
#include "graf.h"        // t_graf_node, t_graf, graf_find_node(), graf_find(), graf_atom_to_id()
#include <string.h>
#include <stdlib.h>      // strtol, strtod

// Initial capacity for the nodes array.
   Will double automatically when exceeded*/
#define GRAF_INIT_CAPACITY       16

// Prefix used for auto-generated instance names when user doesn't specify one
#define GRAF_AUTO_NAME_PREFIX    "graf_"

// Prefix used for auto-generated node IDs when user doesn't specify one
#define GRAF_AUTO_NODE_PREFIX    "node"

// CSV parser limits.
   GRAF_CSV_TOKEN_LEN caps the length of any single token (id, atom value).
   GRAF_CSV_MAX_TOKENS caps the number of columns per line (id + payload atoms). */
#define GRAF_CSV_MAX_TOKENS      32
#define GRAF_CSV_TOKEN_LEN       128
#define GRAF_CSV_LINE_LEN        512

// Global counter for auto-naming graf instances
static long graf_instance_count = 0;

// NOTE: t_graf_node and t_graf are defined in graf.h.
   graf_find_node() is a static inline in graf.h — do not redefine here. */


////////////////////////// function prototypes

void *graf_new(t_symbol *s, long argc, t_atom *argv);
void  graf_free(t_graf *x);
void  graf_assist(t_graf *x, void *b, long m, long a, char *s);

// message handlers
   All id-taking handlers are A_GIMME so numeric ids are accepted —
   see "Node ids" in the file header. */
void  graf_bang(t_graf *x);
void  graf_addnode(t_graf *x, t_symbol *s, long argc, t_atom *argv);
void  graf_removenode(t_graf *x, t_symbol *s, long argc, t_atom *argv);
void  graf_addedge(t_graf *x, t_symbol *s, long argc, t_atom *argv);
void  graf_removeedge(t_graf *x, t_symbol *s, long argc, t_atom *argv);
void  graf_goto(t_graf *x, t_symbol *s, long argc, t_atom *argv);
void  graf_next(t_graf *x);
void  graf_hasnode(t_graf *x, t_symbol *s, long argc, t_atom *argv);
void  graf_neighbours(t_graf *x, t_symbol *s, long argc, t_atom *argv);
void  graf_adjacent(t_graf *x, t_symbol *s, long argc, t_atom *argv);
void  graf_size(t_graf *x);
void  graf_name(t_graf *x);
void  graf_print(t_graf *x);
void  graf_clear(t_graf *x);
void  graf_write(t_graf *x, t_symbol *filename);
void  graf_read(t_graf *x, t_symbol *filename);

//TODO: implement cycles and components related functions
//void graf_hascycles returns 1/0 if yes/no
//void graf_cycles retruns int with the number of cycles
//void graf_components returns int with the number of components

//TODO: implement last visited node and last edge visited
void graf_lastnode(t_graf *x) //returns the last node visited
void graf_lastedge(t_graf *x) //returns the last edge visited
void graf_visitednodes(t_graf *x) //returns a list of all visited nodes
void graf_visitededges(t_graf *x) //returns a list of all visited edges

// internal helpers — graph structure
long  graf_find_node_index(t_graf *x, t_symbol *id); //make as message handler to be able to get the index of a node in the graph??
void  graf_ensure_capacity(t_graf *x);

// internal helpers — quiet core operations (no console output).
   Message handlers call these and then post() on success.
   The read path calls these directly to avoid console spam on bulk load. */
static int  graf_addnode_quiet(t_graf *x, t_symbol *id,
                               long payload_count, t_atom *payload);
static int  graf_addedge_quiet(t_graf *x, t_symbol *u,
                               t_symbol *v, double weight);

// internal helpers — file I/O */
static void  graf_write_to_handle(t_graf *x, t_filehandle fh);
static void  graf_sysfile_write_str(t_filehandle fh, const char *str);
static void  graf_atom_to_str(const t_atom *a, char *buf, long maxlen);
static long  graf_csv_tokenize(char *line,
                               char tokens[][GRAF_CSV_TOKEN_LEN],
                               long max_tokens);
static void  graf_parse_atom_str(const char *str, t_atom *a);
static void  graf_load_from_buffer(t_graf *x, char *buf, t_ptr_size count);

// global class pointer */
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

    // graph structure
       Id-taking messages are A_GIMME (not A_SYM): the Max typechecker
       rejects int atoms on A_SYM messages before the handler even runs,
       which made numeric node ids (e.g. those learned by graf.observe)
       unreachable from message boxes. A_GIMME lets the handler accept
       symbols, ints, and floats via graf_atom_to_id(). */
    class_addmethod(c, (method)graf_addnode,     "addnode",     A_GIMME, 0);
    class_addmethod(c, (method)graf_removenode,  "removenode",  A_GIMME, 0);
    class_addmethod(c, (method)graf_addedge,     "addedge",     A_GIMME, 0);
    class_addmethod(c, (method)graf_removeedge,  "removeedge",  A_GIMME, 0);

    // traversal */
    class_addmethod(c, (method)graf_goto,        "goto",        A_GIMME, 0);
    class_addmethod(c, (method)graf_next,        "next",        0);

    // queries */
    class_addmethod(c, (method)graf_hasnode,     "hasnode",     A_GIMME, 0);
    class_addmethod(c, (method)graf_neighbours,  "neighbours",  A_GIMME, 0);
    class_addmethod(c, (method)graf_adjacent,    "adjacent",    A_GIMME, 0);
    class_addmethod(c, (method)graf_size,        "size",        0);
    class_addmethod(c, (method)graf_name,        "name",        0);
    class_addmethod(c, (method)graf_print,       "print",       0);

    // persistence
       A_DEFSYM: like an Optional<String> in Java — argument is optional.
       When no filename is given, Max passes gensym("") (empty string).
       Handlers check for empty string and open a file dialog instead. */
    class_addmethod(c, (method)graf_clear,       "clear",       0);
    class_addmethod(c, (method)graf_write,       "write",       A_DEFSYM, 0);
    class_addmethod(c, (method)graf_read,        "read",        A_DEFSYM, 0);

    class_register(CLASS_BOX, c);
    graf_class = c;

    post("graf: loaded");
}


////////////////////////// internal helpers — graph structure

/**
 * Find a node's index in the nodes array.
 * Returns -1 if not found.
 * Used when we need to shift the array after removal (removenode).
 *
 * Note: graf_find_node() (returns pointer) is in graf.h as a static inline,
 * shared with graf.traverse. This index variant is only needed internally
 * in graf.c so it stays here.
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

/**
 * Add a node without posting to the Max console.
 * This is the core insertion logic shared by graf_addnode (interactive)
 * and graf_load_from_buffer (bulk load from file).
 *
 * id            — interned symbol to use as the node identifier
 * payload_count — number of Max atoms in the payload array (may be 0)
 * payload       — pointer to payload atoms; copied into the node (may be NULL)
 *
 * Returns  0 on success
 *         -1 if a node with this id already exists (warns via object_warn)
 *
 * Java analogy: like addVertice() in IGraph<V>, but with payload support.
 */
static int graf_addnode_quiet(t_graf *x, t_symbol *id,
                              long payload_count, t_atom *payload)
{
    if (graf_find_node(x, id)) {
        object_warn((t_object *)x,
                    "addnode: node '%s' already exists", id->s_name);
        return -1;
    }

    graf_ensure_capacity(x);

    t_graf_node *n  = &x->nodes[x->node_count];
    n->id           = id;
    n->edge_count   = 0;
    n->edges_to     = NULL;     // allocated lazily on first addedge */
    n->edge_weights = NULL;

    if (payload_count > 0 && payload) {
        n->payload_count = payload_count;
        n->payload = (t_atom *)sysmem_newptr(payload_count * sizeof(t_atom));
        sysmem_copyptr(payload, n->payload, payload_count * sizeof(t_atom));
    } else {
        n->payload       = NULL;
        n->payload_count = 0;
    }

    x->node_count++;
    return 0;
}

/**
 * Add an edge without posting to the Max console.
 * Core insertion logic shared by graf_addedge and graf_load_from_buffer.
 *
 * u, v   — interned symbols identifying source and target nodes
 * weight — edge weight (0.0 = no preference; used by graf.traverse for
 *          weighted random and Dijkstra)
 *
 * Returns  0 on success
 *         -1 on error (node not found, or duplicate edge)
 *
 * Java analogy: like addEdge() in IGraph<V>, but directed and weighted.
 */
static int graf_addedge_quiet(t_graf *x, t_symbol *u,
                              t_symbol *v, double weight)
{
    t_graf_node *src = graf_find_node(x, u);
    t_graf_node *dst = graf_find_node(x, v);

    if (!src) {
        object_error((t_object *)x,
                     "addedge: source '%s' not found", u->s_name);
        return -1;
    }
    if (!dst) {
        object_error((t_object *)x,
                     "addedge: target '%s' not found", v->s_name);
        return -1;
    }

    // duplicate check
    long i;
    for (i = 0; i < src->edge_count; i++) {
        if (src->edges_to[i] == v) {
            object_warn((t_object *)x,
                        "addedge: edge '%s'->'%s' already exists",
                        u->s_name, v->s_name);
            return -1;
        }
    }

    // grow edge arrays: allocate on first edge, resize thereafter
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

    src->edges_to[src->edge_count]     = v;
    src->edge_weights[src->edge_count] = weight;
    src->edge_count++;
    return 0;
}


////////////////////////// internal helpers — file I/O

/**
 * Write a null-terminated string to an open file handle.
 * Thin wrapper around sysfile_write for line-at-a-time text output.
 */
static void graf_sysfile_write_str(t_filehandle fh, const char *str)
{
    t_ptr_size len = (t_ptr_size)strlen(str);
    sysfile_write(fh, &len, (void *)str);
}

/**
 * Render a single Max atom as a string suitable for CSV output.
 *
 *   A_LONG  → integer, e.g. "60"
 *   A_FLOAT → decimal, e.g. "0.5" — %.10g preserves round-trip precision
 *             without unnecessary trailing zeros
 *   A_SYM   → raw symbol name, e.g. "foo"
 *
 * These formats are the SAME as graf_atom_to_id() in graf.h — node ids
 * round-trip identically through message boxes, graf.observe, and CSV.
 *
 * Limitation: symbol values containing commas or whitespace will break
 * the CSV parser on read. Avoid commas and leading/trailing spaces in
 * node IDs and symbol payloads.
 */
static void graf_atom_to_str(const t_atom *a, char *buf, long maxlen)
{
    switch (atom_gettype(a)) {
        case A_LONG:
            snprintf(buf, maxlen, "%ld", (long)atom_getlong(a));
            break;
        case A_FLOAT:
            snprintf(buf, maxlen, "%.10g", (double)atom_getfloat(a));
            break;
        case A_SYM:
            snprintf(buf, maxlen, "%s", atom_getsym(a)->s_name);
            break;
        default:
            snprintf(buf, maxlen, "0");
            break;
    }
}

/**
 * Split a single CSV line into whitespace-trimmed tokens.
 *
 * Lines starting with '#' (optionally preceded by whitespace) are comments:
 * returns 0 immediately. Empty lines also return 0.
 *
 * tokens      — output array; each entry is a null-terminated string
 * max_tokens  — capacity of the tokens array
 * returns     — number of tokens found (0 for blank/comment lines)
 *
 * Java analogy: like line.split(",") followed by .strip() on each element,
 * but without allocations (operates in place on the caller's buffer).
 */
static long graf_csv_tokenize(char *line,
                              char tokens[][GRAF_CSV_TOKEN_LEN],
                              long max_tokens)
{
    long  count = 0;
    char *p     = line;

    // skip leading whitespace */
    while (*p == ' ' || *p == '\t') p++;

    // comment or empty line — caller should skip */
    if (!*p || *p == '#') return 0;

    while (*p && count < max_tokens) {
        // skip whitespace before this token */
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        // scan to comma or end of string */
        char *start = p;
        while (*p && *p != ',') p++;
        char *end = p;

        // trim trailing whitespace */
        while (end > start && (*(end - 1) == ' ' || *(end - 1) == '\t'))
            end--;

        long len = (long)(end - start);
        if (len <= 0) {
            // empty token between commas — skip */
            if (*p == ',') p++;
            continue;
        }
        if (len > GRAF_CSV_TOKEN_LEN - 1) len = GRAF_CSV_TOKEN_LEN - 1;
        strncpy(tokens[count], start, len);
        tokens[count][len] = '\0';
        count++;

        if (*p == ',') p++;  // advance past comma */
    }

    return count;
}

/**
 * Infer the Max atom type for a string token and set the atom accordingly.
 *
 * Priority:
 *   1. Try integer (strtol) — succeeds when the entire string is consumed
 *   2. Try float   (strtod) — succeeds when the entire string is consumed
 *   3. Fall back to symbol
 *
 * Java analogy: trying Integer.parseInt, then Double.parseDouble, then
 * treating the raw string as a Symbol object.
 */
static void graf_parse_atom_str(const char *str, t_atom *a)
{
    char *end;

    long lval = strtol(str, &end, 10);
    if (end != str && *end == '\0') {
        atom_setlong(a, lval);
        return;
    }

    double dval = strtod(str, &end);
    if (end != str && *end == '\0') {
        atom_setfloat(a, (float)dval);
        return;
    }

    atom_setsym(a, gensym(str));
}

/**
 * Write the entire graph to an already-open file handle.
 * Called by graf_write after creating the file.
 *
 * Output order: header comments, then all nodes, then all edges.
 * Edge weights are always written explicitly (even 0.0) so that a
 * round-trip read restores the full weight information exactly.
 */
static void graf_write_to_handle(t_graf *x, t_filehandle fh)
{
    char line[GRAF_CSV_LINE_LEN];
    long i, j;

    graf_sysfile_write_str(fh, "# graf CSV — graph data file\n");
    graf_sysfile_write_str(fh, "# nodes: n, id [, payload...]\n");
    graf_sysfile_write_str(fh, "# edges: e, from, to [, weight]\n");

    // nodes */
    for (i = 0; i < x->node_count; i++) {
        t_graf_node *n = &x->nodes[i];
        snprintf(line, sizeof(line), "n, %s", n->id->s_name);

        for (j = 0; j < n->payload_count; j++) {
            char val[64];
            graf_atom_to_str(&n->payload[j], val, sizeof(val));
            strncat(line, ", ",  sizeof(line) - strlen(line) - 1);
            strncat(line, val,   sizeof(line) - strlen(line) - 1);
        }
        strncat(line, "\n", sizeof(line) - strlen(line) - 1);
        graf_sysfile_write_str(fh, line);
    }

    // edges */
    for (i = 0; i < x->node_count; i++) {
        t_graf_node *n = &x->nodes[i];
        for (j = 0; j < n->edge_count; j++) {
            snprintf(line, sizeof(line), "e, %s, %s, %.10g\n",
                     n->id->s_name,
                     n->edges_to[j]->s_name,
                     n->edge_weights[j]);
            graf_sysfile_write_str(fh, line);
        }
    }
}

/**
 * Parse a text buffer of CSV content and populate the graph.
 * Called by graf_read after reading the whole file into memory.
 *
 * The buffer is modified in place (newlines replaced with null bytes to
 * null-terminate each line). The caller owns the buffer and frees it.
 *
 * Uses the quiet core operations to avoid flooding the Max console
 * with per-node/per-edge messages during bulk load. graf_read posts
 * a single summary line and fires a single object_notify after this
 * function returns.
 */
static void graf_load_from_buffer(t_graf *x, char *buf, t_ptr_size count)
{
    char  tokens[GRAF_CSV_MAX_TOKENS][GRAF_CSV_TOKEN_LEN];
    char *p   = buf;
    char *end = buf + count;

    while (p < end && *p) {
        // locate end of this line */
        char *line_end = p;
        while (line_end < end && *line_end != '\n' && *line_end != '\r')
            line_end++;

        // null-terminate the line in the buffer */
        if (line_end < end) *line_end = '\0';

        // skip past the newline (handle \r\n and \n\r) */
        char *next_line = line_end + 1;
        if (next_line < end && (*next_line == '\r' || *next_line == '\n'))
            next_line++;

        long ntokens = graf_csv_tokenize(p, tokens, GRAF_CSV_MAX_TOKENS);

        if (ntokens >= 2 && strcmp(tokens[0], "n") == 0) {
            /*
             * Node line: n, id [, payload...]
             *
             * Reconstruct a payload atom array from tokens[2..ntokens-1].
             * tokens[1] is the node id.
             *
             * Example:  n, a, 60, 0.5, 127
             *   tokens: [n] [a] [60] [0.5] [127]   ntokens=5
             *   payload: atoms [60, 0.5, 127]       payload_count=3
             */
            t_atom payload[GRAF_CSV_MAX_TOKENS];
            long k;
            long payload_count = ntokens - 2;
            for (k = 0; k < payload_count; k++)
                graf_parse_atom_str(tokens[k + 2], &payload[k]);

            graf_addnode_quiet(x, gensym(tokens[1]),
                               payload_count,
                               payload_count > 0 ? payload : NULL);

        } else if (ntokens >= 3 && strcmp(tokens[0], "e") == 0) {
            /*
             * Edge line: e, from, to [, weight]
             *
             * Weight is optional in the file; defaults to 0.0 if absent.
             */
            double weight = (ntokens >= 4) ? strtod(tokens[3], NULL) : 0.0;
            graf_addedge_quiet(x,
                               gensym(tokens[1]),
                               gensym(tokens[2]),
                               weight);
        }

        p = next_line;
    }
}


////////////////////////// object lifecycle

/**
 * Constructor — called when the user creates [graf] or [graf my_name].
 *
 * Named instances register themselves via object_register() so that
 * graf.traverse (and future family members) can locate them by name
 * using object_findregistered(). This mirrors the dict~ / buffer~ pattern.
 */
void *graf_new(t_symbol *s, long argc, t_atom *argv)
{
    t_graf *x = (t_graf *)object_alloc(graf_class);
    if (!x) return NULL;

    if (argc > 0 && atom_gettype(argv) == A_SYM) {
        x->name = atom_getsym(argv);
    } else {
        char auto_name[64];
        snprintf(auto_name, sizeof(auto_name), "%s%ld",
                 GRAF_AUTO_NAME_PREFIX, graf_instance_count++);
        x->name = gensym(auto_name);
    }

    object_register(CLASS_BOX, x->name, x);

    x->node_count    = 0;
    x->node_capacity = GRAF_INIT_CAPACITY;
    x->next_node_id  = 0;
    x->nodes         = (t_graf_node *)sysmem_newptr(
                        x->node_capacity * sizeof(t_graf_node));
    x->current       = NULL;

    x->outlet = outlet_new(x, NULL);

    post("graf: created instance '%s'", x->name->s_name);
    return x;
}

/**
 * Destructor — free all dynamically allocated memory.
 * Each node owns its payload and edge arrays, so those are freed first,
 * then the nodes array itself.
 */
void graf_free(t_graf *x)
{
    long i;

    object_unregister(x);

    for (i = 0; i < x->node_count; i++) {
        t_graf_node *n = &x->nodes[i];
        if (n->payload)      sysmem_freeptr(n->payload);
        if (n->edges_to)     sysmem_freeptr(n->edges_to);
        if (n->edge_weights) sysmem_freeptr(n->edge_weights);
    }

    if (x->nodes) sysmem_freeptr(x->nodes);
}

void graf_assist(t_graf *x, void *b, long m, long a, char *s)
{
    if (m == ASSIST_INLET)
        sprintf(s,
            "addnode / addedge / goto / next / bang / "
            "clear / read / write / hasnode / ...");
    else
        sprintf(s, "node output: id + payload");
}


////////////////////////// message handlers

/**
 * bang — output current node id + payload to the outlet.
 *
 * Output format: node id is the message selector; payload atoms follow.
 * Example (node foo with payload [60, 0.5]): outlet sends "foo 60 0.5"
 * which downstream [route] or [unpack] objects can handle.
 *
 * bang does NOT notify — it is a query, not a state change.
 */
void graf_bang(t_graf *x)
{
    if (!x->current) {
        object_error((t_object *)x,
                     "bang: no current node — use 'goto' first");
        return;
    }
    t_graf_node *n = graf_find_node(x, x->current);
    if (!n) return;

    outlet_anything(x->outlet, n->id,
                    n->payload_count,
                    n->payload_count > 0 ? n->payload : NULL);
}

/**
 * addnode [id] [payload...]
 *
 * Add a node. ID auto-increments if omitted. Any additional arguments
 * become the node's payload (ints, floats, or symbols — any Max atoms).
 * Numeric ids are canonicalised via graf_atom_to_id() (this also fixes
 * the old behaviour where a float id was silently truncated to an int).
 *
 * Notifies subscribers on success so graf.affiche redraws.
 *
 * Examples:
 *   addnode              → node0 (no payload)
 *   addnode foo          → foo (no payload)
 *   addnode foo 60 0.5   → foo with payload [60, 0.5]
 *   addnode 60 0.5 127   → node "60" with payload [0.5, 127]
 */
void graf_addnode(t_graf *x, t_symbol *s, long argc, t_atom *argv)
{
    t_symbol *id;

    if (argc < 1) {
        char auto_id[64];
        snprintf(auto_id, sizeof(auto_id), "%s%ld",
                 GRAF_AUTO_NODE_PREFIX, x->next_node_id++);
        id = gensym(auto_id);
    } else {
        id = graf_atom_to_id(argv);
        if (!id) {
            object_error((t_object *)x,
                         "addnode: id must be a symbol, int, or float");
            return;
        }
    }

    long payload_count = (argc > 1) ? argc - 1 : 0;
    t_atom *payload    = (payload_count > 0) ? argv + 1 : NULL;

    if (graf_addnode_quiet(x, id, payload_count, payload) == 0) {
        post("graf: added node '%s'", id->s_name);
        object_notify((t_object *)x, gensym("modified"), NULL);
    }
}

/**
 * removenode <id>
 *
 * Remove a node and clean up:
 *   1. Free the node's own memory (payload, edge arrays)
 *   2. Remove all edges pointing TO this node from other nodes
 *   3. Shift the nodes array to fill the gap
 *   4. Clear current pointer if it was pointing to this node
 *
 * A_GIMME so numeric ids work: "removenode 0" from a message box sends
 * an int atom, which an A_SYM registration would have rejected outright.
 *
 * Notifies subscribers on success.
 */
void graf_removenode(t_graf *x, t_symbol *s, long argc, t_atom *argv)
{
    if (argc < 1) {
        object_error((t_object *)x, "removenode: requires a node id");
        return;
    }

    t_symbol *id = graf_atom_to_id(argv);
    if (!id) {
        object_error((t_object *)x,
                     "removenode: id must be a symbol, int, or float");
        return;
    }

    long idx = graf_find_node_index(x, id);
    if (idx < 0) {
        object_error((t_object *)x,
                     "removenode: node '%s' not found", id->s_name);
        return;
    }

    t_graf_node *n = &x->nodes[idx];
    if (n->payload)      sysmem_freeptr(n->payload);
    if (n->edges_to)     sysmem_freeptr(n->edges_to);
    if (n->edge_weights) sysmem_freeptr(n->edge_weights);

    // remove any inbound edges from other nodes */
    long i, j;
    for (i = 0; i < x->node_count; i++) {
        if (i == idx) continue;
        t_graf_node *other = &x->nodes[i];
        for (j = 0; j < other->edge_count; j++) {
            if (other->edges_to[j] == id) {
                long k;
                for (k = j; k < other->edge_count - 1; k++) {
                    other->edges_to[k]     = other->edges_to[k + 1];
                    other->edge_weights[k] = other->edge_weights[k + 1];
                }
                other->edge_count--;
                j--;  // re-check index after shift */
            }
        }
    }

    // shift nodes array to fill the gap */
    long shift;
    for (shift = idx; shift < x->node_count - 1; shift++)
        x->nodes[shift] = x->nodes[shift + 1];
    x->node_count--;

    if (x->current == id) x->current = NULL;

    post("graf: removed node '%s'", id->s_name);
    object_notify((t_object *)x, gensym("modified"), NULL);
}

/**
 * addedge <u> <v> [weight]
 *
 * Add a directed edge u→v. Weight defaults to 0.0.
 * Both nodes must already exist. Duplicate edges are rejected.
 * Numeric ids accepted ("addedge 60 64 1." works on nodes "60" and "64" —
 * the old atom_getsym parsing silently produced the empty symbol for
 * numeric atoms, so such edges could never be created from message boxes).
 *
 * Notifies subscribers on success.
 */
void graf_addedge(t_graf *x, t_symbol *s, long argc, t_atom *argv)
{
    if (argc < 2) {
        object_error((t_object *)x,
                     "addedge: requires source and target node ids");
        return;
    }

    t_symbol *u = graf_atom_to_id(argv);
    t_symbol *v = graf_atom_to_id(argv + 1);
    if (!u || !v) {
        object_error((t_object *)x,
                     "addedge: ids must be symbols, ints, or floats");
        return;
    }

    double weight = (argc >= 3) ? atom_getfloat(argv + 2) : 0.0;

    if (graf_addedge_quiet(x, u, v, weight) == 0) {
        post("graf: added edge '%s' -> '%s' (weight: %.2f)",
             u->s_name, v->s_name, weight);
        object_notify((t_object *)x, gensym("modified"), NULL);
    }
}

/**
 * removeedge <u> <v>
 *
 * Remove the directed edge u→v only (not v→u).
 * Numeric ids accepted.
 * Notifies subscribers on success.
 */
void graf_removeedge(t_graf *x, t_symbol *s, long argc, t_atom *argv)
{
    if (argc < 2) {
        object_error((t_object *)x,
                     "removeedge: requires source and target node ids");
        return;
    }

    t_symbol *u = graf_atom_to_id(argv);
    t_symbol *v = graf_atom_to_id(argv + 1);
    if (!u || !v) {
        object_error((t_object *)x,
                     "removeedge: ids must be symbols, ints, or floats");
        return;
    }

    t_graf_node *src = graf_find_node(x, u);
    if (!src) {
        object_error((t_object *)x,
                     "removeedge: source '%s' not found", u->s_name);
        return;
    }

    long i;
    for (i = 0; i < src->edge_count; i++) {
        if (src->edges_to[i] == v) {
            long k;
            for (k = i; k < src->edge_count - 1; k++) {
                src->edges_to[k]     = src->edges_to[k + 1];
                src->edge_weights[k] = src->edge_weights[k + 1];
            }
            src->edge_count--;
            post("graf: removed edge '%s' -> '%s'", u->s_name, v->s_name);
            object_notify((t_object *)x, gensym("modified"), NULL);
            return;
        }
    }
    object_error((t_object *)x,
                 "removeedge: edge '%s'->'%s' not found",
                 u->s_name, v->s_name);
}

/**
 * goto <id>
 *
 * Set the current traversal position to the specified node.
 * Direct jump — edges are ignored, any node can be reached.
 * After this, bang outputs that node's id and payload.
 * Numeric ids accepted — so [prepend goto] fed with numeric node output
 * (e.g. from a graph learned by graf.observe) highlights correctly.
 *
 * Notifies subscribers so graf.affiche highlights the new current node.
 * This is also the hook used in the [prepend goto] → [graf] patcher pattern
 * that connects graf.traverse output to graf.affiche highlighting.
 */
void graf_goto(t_graf *x, t_symbol *s, long argc, t_atom *argv)
{
    if (argc < 1) {
        object_error((t_object *)x, "goto: requires a node id");
        return;
    }

    t_symbol *id = graf_atom_to_id(argv);
    if (!id) {
        object_error((t_object *)x,
                     "goto: id must be a symbol, int, or float");
        return;
    }

    if (!graf_find_node(x, id)) {
        object_error((t_object *)x, "goto: node '%s' not found", id->s_name);
        return;
    }
    x->current = id;
    post("graf: current -> '%s'", id->s_name);
    object_notify((t_object *)x, gensym("modified"), NULL);
}

/**
 * next
 *
 * Move to a uniform-random outgoing neighbour, output the new node.
 * This is the only traversal built into graf — intentionally simple.
 * Weight-aware traversal (= navigation algos such as DFS, BFS, Dijkstra, etc) lives in graf.traverse.
 *
 * Notifies subscribers after moving so graf.affiche follows along.
 */
void graf_next(t_graf *x)
{
    if (!x->current) {
        object_error((t_object *)x,
                     "next: no current node — use 'goto' first");
        return;
    }

    t_graf_node *n = graf_find_node(x, x->current);
    if (!n) return;

    if (n->edge_count == 0) {
        object_error((t_object *)x,
                     "next: node '%s' has no outgoing edges (dead end)",
                     x->current->s_name);
        return;
    }

    long chosen    = (long)(rand() % n->edge_count);
    x->current     = n->edges_to[chosen];

    t_graf_node *next_node = graf_find_node(x, x->current);
    if (!next_node) return;

    outlet_anything(x->outlet, next_node->id,
                    next_node->payload_count,
                    next_node->payload_count > 0 ? next_node->payload : NULL);

    object_notify((t_object *)x, gensym("modified"), NULL);
}

/**
 * hasnode <id>
 *
 * Output "hasnode 1" or "hasnode 0". Numeric ids accepted.
 * Equivalent to IGraph.hasVertice() in Java.
 * Query only — no notify.
 */
void graf_hasnode(t_graf *x, t_symbol *s, long argc, t_atom *argv)
{
    if (argc < 1) {
        object_error((t_object *)x, "hasnode: requires a node id");
        return;
    }

    t_symbol *id = graf_atom_to_id(argv);
    if (!id) {
        object_error((t_object *)x,
                     "hasnode: id must be a symbol, int, or float");
        return;
    }

    t_atom result;
    atom_setlong(&result, graf_find_node(x, id) ? 1 : 0);
    outlet_anything(x->outlet, gensym("hasnode"), 1, &result);
}

/**
 * neighbours <id>
 *
 * Output each outgoing neighbour as a separate "neighbour <id>" message.
 * Numeric ids accepted.
 * Equivalent to IGraph.neighbours() in Java.
 * Query only — no notify.
 */
void graf_neighbours(t_graf *x, t_symbol *s, long argc, t_atom *argv)
{
    if (argc < 1) {
        object_error((t_object *)x, "neighbours: requires a node id");
        return;
    }

    t_symbol *id = graf_atom_to_id(argv);
    if (!id) {
        object_error((t_object *)x,
                     "neighbours: id must be a symbol, int, or float");
        return;
    }

    t_graf_node *n = graf_find_node(x, id);
    if (!n) {
        object_error((t_object *)x,
                     "neighbours: node '%s' not found", id->s_name);
        return;
    }

    if (n->edge_count == 0) {
        post("graf: node '%s' has no neighbours", id->s_name);
        return;
    }

    long   i;
    t_atom a;
    for (i = 0; i < n->edge_count; i++) {
        atom_setsym(&a, n->edges_to[i]);
        outlet_anything(x->outlet, gensym("neighbour"), 1, &a);
    }
}

/**
 * adjacent <u> <v>
 *
 * Output "adjacent 1" if directed edge u→v exists, "adjacent 0" otherwise.
 * Numeric ids accepted.
 * Equivalent to IGraph.adjacent() in Java.
 * Query only — no notify.
 */
void graf_adjacent(t_graf *x, t_symbol *s, long argc, t_atom *argv)
{
    if (argc < 2) {
        object_error((t_object *)x, "adjacent: requires two node ids");
        return;
    }

    t_symbol *u = graf_atom_to_id(argv);
    t_symbol *v = graf_atom_to_id(argv + 1);
    if (!u || !v) {
        object_error((t_object *)x,
                     "adjacent: ids must be symbols, ints, or floats");
        return;
    }

    t_graf_node *src = graf_find_node(x, u);
    if (!src) {
        object_error((t_object *)x,
                     "adjacent: node '%s' not found", u->s_name);
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
 * size — output "size <n>".
 * Equivalent to IGraph.size() in Java.
 * Query only — no notify.
 */
void graf_size(t_graf *x)
{
    t_atom a;
    atom_setlong(&a, x->node_count);
    outlet_anything(x->outlet, gensym("size"), 1, &a);
}

/**
 * name — output "name <symbol>", the registered instance name.
 * Equivalent to a getName() accessor in Java.
 * Query only — no notify.
 */
void graf_name(t_graf *x)
{
    t_atom a;
    atom_setsym(&a, x->name);
    outlet_anything(x->outlet, gensym("name"), 1, &a);
}

/**
 * print — dump entire graph structure to the Max console.
 * Query only — no notify.
 */
void graf_print(t_graf *x)
{
    long i, j;
    post("graf '%s': --- graph (%ld nodes) ---",
         x->name->s_name, x->node_count);

    for (i = 0; i < x->node_count; i++) {
        t_graf_node *n = &x->nodes[i];

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


// TODO
// logic might be tricky since navigation is per now handled by graf.traverse object. The data structure itself doesnt include 

/**
 * lastnode — output "lastnode <symbol>", the last visited node.
 * Query only — no notify.
 */
void graf_lastnode(t_graf *x)
{
    //TODO, need first to store visited nodes somewhere
}

/**
 * lastedge — output "lastedge <symbol> <symbol>", the last visited edge.
 * Query only — no notify.
 */
void graf_lastedge(t_graf *x)
{
    //TODO, need first to store visited edges somewhere
}

/**
 * visitednodes — output "visitednodes <symbol> ..." for all visited nodes.
 * Query only — no notify.
 */
void graf_visitednodes(t_graf *x)
{
    //TODO, need first to store visited nodes somewhere
}

/**
 * visitededges — output "visitededges <symbol> <symbol> ..." for all visited edges.
 * Query only — no notify.
 */
void graf_visitededges(t_graf *x)
{
    //TODO, need first to store visited edges somewhere
}

//TODO: implement cycles and components related functions
//void graf_hascycles returns 1/0 if yes/no
//void graf_cycles retruns int with the number of cycles
//void graf_components returns int with the number of components


////////////////////////// clear, write, read

/**
 * clear
 *
 * Remove all nodes and edges. Reset the graph to an empty state.
 *
 * The pre-allocated nodes array is kept at its current capacity — no
 * reallocation. next_node_id resets to 0 so auto-named nodes restart
 * from "node0". current is cleared.
 *
 * Notifies subscribers so graf.affiche redraws to an empty state.
 *
 * Java analogy: like calling clear() on an ArrayList — the backing array
 * stays allocated, but length resets to 0 and the old elements are released.
 */
void graf_clear(t_graf *x)
{
    long i;
    for (i = 0; i < x->node_count; i++) {
        t_graf_node *n = &x->nodes[i];
        if (n->payload)      sysmem_freeptr(n->payload);
        if (n->edges_to)     sysmem_freeptr(n->edges_to);
        if (n->edge_weights) sysmem_freeptr(n->edge_weights);
    }
    x->node_count   = 0;
    x->current      = NULL;
    x->next_node_id = 0;
    post("graf '%s': cleared", x->name->s_name);
    object_notify((t_object *)x, gensym("modified"), NULL);
}

/**
 * write [filename]
 *
 * Serialize the entire graph to a CSV file.
 *
 * With filename argument: written to that path directly.
 *   - Full POSIX path (/Users/me/graph.csv): used as-is.
 *   - Bare name (my_graph.csv): written to Max's default path (patcher folder).
 *
 * Without filename argument (A_DEFSYM sends gensym("")):
 *   Opens a system save dialog. Returns silently if user cancels.
 *
 * Uses Max's sysfile API (not stdio) for correct operation in Max's
 * sandboxed environment, consistent with buffer~ and dict.
 *
 * Java analogy: like a FileWriter, but with the OS file picker
 * triggered via JFileChooser when no path is provided.
 */
void graf_write(t_graf *x, t_symbol *filename)
{
    short        path;
    char         name[MAX_PATH_CHARS];
    t_filehandle fh;

    if (filename->s_name[0] == '\0') {
        // No filename given — open system save dialog.
           saveasdialog_extended returns 0 if the user confirmed a path,
           non-zero if they cancelled. The confirmed name and volume are
           written into name[] and path. */
        t_fourcc type = 0;
        strncpy(name, "untitled.csv", MAX_PATH_CHARS);
        if (saveasdialog_extended(name, &path, &type, NULL, 0) != 0)
            return; // user cancelled — silent exit */
    } else {
        // Filename provided — split into volume + bare name.
           path_frompathname handles full POSIX paths.
           Falls back to default path (patcher folder) for bare names. */
        if (path_frompathname(filename->s_name, &path, name) != 0) {
            path = path_getdefault();
            strncpy(name, filename->s_name, MAX_PATH_CHARS);
            name[MAX_PATH_CHARS - 1] = '\0';
        }
    }

    if (path_createsysfile(name, path, 0, &fh) != 0) {
        object_error((t_object *)x,
                     "write: could not create file '%s'", name);
        return;
    }

    graf_write_to_handle(x, fh);
    sysfile_close(fh);
    post("graf '%s': written to '%s' (%ld nodes)",
         x->name->s_name, name, x->node_count);
}

/**
 * read [filename]
 *
 * Load a graph from a CSV file, replacing the current contents.
 * Calls clear first, then parses the file.
 *
 * With filename argument: located by path or Max search path.
 *   - Full POSIX path: path_frompathname.
 *   - Bare name: locatefile_extended searches patcher folder then Max path.
 *
 * Without filename argument (A_DEFSYM sends gensym("")):
 *   Opens a system open dialog. Returns silently if user cancels.
 *
 * Load strategy:
 *   1. Resolve and open the file
 *   2. Read entire file into a heap buffer (sysfile_geteof + sysfile_read)
 *   3. Close file handle, clear graph, parse buffer, free buffer
 *   4. Single object_notify after the full load — graf.affiche redraws once.
 *
 * Note: bulk load uses quiet helpers (no per-node notify during the loop).
 * One notify fires at the end. This is cleaner than coalescing many notifies
 * and avoids any intermediate partial-graph redraws.
 */
void graf_read(t_graf *x, t_symbol *filename)
{
    short        path;
    char         name[MAX_PATH_CHARS];
    t_filehandle fh;
    t_fourcc     type = 0;

    if (filename->s_name[0] == '\0') {
        // No filename given — open system open dialog.
           open_dialog returns 0 if the user selected a file,
           non-zero if they cancelled. */
        name[0] = '\0';
        if (open_dialog(name, &path, &type, NULL, 0) != 0)
            return; // user cancelled — silent exit */
    } else {
        strncpy(name, filename->s_name, MAX_PATH_CHARS);
        name[MAX_PATH_CHARS - 1] = '\0';

        if (path_frompathname(filename->s_name, &path, name) != 0) {
            // bare filename: search Max's file path */
            strncpy(name, filename->s_name, MAX_PATH_CHARS);
            if (locatefile_extended(name, &path, &type, NULL, 0) != 0) {
                object_error((t_object *)x,
                             "read: file '%s' not found", filename->s_name);
                return;
            }
        }
    }

    if (path_opensysfile(name, path, &fh, READ_PERM) != 0) {
        object_error((t_object *)x,
                     "read: cannot open '%s'", name);
        return;
    }

    // read entire file into a heap buffer */
    t_ptr_size filesize;
    sysfile_geteof(fh, &filesize);

    char *buf = (char *)sysmem_newptr(filesize + 1);
    if (!buf) {
        object_error((t_object *)x, "read: out of memory");
        sysfile_close(fh);
        return;
    }

    t_ptr_size count = filesize;
    sysfile_read(fh, &count, buf);
    buf[count] = '\0';
    sysfile_close(fh);

    // clear existing graph, parse buffer, free buffer.
       graf_clear fires its own notify (empty-state redraw).
       graf_load_from_buffer uses quiet helpers — no per-node notifies.
       A second notify below triggers the final populated redraw. */
    graf_clear(x);
    graf_load_from_buffer(x, buf, count);
    sysmem_freeptr(buf);

    post("graf '%s': loaded '%s' — %ld nodes",
         x->name->s_name, name, x->node_count);
    object_notify((t_object *)x, gensym("modified"), NULL);
}