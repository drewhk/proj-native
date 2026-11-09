#ifndef PROJ_EXTENSIONS_INTERNAL_HPP
#define PROJ_EXTENSIONS_INTERNAL_HPP
#include "grids.hpp"
#include "proj_extensions.h"
#include "proj_internal.h"

PJ_CONSTRUCTOR pj_ext_find_operation(const char *name);
PROJ_EXT_INTERPOLATE pj_ext_find_interpolator(const char *name);
int pj_ext_interpolate(PJ_CONTEXT *ctx, PROJ_EXT_INTERPOLATE interpolate,
                       const NS_PROJ::GenericShiftGrid *grid, double x,
                       double y, int count, const int *bands, double *values);
#endif
