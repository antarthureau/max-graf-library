# graf.affiche — jbox UI external for the graf family
# Follows the same pattern as graf/ and graf.traverse/
 
set_property(DIRECTORY PROPERTY max::external YES)
set_property(DIRECTORY PROPERTY max::glob ON)
 
# Point up one level to source/graf/ where graf.h lives
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/..")
 
project(graf.affiche)