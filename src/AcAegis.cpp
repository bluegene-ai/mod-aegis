#include "AcAegisScripts.h"

// AzerothCore derives this entry point from the module's DIRECTORY name, so the two must
// stay in sync: src/cmake/macros/ConfigureModules.cmake collects every directory under
// modules/, then modules/CMakeLists.txt replaces '-' with '_' in that name and generates a
// call to "Add<name>Scripts()" (ModulesLoader.cpp.in.cmake, from the generated script
// loader).
//
// The directory is modules/mod-aegis, so the generated symbol is Addmod_aegisScripts().
// This file used to export Addmod_ac_aegisScripts() - the name from when the directory was
// called mod-ac-aegis - which left the generated loader with an undefined reference and
// kept the whole module out of the build. If the directory is ever renamed, this function
// has to be renamed with it.
void Addmod_aegisScripts()
{
    startAcAegisScripts();
}
