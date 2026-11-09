#include "proj_extensions.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" unsigned int extension_test_c_abi(void);
namespace {
int checks = 0;
std::atomic<int> created{0}, destroyed{0}, reassigned{0};
void check(bool condition, const char *message) {
    ++checks;
    if (!condition)
        throw std::runtime_error(message);
}
using Op = std::unique_ptr<PJ, decltype(&proj_destroy)>;
Op operation(PJ_CONTEXT *ctx, const std::string &definition) {
    return Op(proj_create(ctx, definition.c_str()), proj_destroy);
}
void same(PJ_COORD a, PJ_COORD b, double tolerance) {
    for (int i = 0; i < 3; ++i)
        check(std::abs(a.v[i] - b.v[i]) <= tolerance, "coordinate mismatch");
    check(a.v[3] == b.v[3], "time was not preserved");
}

// A stateless bilinear interpolator supplied by a client. Comparing it with
// native gridshift exercises the callback boundary, not a new country
// algorithm.
int bilinear(PJ_CONTEXT *, const PROJ_EXT_GRID_VIEW *view, double x, double y,
             int count, const int *bands, double *out) {
    const auto *info = proj_ext_grid_info(view);
    if (!info || info->width < 2 || info->height < 2)
        return PROJ_ERR_COORD_TRANSFM_OUTSIDE_GRID;
    if (x < 0 || y < 0 || x > info->width - 1 || y > info->height - 1)
        return PROJ_ERR_COORD_TRANSFM_OUTSIDE_GRID;
    int col = std::min(static_cast<int>(std::floor(x)), info->width - 2);
    int row = std::min(static_cast<int>(std::floor(y)), info->height - 2);
    const double fx = x - col, fy = y - row;
    std::vector<float> samples(static_cast<size_t>(count) * 4);
    int err = proj_ext_grid_read(view, col, row, 2, 2, count, bands,
                                 samples.data(), samples.size());
    if (err)
        return err;
    for (int band = 0; band < count; ++band) {
        out[band] = (1 - fx) * (1 - fy) * samples[band] +
                    fx * (1 - fy) * samples[count + band] +
                    (1 - fx) * fy * samples[2 * count + band] +
                    fx * fy * samples[3 * count + band];
    }
    return 0;
}

// Operation state is recreated per PJ, including when the PJ is cloned.
// Optionally sample a grid to implement a spatially varying height multiplier.
struct State {
    PJ_CONTEXT *ctx = nullptr;
    PROJ_EXT_GRID *grid = nullptr;
    ~State() { proj_ext_grid_close(grid); }
};
int create(PJ_CONTEXT *ctx, int argc, const char *const *argv, double a,
           double es, PROJ_EXT_OPERATION_INSTANCE *instance) {
    if (!(a > 0 && es >= 0 && es < 1))
        return PROJ_ERR_OTHER_API_MISUSE;
    auto *state = new State;
    ++created;
    state->ctx = ctx;
    instance->state = state;
    instance->input_domain = instance->output_domain = PROJ_EXT_PROJECTED;
    for (int i = 0; i < argc; ++i) {
        if (std::strcmp(argv[i], "fail=1") == 0)
            return PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE;
        if (std::strcmp(argv[i], "bad_domain=1") == 0)
            instance->input_domain = 99;
        if (std::strcmp(argv[i], "angular=1") == 0)
            instance->input_domain = instance->output_domain = PROJ_EXT_RADIANS;
        if (std::strncmp(argv[i], "sample_grid=", 12) == 0) {
            state->grid = proj_ext_grid_open(ctx, argv[i] + 12);
            if (!state->grid)
                return PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID;
        }
    }
    return 0;
}
int apply(PJ_CONTEXT *ctx, void *opaque, PJ_COORD *coord, bool inverse) {
    auto *state = static_cast<State *>(opaque);
    if (ctx != state->ctx)
        return PROJ_ERR_OTHER_API_MISUSE;
    double scale = 2;
    if (state->grid) {
        const int band = 0;
        double sample;
        const int err = proj_ext_grid_sample(
            ctx, state->grid, 0, nullptr, coord->xy.x, coord->xy.y,
            "test_bilinear", 1, &band, &sample);
        if (err)
            return err;
        scale += sample;
    }
    coord->v[2] = inverse ? coord->v[2] / scale : coord->v[2] * scale;
    return 0;
}
int forward(PJ_CONTEXT *ctx, void *state, PJ_COORD *coord) {
    return apply(ctx, state, coord, false);
}
int inverse(PJ_CONTEXT *ctx, void *state, PJ_COORD *coord) {
    return apply(ctx, state, coord, true);
}
void destroy(void *state) {
    if (state) {
        ++destroyed;
        delete static_cast<State *>(state);
    }
}
void reassign(void *state, PJ_CONTEXT *ctx) {
    ++reassigned;
    static_cast<State *>(state)->ctx = ctx;
}
int fail_transform(PJ_CONTEXT *, void *, PJ_COORD *) {
    return PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN;
}
int fail_interpolation(PJ_CONTEXT *, const PROJ_EXT_GRID_VIEW *, double, double,
                       int, const int *, double *) {
    return PROJ_ERR_COORD_TRANSFM_GRID_AT_NODATA;
}
int incomplete_interpolation(PJ_CONTEXT *, const PROJ_EXT_GRID_VIEW *, double,
                             double, int, const int *, double *) {
    return 0;
}

// Synthetic, constant offsets on the existing real grid hierarchy. These make
// the inverse cross ALbanff's western edge into its parent deterministically,
// without depending on the displacement values in this particular dataset.
double subgrid_west = 0;
int subgrid_misses = 0, parent_samples = 0;
int crossing_interpolation(PJ_CONTEXT *, const PROJ_EXT_GRID_VIEW *view,
                           double x, double y, int count, const int *bands,
                           double *values) {
    const auto *info = proj_ext_grid_info(view);
    const bool child =
        std::strcmp(proj_ext_grid_metadata(view, "grid_name", -1), "ALbanff") ==
        0;
    if (child)
        subgrid_west = info->west;
    if (x < 0 || y < 0 || x > info->width - 1 || y > info->height - 1) {
        if (child)
            ++subgrid_misses;
        return PROJ_ERR_COORD_TRANSFM_OUTSIDE_GRID;
    }
    if (!child)
        ++parent_samples;
    for (int i = 0; i < count; ++i) {
        // This fixture stores latitude in band 0, longitude in band 1.
        values[i] = bands[i] == 1 ? 60.0 : 0.0; // arcseconds
    }
    return 0;
}

int inspect_subgrid(PJ_CONTEXT *, const PROJ_EXT_GRID_VIEW *view, double,
                    double, int, const int *bands, double *values) {
    const auto *info = proj_ext_grid_info(view);
    if (info->width != 11 || info->height != 21 || !info->is_geographic ||
        std::strcmp(proj_ext_grid_metadata(view, "grid_name", -1), "ALbanff") !=
            0)
        return PROJ_ERR_OTHER_API_MISUSE;
    float value;
    if (proj_ext_grid_read(view, -1, 0, 1, 1, 1, bands, &value, 1) !=
        PROJ_ERR_COORD_TRANSFM_OUTSIDE_GRID)
        return PROJ_ERR_OTHER_API_MISUSE;
    if (proj_ext_grid_read(view, 0, 0, 2, 2, 1, bands, &value, 1) !=
        PROJ_ERR_OTHER_API_MISUSE)
        return PROJ_ERR_OTHER_API_MISUSE;
    values[0] = 123;
    return 0;
}

void compare_grid(PJ_CONTEXT *ctx, const char *file, PJ_COORD input) {
    const std::string base = std::string("+proj=gridshift +grids=") + file;
    auto native = operation(ctx, base + " +interpolation=bilinear");
    auto custom = operation(ctx, base + " +interpolation=test_bilinear");
    check(native && custom, "gridshift creation");
    auto expected = proj_trans(native.get(), PJ_FWD, input);
    auto actual = proj_trans(custom.get(), PJ_FWD, input);
    same(actual, expected, 1e-8);
    same(proj_trans(custom.get(), PJ_INV, actual), input, 1e-7);
    Op clone(proj_clone(ctx, custom.get()), proj_destroy);
    check(clone != nullptr, "custom gridshift clone");
    same(proj_trans(clone.get(), PJ_FWD, input), expected, 1e-8);
}

void test(PJ_CONTEXT *ctx, PJ_CONTEXT *second) {
    check(extension_test_c_abi() == PROJ_EXT_ABI_VERSION, "C ABI header");
    PROJ_EXT_OPERATION callbacks{PROJ_EXT_ABI_VERSION,
                                 sizeof(PROJ_EXT_OPERATION),
                                 create,
                                 forward,
                                 inverse,
                                 destroy,
                                 reassign};
    check(proj_ext_register_operation(ctx, "test_height", &callbacks) == 0,
          "register operation");
    check(proj_ext_register_operation(ctx, "test_height", &callbacks) != 0,
          "reject duplicate");
    check(proj_ext_register_operation(ctx, "somerc", &callbacks) != 0,
          "reject built-in override");
    auto invalid = callbacks;
    invalid.abi_version++;
    check(proj_ext_register_operation(ctx, "test_wrong_abi", &invalid) != 0,
          "reject ABI mismatch");
    invalid = callbacks;
    invalid.struct_size = 1;
    check(proj_ext_register_operation(ctx, "test_small", &invalid) != 0,
          "reject short descriptor");
    auto failed = callbacks;
    failed.forward = fail_transform;
    check(proj_ext_register_operation(ctx, "test_fail", &failed) == 0,
          "register failure operation");
    auto forward_only = callbacks;
    forward_only.inverse = nullptr;
    check(proj_ext_register_operation(ctx, "test_forward", &forward_only) == 0,
          "register forward-only");

    PROJ_EXT_INTERPOLATOR interpolator{PROJ_EXT_ABI_VERSION,
                                       sizeof(PROJ_EXT_INTERPOLATOR), bilinear};
    check(proj_ext_register_interpolator(ctx, "test_bilinear", &interpolator) ==
              0,
          "register interpolator");
    check(proj_ext_register_interpolator(ctx, "test_bilinear", &interpolator) !=
              0,
          "reject interpolation replacement");
    check(proj_ext_register_interpolator(ctx, "bilinear", &interpolator) != 0,
          "protect native interpolation");
    interpolator.interpolate = fail_interpolation;
    check(proj_ext_register_interpolator(ctx, "test_nodata", &interpolator) ==
              0,
          "register failure interpolation");
    interpolator.interpolate = incomplete_interpolation;
    check(proj_ext_register_interpolator(ctx, "test_incomplete",
                                         &interpolator) == 0,
          "register incomplete interpolation");
    interpolator.interpolate = inspect_subgrid;
    check(proj_ext_register_interpolator(ctx, "test_inspect", &interpolator) ==
              0,
          "register grid inspector");
    interpolator.interpolate = crossing_interpolation;
    check(proj_ext_register_interpolator(ctx, "test_crossing", &interpolator) ==
              0,
          "register boundary-crossing interpolator");

    const PJ_COORD input = proj_coord(10, 20, 123.45, 2026);
    const PJ_COORD output = proj_coord(10, 20, 246.9, 2026);
    {
        auto op = operation(ctx, "+proj=test_height");
        check(op != nullptr, "create extension operation");
        same(proj_trans(op.get(), PJ_FWD, input), output, 0);
        same(proj_trans(op.get(), PJ_INV, output), input, 0);
        Op clone(proj_clone(second, op.get()), proj_destroy);
        check(clone != nullptr, "clone into a fresh context");
        op.reset();
        same(proj_trans(clone.get(), PJ_FWD, input), output, 0);
        proj_assign_context(clone.get(), ctx);
        same(proj_trans(clone.get(), PJ_INV, output), input, 0);
        check(reassigned > 0, "context reassignment callback");
        const char *definition =
            proj_as_proj_string(ctx, clone.get(), PJ_PROJ_5, nullptr);
        check(definition != nullptr, "serialize extension definition");
        auto reopened = operation(ctx, definition);
        check(reopened != nullptr, "reopen serialized definition");
        same(proj_trans(reopened.get(), PJ_FWD, input), output, 0);
    }
    {
        auto pipeline = operation(ctx, "+proj=pipeline +step +proj=test_height "
                                       "+step +inv +proj=test_height");
        check(pipeline != nullptr, "pipeline containing extension steps");
        same(proj_trans(pipeline.get(), PJ_FWD, input), input, 0);
        Op clone(proj_clone(second, pipeline.get()), proj_destroy);
        check(clone != nullptr, "pipeline clone");
        same(proj_trans(clone.get(), PJ_INV, input), input, 0);
        auto incompatible =
            operation(ctx, "+proj=pipeline +step +proj=test_height +step "
                           "+proj=test_height +angular=1");
        check(!incompatible, "reject mismatching pipeline domains");
        auto no_inverse =
            operation(ctx, "+proj=pipeline +step +inv +proj=test_forward");
        check(!no_inverse, "reject missing inverse");
        auto failure = operation(ctx, "+proj=test_fail");
        check(failure != nullptr, "create failure operation");
        check(std::isinf(proj_trans(failure.get(), PJ_FWD, input).v[0]),
              "transform failure result");
        check(proj_errno(failure.get()) ==
                  PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              "transform failure code");
    }
    int before = destroyed;
    check(!operation(ctx, "+proj=test_height +fail=1"), "constructor failure");
    check(destroyed == before + 1, "cleanup after constructor failure");
    check(!operation(ctx, "+proj=test_height +bad_domain=1"),
          "invalid callback domain");
    check(!operation(ctx, "+proj=unregistered_extension"),
          "reject unregistered operation");

    constexpr double rad = 3.14159265358979323846 / 180;
    compare_grid(ctx, "tests/test_hgrid_with_subgrid.tif",
                 proj_coord(-115.5416667 * rad, 51.1666667 * rad, 30, 2026));
    compare_grid(ctx, "tests/test_gridshift_projected.tif",
                 proj_coord(-598000, -1160020, 30, 2026));
    compare_grid(ctx, "tests/us_noaa_geoid06_ak_subset_at_antimeridian.tif",
                 proj_coord(-179.99 * rad, 54.5 * rad, 30, 2026));

    {
        auto crossing =
            operation(ctx, "+proj=gridshift "
                           "+grids=tests/test_hgrid_with_subgrid.tif "
                           "+interpolation=test_crossing");
        check(crossing != nullptr, "create boundary-crossing operation");
        proj_errno_reset(crossing.get());
        // First locate the child boundary through the public callback view.
        proj_trans(crossing.get(), PJ_FWD,
                   proj_coord(-115.5416667 * rad, 51.1666667 * rad, 30, 2026));
        check(subgrid_west != 0, "located ALbanff western boundary");
        subgrid_misses = parent_samples = 0;
        const auto target = proj_coord(subgrid_west + 30 * rad / 3600,
                                       51.1666667 * rad, 30, 2026);
        const auto recovered = proj_trans(crossing.get(), PJ_INV, target);
        check(subgrid_misses > 0 && parent_samples > 0,
              "inverse retried outside child and sampled parent");
        check(proj_errno(crossing.get()) == 0,
              "recovered subgrid miss must not poison successful inverse");
        same(recovered,
             proj_coord(subgrid_west - 30 * rad / 3600, target.v[1], 30, 2026),
             1e-12);
        same(proj_trans(crossing.get(), PJ_FWD, recovered), target, 1e-12);

        // A terminal callback failure still reaches the caller unchanged.
        auto failing =
            operation(ctx, "+proj=gridshift "
                           "+grids=tests/test_hgrid_with_subgrid.tif "
                           "+interpolation=test_nodata");
        check(failing != nullptr, "create failing gridshift");
        proj_errno_reset(failing.get());
        check(std::isinf(proj_trans(failing.get(), PJ_INV, target).v[0]),
              "unrecovered interpolation failure returns error coordinate");
        check(proj_errno(failing.get()) ==
                  PROJ_ERR_COORD_TRANSFM_GRID_AT_NODATA,
              "unrecovered interpolation failure keeps its error code");
    }

    std::unique_ptr<PROJ_EXT_GRID, decltype(&proj_ext_grid_close)> grid(
        proj_ext_grid_open(ctx, "tests/test_hgrid_with_subgrid.tif"),
        proj_ext_grid_close);
    check(grid != nullptr, "open raw grid");
    const int band = 0;
    double value;
    check(proj_ext_grid_sample(second, grid.get(), 1, nullptr,
                               -115.5416667 * rad, 51.1666667 * rad,
                               "test_inspect", 1, &band, &value) == 0,
          "subgrid geometry, metadata, bounds and context migration");
    check(value == 123, "inspector output");
    check(proj_ext_grid_sample(ctx, grid.get(), 0, nullptr, -115.5416667 * rad,
                               51.1666667 * rad, "test_bilinear", 1, &band,
                               &value) != 0,
          "reject coordinate-domain mismatch");
    check(proj_ext_grid_sample(ctx, grid.get(), 1, nullptr, 0, 0,
                               "test_bilinear", 1, &band,
                               &value) == PROJ_ERR_COORD_TRANSFM_OUTSIDE_GRID,
          "outside coverage");
    check(proj_ext_grid_sample(ctx, grid.get(), 1, nullptr, -115.5416667 * rad,
                               51.1666667 * rad, "test_nodata", 1, &band,
                               &value) == PROJ_ERR_COORD_TRANSFM_GRID_AT_NODATA,
          "propagate callback error");
    check(proj_ext_grid_sample(ctx, grid.get(), 1, nullptr, -115.5416667 * rad,
                               51.1666667 * rad, "test_incomplete", 1, &band,
                               &value) == PROJ_ERR_COORD_TRANSFM_INVALID_COORD,
          "reject unwritten callback output");
    check(proj_ext_grid_sample(ctx, grid.get(), 1, nullptr, -115.5416667 * rad,
                               51.1666667 * rad, "unregistered", 1, &band,
                               &value) != 0,
          "reject unregistered interpolation");
    grid.reset(proj_ext_grid_open(ctx, "tests/test_vgrid_nodata.tif"));
    check(grid != nullptr, "open nodata grid");
    check(proj_ext_grid_sample(ctx, grid.get(), 1, nullptr, 4.05 * rad,
                               52.1 * rad, "test_bilinear", 1, &band,
                               &value) == PROJ_ERR_COORD_TRANSFM_GRID_AT_NODATA,
          "nodata is not silently substituted");
    grid.reset(proj_ext_grid_open(
        ctx, "tests/test_vgrid_uint16_with_scale_offset.tif"));
    check(grid != nullptr, "open scaled integer grid");
    check(proj_ext_grid_sample(ctx, grid.get(), 1, nullptr, 4.5 * rad,
                               52.5 * rad, "test_bilinear", 1, &band,
                               &value) == 0,
          "sample decoded grid values");
    check(std::abs(value - 11.5) < 1e-12,
          "reader applies band scale and offset");
    {
        auto height = operation(
            ctx,
            "+proj=test_height +sample_grid=tests/test_3d_grid_projected.tif");
        check(height != nullptr, "operation using raw sampling");
        const auto point = proj_coord(1500300, 5400300, 200, 2026);
        const auto changed = proj_trans(height.get(), PJ_FWD, point);
        check(changed.v[2] != point.v[2],
              "sampled multiplicative height correction");
        same(proj_trans(height.get(), PJ_INV, changed), point, 1e-10);
        Op clone(proj_clone(second, height.get()), proj_destroy);
        check(clone != nullptr, "clone operation with grid resources");
        height.reset();
        same(proj_trans(clone.get(), PJ_FWD, point), changed, 1e-10);
    }
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    auto *ctx = proj_context_create();
    auto *second = proj_context_create();
    const char *paths[] = {argv[1]};
    proj_context_set_search_paths(ctx, 1, paths);
    proj_context_set_search_paths(second, 1, paths);
    try {
        test(ctx, second);
        check(created == destroyed,
              "all operation instances destroyed exactly once");
        std::cout << checks << " extension checks passed\n";
    } catch (const std::exception &e) {
        std::cerr << "Failure after " << checks << " checks: " << e.what()
                  << '\n';
        proj_context_destroy(ctx);
        proj_context_destroy(second);
        return 1;
    }
    proj_context_destroy(ctx);
    proj_context_destroy(second);
    return 0;
}
