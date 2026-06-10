# graf — Max/MSP External Family

Directed weighted graph data structure for use as a non-linear sequencer in Max/MSP.
Written in C using the Cycling ’74 Max SDK.

**Externals in this family:**

- `graf` — graph data store (nodes, edges, weights, payloads, CSV serialization)
- `graf.traverse` — step-by-step traversal (weighted random walk, DFS, BFS)
- `graf.affiche` — real-time graph visualizer (jbox UI external)

-----

## Repository contents

This repository contains **source code only**. Built `.mxo` bundles are not
committed — they are architecture-specific and must be compiled locally on each machine.

```
max-graf-library/
    README.md
    .gitignore
    install.sh              ← run after every build to copy .mxo files to Max packages
    graf.h                  ← shared header (included by all externals)
    graf/
        graf.c
        CMakeLists.txt
    graf.traverse/
        graf.traverse.c
        CMakeLists.txt
    graf.affiche/
        graf.affiche.c
        CMakeLists.txt
    CMakeLists.txt          ← root: add_subdirectory for each external
    patches/
        graf_test.maxpat    ← reference test patch
```

-----

## First-time setup on a new machine

Do this once per machine. Takes about 10 minutes.

### 1. Clone the Max SDK to a space-free path

The SDK **must** live on a local path with no spaces. OneDrive paths contain
spaces and break CMake’s framework search flags on fresh builds.

```bash
mkdir -p ~/Documents/dev/max-sdk-sandbox
cd ~/Documents/dev/max-sdk-sandbox
git clone https://github.com/Cycling74/max-sdk.git
```

### 2. Edit the SDK root CMakeLists.txt

The SDK’s root `CMakeLists.txt` references example subdirectories that either
don’t compile or don’t exist. Comment them all out and add the graf family.

```bash
open ~/Documents/dev/max-sdk-sandbox/max-sdk/CMakeLists.txt
```

Find the block of `add_subdirectory(source/...)` lines. Comment out every one
of them, then add the graf family at the end:

```cmake
# add_subdirectory(source/basics/...)   ← comment out all of these
# add_subdirectory(source/audio/...)
# ... etc

add_subdirectory(source/graf)           ← add this line
```

Save and close.

### 3. Clone this repository into the SDK source tree

```bash
cd ~/Documents/dev/max-sdk-sandbox/max-sdk/source
git clone https://github.com/YOUR_USERNAME/max-graf-library.git graf
```

The clone must be named `graf` to match the `add_subdirectory(source/graf)`
line added above.

Verify the directory looks right:

```bash
ls ~/Documents/dev/max-sdk-sandbox/max-sdk/source/graf/
# should show: README.md  CMakeLists.txt  graf.h  graf/  graf.traverse/  graf.affiche/  install.sh  patches/
```

### 4. Create the Max packages folder

```bash
mkdir -p ~/Documents/Max\ 8/Packages/graf/externals
```

### 5. Configure the CMake build

```bash
cd ~/Documents/dev/max-sdk-sandbox/max-sdk
mkdir build && cd build
cmake ..
```

If this runs without errors, the environment is ready.

### 6. Build and install

```bash
cmake --build . --config Release
cd ../source/graf
bash install.sh
```

Open Max. Type `graf` in a new object box. If it instantiates without error,
setup is complete.

-----

## Day-to-day workflow

### Starting a session

Before writing any code, pull the latest changes from GitHub.

**In GitHub Desktop:** click `Fetch origin`, then `Pull origin` if there are
incoming commits.

Or from the terminal:

```bash
cd ~/Documents/dev/max-sdk-sandbox/max-sdk/source/graf
git pull
```

### Making changes

Edit source files directly inside
`~/Documents/dev/max-sdk-sandbox/max-sdk/source/graf/`.

These files are inside the git repository — any changes you make here are
tracked automatically by GitHub Desktop.

### Building after changes

```bash
cd ~/Documents/dev/max-sdk-sandbox/max-sdk/build
cmake --build . --config Release
```

CMake only recompiles files that changed. A typical incremental build takes
a few seconds.

If the build fails with errors you don’t recognise, see the
[Troubleshooting](#troubleshooting) section below.

### Installing to Max

After every successful build, run the install script to copy the new `.mxo`
bundles to your Max packages folder:

```bash
cd ~/Documents/dev/max-sdk-sandbox/max-sdk/source/graf
bash install.sh
```

Then restart Max (or use `Max Menu → Extras → Refresh Max` if available)
so it picks up the new externals.

### Committing and pushing

**In GitHub Desktop:**

1. Review the changed files in the left panel — should only be `.c`, `.h`,
   `.md`, `.maxpat` files. If you see any `.mxo` files listed, stop and check
   your `.gitignore` (see below).
1. Write a short commit message describing what changed.
1. Click `Commit to main`.
1. Click `Push origin`.

Or from the terminal:

```bash
cd ~/Documents/dev/max-sdk-sandbox/max-sdk/source/graf
git add -A
git commit -m "your message here"
git push
```

-----

## Syncing between machines

When Machine A is ahead of Machine B:

On Machine B, pull the latest source and rebuild:

```bash
# Pull
cd ~/Documents/dev/max-sdk-sandbox/max-sdk/source/graf
git pull

# Build
cd ~/Documents/dev/max-sdk-sandbox/max-sdk/build
cmake --build . --config Release

# Install
cd ~/Documents/dev/max-sdk-sandbox/max-sdk/source/graf
bash install.sh
```

That’s it. Both machines are now running identical source and freshly compiled
binaries.

Never copy `.mxo` files between machines. They are compiled for one CPU
architecture and will silently misbehave or crash on a different one.

-----

## Full clean rebuild

Use this when you suspect the build cache is stale, after changing `CMakeLists.txt`,
or after a fresh SDK clone.

```bash
cd ~/Documents/dev/max-sdk-sandbox/max-sdk
rm -rf build
mkdir build && cd build
cmake ..
cmake --build . --config Release
cd ../source/graf
bash install.sh
```

A clean rebuild takes longer (compiles everything from scratch including SDK
helpers) but is always safe.

-----

## install.sh

The install script lives at the root of this repository. Its only job is to
copy freshly built `.mxo` bundles to the right place in your Max packages folder.

```bash
#!/bin/bash
# install.sh — copy built externals to Max 8 packages folder
# Run from the repo root after cmake --build

SDK_BUILD=~/Documents/dev/max-sdk-sandbox/max-sdk/build
MAX_PKG=~/Documents/Max\ 8/Packages/graf/externals

set -e  # stop on first error

cp -r "$SDK_BUILD/../externals/graf.mxo"          "$MAX_PKG/"
cp -r "$SDK_BUILD/../externals/graf.traverse.mxo" "$MAX_PKG/"
cp -r "$SDK_BUILD/../externals/graf.affiche.mxo"  "$MAX_PKG/"

echo "graf: externals installed to $MAX_PKG"
```

If the install step fails with a `No such file or directory` error on the
`.mxo` side, the build did not succeed — check the CMake output for errors.
If it fails on the destination side, the Max packages folder does not exist
yet — run the `mkdir` command from step 4 above.

-----

## .gitignore

The repository root should contain a `.gitignore` with at minimum:

```
# Compiled externals — never commit binaries
*.mxo
*.mxo/

# CMake build artifacts
build/

# macOS
.DS_Store
**/.DS_Store

# Editor
.vscode/
*.swp
```

To create it if it doesn’t exist yet:

```bash
cd ~/Documents/dev/max-sdk-sandbox/max-sdk/source/graf
cat > .gitignore << 'EOF'
*.mxo
*.mxo/
build/
.DS_Store
**/.DS_Store
.vscode/
*.swp
EOF
git add .gitignore
git commit -m "add .gitignore"
git push
```

-----

## Reference: all build commands in one place

```bash
# --- First-time setup ---
mkdir -p ~/Documents/dev/max-sdk-sandbox
cd ~/Documents/dev/max-sdk-sandbox
git clone https://github.com/Cycling74/max-sdk.git
# (edit CMakeLists.txt as described above)
cd max-sdk/source
git clone https://github.com/YOUR_USERNAME/max-graf-library.git graf
mkdir -p ~/Documents/Max\ 8/Packages/graf/externals
cd ~/Documents/dev/max-sdk-sandbox/max-sdk
mkdir build && cd build
cmake ..
cmake --build . --config Release
cd ../source/graf && bash install.sh

# --- Incremental build (day-to-day) ---
cd ~/Documents/dev/max-sdk-sandbox/max-sdk/build
cmake --build . --config Release
cd ../source/graf && bash install.sh

# --- Full clean rebuild ---
cd ~/Documents/dev/max-sdk-sandbox/max-sdk
rm -rf build && mkdir build && cd build
cmake ..
cmake --build . --config Release
cd ../source/graf && bash install.sh

# --- Pull and rebuild on second machine ---
cd ~/Documents/dev/max-sdk-sandbox/max-sdk/source/graf
git pull
cd ../../build
cmake --build . --config Release
cd ../source/graf && bash install.sh
```

-----

## Troubleshooting

**`cmake ..` fails with “no such file or directory” for source files**
The SDK’s root `CMakeLists.txt` still references example subdirectories that
don’t exist. Comment them all out — see step 2 of first-time setup.

**Build succeeds but Max says “object not found”**
The install step was skipped, or Max hasn’t reloaded its packages. Run
`bash install.sh` and restart Max.

**`graf.affiche` draws the graph but the current node doesn’t highlight**
The patcher is missing the `[prepend goto]` → `[graf]` connection from the
`graf.traverse` left outlet. `graf.affiche` reads `graf->current` directly —
`graf.traverse` does not write back to it. The connection must be made
explicitly in the patch.

**`graf.affiche` shows “(not found)” instead of the graph**
The object was created before `[graf my_graph]` was instantiated, or the name
doesn’t match. Send `update my_graph` from a loadbang to force re-subscription.
This is a known open issue with the constructor argument parsing.

**Changes to `.c` files don’t seem to take effect**
Either the build failed silently (scroll up in the terminal for errors) or the
install step was skipped. Always run both `cmake --build` and `bash install.sh`.

**OneDrive path errors during cmake**
The SDK must not live on OneDrive. The spaces in `OneDrive - University of Bergen/`
break framework search flags. Clone the SDK to `~/Documents/dev/` as described
in step 1.