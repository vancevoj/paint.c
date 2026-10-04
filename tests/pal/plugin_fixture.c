/* plugin_fixture.c - tiny loadable module for the pal_lib_* tests. */
#include "fx/fx_abi.h"

FX_EXPORT int pal_fixture_add(int a, int b);
FX_EXPORT int pal_fixture_add(int a, int b)
{
    return a + b;
}
