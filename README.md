# max-graf-library

Max/MSP externals implementing a directed weighted graph as a non-linear sequencer. Written in C using the Cycling '74 Max SDK.

## Objects
graf — directed weighted graph data store
graf.traverse — traversal algorithms (weighted random, DFS, BFS)

## TODO list
TODO: implement "graf.affiche", a graf object vizualizer (white window, with simple circles representing vertices and lines to represent edges, eventually a green overlay on the current visited node, orange overlays on alreadyvisited nodes, blue overlay for adjacent nodes, weight in a little textbox by the edge line, etc)

TODO: implement load/open/read/write/edit/clear/undo/redo functions/message handlers in "graf"

TODO: implement mode dijkstra in "graf.traverse"

TODO: implement mode astar in "graf.traverse"

## Setup on a new machine
1. Clone the Max SDK somewhere with no spaces in the path
cd ~/Documents/dev
mkdir max-sdk-sandbox && cd max-sdk-sandbox
git clone https://github.com/Cycling74/max-sdk.git
2. Clone this repo into the SDK source folder
cd max-sdk/source
git clone git@github.com:yourname/max-graf-library.git graf
3. Fix the SDK's CMakeLists.txt
From the max-sdk root, run these commands:
sed -i '' 's/add_subdirectory(source/advanced)/# add_subdirectory(source/advanced)/' CMakeLists.txt
sed -i '' 's/add_subdirectory(source/audio)/# add_subdirectory(source/audio)/' CMakeLists.txt
sed -i '' 's/add_subdirectory(source/basics)/# add_subdirectory(source/basics)/' CMakeLists.txt
sed -i '' 's/add_subdirectory(source/dictionary)/# add_subdirectory(source/dictionary)/' CMakeLists.txt
sed -i '' 's/add_subdirectory(source/gl)/# add_subdirectory(source/gl)/' CMakeLists.txt
sed -i '' 's/add_subdirectory(source/matrix)/# add_subdirectory(source/matrix)/' CMakeLists.txt
sed -i '' 's/add_subdirectory(source/mc)/# add_subdirectory(source/mc)/' CMakeLists.txt
sed -i '' 's/add_subdirectory(source/misc)/# add_subdirectory(source/misc)/' CMakeLists.txt
sed -i '' 's/add_subdirectory(source/patcher)/# add_subdirectory(source/patcher)/' CMakeLists.txt
sed -i '' 's/add_subdirectory(source/ui)/# add_subdirectory(source/ui)/' CMakeLists.txt
echo "add_subdirectory(source/graf)" >> CMakeLists.txt
4. Build
mkdir build && cd build
cmake ..
cmake --build . --config Release
5. Copy externals to Max package
cp -r externals/graf.mxo ~/Documents/Max\ 8/Packages/graf/externals/
cp -r externals/graf.traverse.mxo ~/Documents/Max\ 8/Packages/graf/externals/

## Daily workflow
Start of session: git pull
End of session: git add . && git commit -m "message" && git push