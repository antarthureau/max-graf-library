# graf

A graph data structure for Max/MSP, implemented as a family of C
externals against the Cycling '74 SDK.

`graf` is grounded in discrete mathematics and formal computer science:
a set of objects and weighted relations between them.
It's aimed for example at machine states, Markov modeling, non-linear sequencing,
algorithmic composition, data acquisition, etc...
It allows building graphs by hand (patching or importing CSV data), generating graphs in real-time from observed data (pitch, amplitude, or any other data), and graph navigation through classic algorithms like weighted random walk, BFS, DFS, or Dijkstra (and more if you patch specific navigation logic using other Max objects).

-----

## Why

Max has no native graph data structure, and often I find myself needing one! That's why :)

-----

## The model

A graf instance is a directed weighted graph G = (V, R):

V is a set of uniquely named nodes (=vertices). Each node may carry an optional
payload — arbitrary Max atoms (pitch, duration, velocity, or anything
else) attached to that state.

R ⊆ V × V × ℝ is the set of directed, weighted edges between nodes —
the possible transitions and their relative weight.

The weight of an edge means different things depending on how the graph is
traversed: in a random walk, higher weight means more probable; in
shortest-path search, lower weight means lower cost. Both readings operate
on the same underlying structure.

-----

## The object family

### graf — the data store

Holds the graph itself: nodes, edges, weights, payloads. Every other
object in the family reads a named `graf` instance by reference rather
than owning its own copy, so multiple traversers or visualizers can share
one graph.

    addnode [id] [payload...]    add a node (id optional, auto-generated if omitted)
    removenode <id>               remove a node and its edges
    addedge <u> <v> [weight]      add directed edge u -> v (weight defaults to 0.0)
    removeedge <u> <v>            remove directed edge u -> v
    goto <id>                     teleport current position to a node, ignoring edges
    next                          move to a random neighbour of the current node
    bang                          output current node id + payload
    hasnode <id>                  1 if node exists, 0 otherwise
    neighbours <id>                output all direct neighbours of a node
    adjacent <u> <v>              1 if edge u -> v exists, 0 otherwise
    size                          output number of nodes
    print                         dump the graph to the Max console
    clear                         remove all nodes and edges
    write [filename]              save the graph to a CSV file
    read [filename]                load a graph from a CSV file

`[graf my_graph]` registers under a name other objects can find it by.
`[graf]` with no argument still works, auto-named `graf_0`, `graf_1`, ...

### graf.traverse — navigation engine

Walks a named `graf` instance step by step, without modifying it. The
graph and the traversal over it are separate objects on purpose — several
traversers can move over the same graph independently and at the same time.
A traverser can target a different destination than another one searching the same
graph.

    step                  advance one step; new node id on the left outlet
    reset [<id> [<tgt>]]  reset the traversal; optional start and target
    mode random           weighted random walk (default) — weight = likelihood
    mode dfs              depth-first
    mode bfs              breadth-first
    mode dijkstra         shortest weighted path to a target — weight = cost
    from <id>             set the start node for future resets
    to <id>               set the target node for shortest-path search
    bang                  re-output the current node without advancing

Left outlet emits each node as it's visited; right outlet bangs when the
traversal completes or hits a dead end.

### graf.affiche — visualizer

Draws a named `graf` instance live inside the patcher: nodes as circles,
edges as directed arrows labelled with their weight, current position
highlighted. Redraws automatically whenever the graph changes.

    bang            force an immediate redraw
    update <name>   switch to watching a different named graf instance

### graf.observe — transition learner

Listens to a stream of values and writes into a named
`graf` instance as raw transition counts — nodes and edges appear as
they're observed, rather than being hand-built. Useful for building a
graph from recorded or live material (pitch tracking, an existing
sequence, anything that can be reduced to a stream of symbols).

    record <v>...          observe a value (or list = sequence of values), add
                            or update the node and the transition from the
                            previous one
    order <n>               Markov order, 1-8 (default 1); context window of
                            the last n values, joined into a compound node id
    forget                  clear the context window without touching the
                            graph (use at a phrase boundary, so the last value
                            of one phrase doesn't get linked to the first of
                            the next)
    normalize [prob|cost]   convert accumulated counts into probabilities, or
                            into Dijkstra-compatible costs (cost = -log(p)).
                            Destructive — do this once learning is finished.
    bang                     post current state to the console

Left outlet emits `<from> <to> <count>` for each transition observed;
right outlet emits the id of any newly discovered node. Counts, not
normalized probabilities, are stored while learning, so repeated
observation keeps accumulating correctly; call `normalize` once to
convert into weights `graf.traverse` can use directly.

-----

## Example

    [graf my_graph]              — the data store
    [graf.traverse my_graph]     — walks it
    [graf.affiche my_graph]      — shows it

Build a small graph, either by sending messages or loading a CSV:

    addnode a, addnode b, addnode c
    addedge a b 0.7, addedge a c 0.3, addedge b a 1

Set `[graf.traverse my_graph]` to `mode random` and send repeated `step`
messages for a probability-weighted wander through the states. Switch to
`mode dijkstra`, send `reset a c`, and each `step` walks the cheapest path
from a to c instead.

-----

## File format

Graphs save and load as plain CSV — readable, diffable, and easy to
generate from other tools (a Python script, a spreadsheet, another
program's output). n declares a node/vertex, e declares an edge

    # graf CSV — graph data file
    # nodes: n, id [, payload...]
    # edges: e, from, to [, weight]
    n, a
    n, b
    n, c
    e, a, b, 0.7
    e, a, c, 0.3
    e, b, a, 1

-----

## Status

Under active development. `graf`, `graf.traverse` (random walk, DFS,
BFS, Dijkstra), `graf.affiche`, and `graf.observe` are all implemented
and working. Remaining before a first release: a visual rework of
`graf.affiche` (force-directed layout, zoom/pan, node dragging), a
duplicate-instance-name crash, and packaging/docs. A* search and further
structural operators (`graf.op`, `graf.info`, `graf.path`) are planned
after that. I will consider a Pure Data port once the basic Max version is stable, but you are welcome to work on it if inspired! :)

Known limitations at this stage: creating two named `[graf]` instances
that share a name across patches can and will crash Max; `[graf.affiche]` doesn't
 pick up its instance name at construction and needs a manual
`update <name>` message; in addition, the circular layout of `[graf.affiche]` quickly becomes hard to read, so this is a priority in my to-do list.

-----

## Building from source

Requires the Cycling '74 Max SDK and CMake.

    git clone https://github.com/Cycling74/max-sdk.git
    git clone <this repo> max-sdk/source/graf
    # add `add_subdirectory(source/graf)` to the SDK's root CMakeLists.txt
    cd max-sdk && mkdir build && cd build
    cmake ..
    cmake --build . --config Release
    cd ../source/graf && bash install.sh

`install.sh` copies the built externals into your Max packages folder.
The SDK must live on a path without spaces (OneDrive-style synced folders
will break the build). Detailed setup and troubleshooting notes can be
split out into a separate BUILDING.md if that would be useful — just ask.

-----

## Acknowledgments

Most of the C implementation in this repository was written with the assistance of Anthropic Claude/Claude Code, allowing heavy lifting and fast initial development.
Without LLM assistance I would definitely not have found the time to develop this project.


The conceptual baseline for the algorithms and data structures in `graf` (and much of the Java-style thinking behind everything) draws heavily on [Algorithms, 4th Edition](https://algs4.cs.princeton.edu/home/)
by Robert Sedgewick and Kevin Wayne.


`graf` is nonetheless an original C implementation for the Max/MSP environment, not a derivative of their codebase, but the algorithmic thinking owes them a real debt.

-----

## License

GPLv3. See LICENSE.