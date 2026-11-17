// Translation unit owned by the shared library target. All exported
// functions live in engine_abi.cpp (part of the mixit_engine_core OBJECT
// library, whose objects are linked into the DLL). Keeping this file lets the DLL target exist without
// compiling engine sources twice.
//
// No DllMain on purpose: the engine performs no work at load/unload time.
// Hosts must destroy every engine before unloading the library.
#include "mixit/mixit_engine.h"
