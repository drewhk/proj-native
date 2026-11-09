# Local extension ABI

`src/proj_extensions.h` is the C boundary for this fork. It is installed with
`proj.h`. This is not an upstream PROJ API. ABI version 1 supports statically
linked callbacks; it does not load plugins or register EPSG/WKT methods.

## Registration and operations

Register operations and interpolators once during application initialization.
Registration is process-wide, immutable, and protected by a mutex. Names and
descriptors are copied; callback code must stay loaded until process exit.
Duplicate names and overrides of built-in names fail. Use an application prefix.

A registered operation is available as `+proj=<name>` in an explicit pipeline.
Its constructor receives expanded PROJ parameters and the selected ellipsoid.
It creates per-operation state and declares its input and output domains:
geographic radians, projected metres, or geocentric metres. The callback owns
parameter validation and must preserve coordinate components it does not change.
The adapter does not apply origin, scale, ellipsoid-radius, axis or unit changes
around the callback; express those as explicit pipeline steps when needed.

The forward callback is required; an absent inverse is reported through PROJ's
normal no-inverse behaviour. A supplied inverse must invert the forward map.
Return a PROJ error code on failure. Do not throw C++ exceptions or unwind Rust
panics across the C boundary. State allocated before a construction failure is
destroyed, and normal destruction releases each non-null state once.

`proj_clone` reconstructs independent state from the stored operation definition,
including inside pipelines. A callback's state may cache resources but must not
carry history that changes coordinate results. Context reassignment calls the
optional reassignment hook; use it for cached child PJ objects. All transform
callbacks also receive the current context. Different PJ instances may run in
parallel, but a single instance keeps PROJ's usual single-thread-at-a-time rule.

## Interpolation and grid access

`+proj=gridshift +interpolation=<registered-name>` selects a custom interpolator.
The grid's `interpolation_method` metadata can also name it. Existing bilinear
and biquadratic code paths are unchanged. Custom interpolation uses the existing
iterative gridshift inverse, not the biquadratic-specific approximate reverse.
PROJ still selects grids/subgrids, identifies bands, and applies the existing
gridshift unit conversions and constant offsets.

During interpolation a borrowed opaque view provides grid geometry, metadata
and rectangular windows of decoded band values. The callback chooses its stencil
and edge policy. Reads reject out-of-bounds windows and nodata rather than silently
substituting values. Raw samples retain their band units and sign conventions.
Do not retain a view or any pointer obtained from it after the callback returns.

`proj_ext_grid_open/sample/close` lets a custom operation sample a grid directly,
for example to compute `H_out = a(x,y) * H_in + b(x,y)`. This interface uses the
same generic GeoTIFF reader as gridshift, including its cache, file/network access
and subgrid selection. It currently accepts registered interpolators only; native
bilinear/biquadratic sampling remains available through a child gridshift PJ.
The caller explicitly supplies geographic/projected coordinate kind and band
indices. Geographic positions are radians; projected positions are grid CRS
units. A TYPE filter is optional. The caller applies any band-unit/sign conversion.
Use an explicit path for a file named `null`; the null-grid sentinel is rejected.

This slice does not extend the separate legacy hgridshift/vgridshift readers or
implement Czech interpolation, Swiss HTRANS, or new country presets. It provides
the interfaces for those algorithms without embedding them in native PROJ.

## Verification and build

With `BUILD_TESTING=ON` and TIFF enabled, build `proj_extension_test` and run
`ctest -R '^extensions$' --output-on-failure`. The test uses only the public C ABI
(also compiled as C), without GTest. It checks operation lifetime, errors, domains,
cloning, serialization, inverse pipelines, grid sampling and custom interpolation
against the existing gridshift implementation. The toy height-scaling operation
is a contract test, not a country implementation or conformance reference.

Static TIFF consumers must also link TIFF's dependencies (notably zlib); the Rust
wrapper normally provides them at its final link step. When running the native
test executable from that build tree, supply the existing zlib library through
the executable link configuration. No Rust or Dart bindings are regenerated here.
