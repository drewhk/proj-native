/* Local PROJ fork extension ABI. Not part of the upstream PROJ API. */
#ifndef PROJ_EXTENSIONS_H
#define PROJ_EXTENSIONS_H

#include "proj.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROJ_EXT_ABI_VERSION 1

typedef enum {
    PROJ_EXT_RADIANS = 1,
    PROJ_EXT_PROJECTED = 2,
    PROJ_EXT_CARTESIAN = 3
} PROJ_EXT_DOMAIN;

typedef struct {
    void *state;
    int input_domain; /* A PROJ_EXT_DOMAIN value, checked by the adapter. */
    int output_domain;
} PROJ_EXT_OPERATION_INSTANCE;

/* Return 0 on success, otherwise a PROJ_ERR_* code. Arguments are borrowed,
 * expanded key=value strings (without '+'). The constructor owns validation
 * of its parameters and may copy arguments, but must not retain the pointers.
 * a/es are the ellipsoid selected by normal PROJ initialization.
 * If create fails after assigning state, destroy is still called for it.
 * NULL state is allowed and does not cause a destroy callback. */
typedef int (*PROJ_EXT_CREATE)(PJ_CONTEXT *, int argc, const char *const *argv,
                               double a, double es,
                               PROJ_EXT_OPERATION_INSTANCE *instance);
typedef int (*PROJ_EXT_TRANSFORM)(PJ_CONTEXT *, void *state, PJ_COORD *coord);
typedef void (*PROJ_EXT_DESTROY)(void *state);
typedef void (*PROJ_EXT_REASSIGN_CONTEXT)(void *state, PJ_CONTEXT *);

typedef struct {
    unsigned int abi_version;
    size_t struct_size;
    PROJ_EXT_CREATE create;
    PROJ_EXT_TRANSFORM forward;
    PROJ_EXT_TRANSFORM inverse; /* NULL means no inverse is available. */
    PROJ_EXT_DESTROY destroy;
    PROJ_EXT_REASSIGN_CONTEXT reassign_context; /* Optional. */
} PROJ_EXT_OPERATION;

/* Status-returning API functions return 0 on success, otherwise PROJ_ERR_*.
 * Registrations are immutable and process-wide. Register before constructing
 * pipelines. Descriptors and names are copied; callback code must remain loaded
 * until process exit. There is no replacement or unregister API. Built-in and
 * duplicate names are rejected. ctx is used for error reporting only.
 * All callbacks must be non-throwing (including no Rust unwind across C).
 * Different instances may be called concurrently; one PJ retains PROJ's usual
 * single-thread-at-a-time rule. Cloning reconstructs state from the definition,
 * so state must be a cache/resources, not observable transformation history.
 * Coordinates use radians/metres; z is metres and t follows PROJ's time
 * contract. Callbacks receive complete coordinates: no implicit origin, scale,
 * ellipsoid radius, axis or unit adjustments are applied around them. Use
 * pipeline steps for those adjustments. Preserve unaffected components
 * explicitly. */
int PROJ_DLL proj_ext_register_operation(PJ_CONTEXT *ctx, const char *name,
                                         const PROJ_EXT_OPERATION *operation);

typedef struct PROJ_EXT_GRID PROJ_EXT_GRID;
typedef struct PROJ_EXT_GRID_VIEW PROJ_EXT_GRID_VIEW;

typedef struct {
    size_t struct_size;
    int width;
    int height;
    int band_count;
    int is_geographic;
    double west, south, east, north, res_x, res_y;
} PROJ_EXT_GRID_INFO;

/* Views, info and metadata strings are borrowed for the interpolation callback
 * only. Positions and spacing are radians for geographic grids, otherwise grid
 * CRS units. Window rows run south-to-north, columns west-to-east; output is
 * pixel-interleaved in requested band order. Bands are zero-based. Values are
 * decoded by PROJ (including scale/offset), but retain the band's units/sign.
 * UNITTYPE, DESCRIPTION, positive_value and other GeoTIFF metadata can be read.
 * Window reads return an error for out-of-bounds, nodata or I/O failure; there
 * is no implicit clamping, extrapolation or nodata substitution. */
const PROJ_EXT_GRID_INFO PROJ_DLL *
proj_ext_grid_info(const PROJ_EXT_GRID_VIEW *view);
const char PROJ_DLL *proj_ext_grid_metadata(const PROJ_EXT_GRID_VIEW *view,
                                            const char *key, int band);
int PROJ_DLL proj_ext_grid_read(const PROJ_EXT_GRID_VIEW *view, int column,
                                int row, int width, int height, int band_count,
                                const int *bands, float *values,
                                size_t value_count);

/* column/row are fractional node indices in the selected subgrid. The callback
 * chooses its stencil and edge policy, and must return one finite value per
 * requested band or a PROJ_ERR_* code. Propagate window-read errors. No view,
 * pointer or buffer may be retained after return. There is no per-call
 * userdata; registered interpolators are stateless and must be safe across
 * threads. */
typedef int (*PROJ_EXT_INTERPOLATE)(PJ_CONTEXT *, const PROJ_EXT_GRID_VIEW *,
                                    double column, double row, int band_count,
                                    const int *bands, double *values);
typedef struct {
    unsigned int abi_version;
    size_t struct_size;
    PROJ_EXT_INTERPOLATE interpolate;
} PROJ_EXT_INTERPOLATOR;

int PROJ_DLL
proj_ext_register_interpolator(PJ_CONTEXT *ctx, const char *name,
                               const PROJ_EXT_INTERPOLATOR *interpolator);

/* A single literal GeoTIFF filename, resolved with PROJ's file/search/network
 * machinery. No comma lists, optional-grid markers or null-grid sentinel.
 * Grid handles are independent of PJ instances and require explicit close.
 * Like PJ, a handle may be used by one thread at a time. sample reassigns its
 * context to ctx; the context must remain alive until reassigned or closed.
 * type can be NULL to select any TYPE; otherwise selection also filters TYPE.
 * is_geographic must match the selected grid. Longitude wrapping and subgrid
 * selection follow PROJ. The named interpolation must have been registered.
 * This API does not select a default interpolation or perform band-unit/sign
 * conversions. For normal shift semantics use +proj=gridshift instead. */
PROJ_EXT_GRID PROJ_DLL *proj_ext_grid_open(PJ_CONTEXT *ctx,
                                           const char *filename);
void PROJ_DLL proj_ext_grid_close(PROJ_EXT_GRID *grid);
int PROJ_DLL proj_ext_grid_sample(PJ_CONTEXT *ctx, PROJ_EXT_GRID *grid,
                                  int is_geographic, const char *type, double x,
                                  double y, const char *interpolation,
                                  int band_count, const int *bands,
                                  double *values);

#ifdef __cplusplus
}
#endif
#endif
