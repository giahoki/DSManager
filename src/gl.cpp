#include "gl.h"

#define X(ret, name, args) PFN_##name##_t name = nullptr;
GL_FUNCS
#undef X

bool glLoad(GLGetProc getProc)
{
    bool ok = true;
#define X(ret, name, args) name = (PFN_##name##_t)getProc(#name); if (!name) ok = false;
    GL_FUNCS
#undef X
    return ok;
}
