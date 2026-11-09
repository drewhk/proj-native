/* Static extension callbacks; all private PROJ layouts stay in this adapter. */
#include "extensions.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <vector>

using namespace NS_PROJ;

struct PROJ_EXT_GRID {
    std::unique_ptr<GenericShiftGridSet> set;
};

struct PROJ_EXT_GRID_VIEW {
    const GenericShiftGrid *grid;
    PROJ_EXT_GRID_INFO info;
};

namespace {
struct Registry {
    std::mutex mutex;
    std::map<std::string, PROJ_EXT_OPERATION> operations;
    std::map<std::string, PROJ_EXT_INTERPOLATE> interpolators;
};

Registry &registry() {
    static Registry instance;
    return instance;
}

int error(PJ_CONTEXT *ctx, int code) {
    proj_context_errno_set(ctx ? ctx : pj_get_default_ctx(), code);
    return code;
}

bool valid_name(const char *name) {
    if (!name || !*name)
        return false;
    for (const char *p = name; *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
              *p == '_'))
            return false;
    }
    return true;
}

bool valid_bands(const GenericShiftGrid *grid, int count, const int *bands) {
    if (!bands || count <= 0 || count > grid->samplesPerPixel())
        return false;
    for (int i = 0; i < count; ++i) {
        if (bands[i] < 0 || bands[i] >= grid->samplesPerPixel())
            return false;
    }
    return true;
}

pj_io_units units(int domain) {
    switch (domain) {
    case PROJ_EXT_RADIANS:
        return PJ_IO_UNITS_RADIANS;
    case PROJ_EXT_PROJECTED:
        return PJ_IO_UNITS_PROJECTED;
    case PROJ_EXT_CARTESIAN:
        return PJ_IO_UNITS_CARTESIAN;
    }
    return PJ_IO_UNITS_WHATEVER;
}

struct Operation {
    PROJ_EXT_OPERATION callbacks{};
    PROJ_EXT_OPERATION_INSTANCE instance{};
    std::string name;
};

PJ *destroy_operation(PJ *P, int code) {
    if (P && P->opaque) {
        auto *op = static_cast<Operation *>(P->opaque);
        // Destructors must not throw, even when a client violates its contract.
        try {
            if (op->instance.state)
                op->callbacks.destroy(op->instance.state);
        } catch (...) {
        }
        delete op;
        P->opaque = nullptr;
    }
    return pj_default_destructor(P, code);
}

PJ_COORD transform(PJ_COORD coord, PJ *P, bool inverse) {
    auto *op = static_cast<Operation *>(P->opaque);
    const auto callback =
        inverse ? op->callbacks.inverse : op->callbacks.forward;
    try {
        const int code = callback(P->ctx, op->instance.state, &coord);
        if (code) {
            error(P->ctx, code);
            return proj_coord_error();
        }
        // Time can be HUGE_VAL (unknown epoch), but spatial results cannot.
        for (int i = 0; i < 3; ++i) {
            if (!std::isfinite(coord.v[i])) {
                error(P->ctx, PROJ_ERR_COORD_TRANSFM_INVALID_COORD);
                return proj_coord_error();
            }
        }
        return coord;
    } catch (...) {
        error(P->ctx, PROJ_ERR_OTHER);
        return proj_coord_error();
    }
}

void forward(PJ_COORD &coord, PJ *P) { coord = transform(coord, P, false); }
void inverse(PJ_COORD &coord, PJ *P) { coord = transform(coord, P, true); }

void reassign_context(PJ *P, PJ_CONTEXT *ctx) {
    auto *op = static_cast<Operation *>(P->opaque);
    if (op->callbacks.reassign_context) {
        try {
            op->callbacks.reassign_context(op->instance.state, ctx);
        } catch (...) {
            error(ctx, PROJ_ERR_OTHER);
        }
    }
}

PJ *create_operation(PJ *P) {
    if (!P) {
        P = pj_new();
        if (P) {
            P->need_ellps = 1;
            P->descr = "Registered extension operation";
        }
        return P;
    }
    try {
        auto op = std::unique_ptr<Operation>(new Operation());
        op->name = pj_param(P->ctx, P->params, "sproj").s;
        {
            auto &r = registry();
            std::lock_guard<std::mutex> lock(r.mutex);
            op->callbacks = r.operations.at(op->name);
        }
        P->opaque = op.release();
        P->destructor = destroy_operation;
        auto *state = static_cast<Operation *>(P->opaque);
        P->short_name = state->name.c_str();
        std::vector<const char *> args;
        for (auto param = P->params; param; param = param->next)
            args.push_back(param->param);
        const int code =
            state->callbacks.create(P->ctx, static_cast<int>(args.size()),
                                    args.data(), P->a, P->es, &state->instance);
        if (code)
            return destroy_operation(P, code);
        P->left = units(state->instance.input_domain);
        P->right = units(state->instance.output_domain);
        if (P->left == PJ_IO_UNITS_WHATEVER || P->right == PJ_IO_UNITS_WHATEVER)
            return destroy_operation(P, PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE);
        P->skip_fwd_prepare = P->skip_fwd_finalize = 1;
        P->skip_inv_prepare = P->skip_inv_finalize = 1;
        P->fwd4d = forward;
        P->inv4d = state->callbacks.inverse ? inverse : nullptr;
        P->reassign_context = reassign_context;
        return P;
    } catch (...) {
        return destroy_operation(P, PROJ_ERR_OTHER);
    }
}
} // namespace

int proj_ext_register_operation(PJ_CONTEXT *ctx, const char *name,
                                const PROJ_EXT_OPERATION *op) {
    if (!valid_name(name) || !op || op->abi_version != PROJ_EXT_ABI_VERSION ||
        op->struct_size < sizeof(*op) || !op->create || !op->forward ||
        !op->destroy)
        return error(ctx, PROJ_ERR_OTHER_API_MISUSE);
    for (const auto *builtin = proj_list_operations(); builtin->id; ++builtin) {
        if (strcmp(name, builtin->id) == 0)
            return error(ctx, PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE);
    }
    try {
        auto &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        if (!r.operations.emplace(name, *op).second)
            return error(ctx, PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE);
        return 0;
    } catch (...) {
        return error(ctx, PROJ_ERR_OTHER);
    }
}

PJ_CONSTRUCTOR pj_ext_find_operation(const char *name) {
    auto &r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    return r.operations.find(name) == r.operations.end() ? nullptr
                                                         : create_operation;
}

int proj_ext_register_interpolator(PJ_CONTEXT *ctx, const char *name,
                                   const PROJ_EXT_INTERPOLATOR *interpolator) {
    if (!valid_name(name) || !interpolator ||
        interpolator->abi_version != PROJ_EXT_ABI_VERSION ||
        interpolator->struct_size < sizeof(*interpolator) ||
        !interpolator->interpolate)
        return error(ctx, PROJ_ERR_OTHER_API_MISUSE);
    if (strcmp(name, "bilinear") == 0 || strcmp(name, "biquadratic") == 0)
        return error(ctx, PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE);
    try {
        auto &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        if (!r.interpolators.emplace(name, interpolator->interpolate).second)
            return error(ctx, PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE);
        return 0;
    } catch (...) {
        return error(ctx, PROJ_ERR_OTHER);
    }
}

PROJ_EXT_INTERPOLATE pj_ext_find_interpolator(const char *name) {
    if (!name)
        return nullptr;
    auto &r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    const auto it = r.interpolators.find(name);
    return it == r.interpolators.end() ? nullptr : it->second;
}

const PROJ_EXT_GRID_INFO *proj_ext_grid_info(const PROJ_EXT_GRID_VIEW *view) {
    return view ? &view->info : nullptr;
}

const char *proj_ext_grid_metadata(const PROJ_EXT_GRID_VIEW *view,
                                   const char *key, int band) {
    if (!view || !key || band < -1 || band >= view->grid->samplesPerPixel())
        return nullptr;
    try {
        return view->grid->metadataItem(key, band).c_str();
    } catch (...) {
        return nullptr;
    }
}

int proj_ext_grid_read(const PROJ_EXT_GRID_VIEW *view, int column, int row,
                       int width, int height, int count, const int *bands,
                       float *values, size_t value_count) {
    if (!view || !values || !valid_bands(view->grid, count, bands))
        return PROJ_ERR_OTHER_API_MISUSE;
    const auto &info = view->info;
    if (column < 0 || row < 0 || width <= 0 || height <= 0 ||
        width > info.width || height > info.height ||
        column > info.width - width || row > info.height - height)
        return PROJ_ERR_COORD_TRANSFM_OUTSIDE_GRID;
    if (static_cast<size_t>(width) > value_count / count / height ||
        static_cast<size_t>(width) * height * count >
            static_cast<size_t>(std::numeric_limits<int>::max()))
        return PROJ_ERR_OTHER_API_MISUSE;
    try {
        bool nodata = false;
        if (!view->grid->valuesAt(column, row, width, height, count, bands,
                                  values, nodata))
            return PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID;
        return nodata ? PROJ_ERR_COORD_TRANSFM_GRID_AT_NODATA : 0;
    } catch (...) {
        return PROJ_ERR_OTHER;
    }
}

int pj_ext_interpolate(PJ_CONTEXT *ctx, PROJ_EXT_INTERPOLATE interpolate,
                       const GenericShiftGrid *grid, double x, double y,
                       int count, const int *bands, double *values) {
    if (!interpolate || !grid || !values || !valid_bands(grid, count, bands) ||
        !std::isfinite(x) || !std::isfinite(y))
        return error(ctx, PROJ_ERR_OTHER_API_MISUSE);
    const auto &e = grid->extentAndRes();
    PROJ_EXT_GRID_VIEW view{grid,
                            {sizeof(PROJ_EXT_GRID_INFO), grid->width(),
                             grid->height(), grid->samplesPerPixel(),
                             e.isGeographic, e.west, e.south, e.east, e.north,
                             e.resX, e.resY}};
    try {
        std::vector<double> result(count,
                                   std::numeric_limits<double>::quiet_NaN());
        const int code =
            interpolate(ctx, &view, (x - e.west) / e.resX,
                        (y - e.south) / e.resY, count, bands, result.data());
        if (code)
            return error(ctx, code);
        for (double value : result) {
            if (!std::isfinite(value))
                return error(ctx, PROJ_ERR_COORD_TRANSFM_INVALID_COORD);
        }
        std::copy(result.begin(), result.end(), values);
        return 0;
    } catch (...) {
        return error(ctx, PROJ_ERR_OTHER);
    }
}

PROJ_EXT_GRID *proj_ext_grid_open(PJ_CONTEXT *ctx, const char *filename) {
    if (!ctx)
        ctx = pj_get_default_ctx();
    if (!filename || !*filename || strcmp(filename, "null") == 0) {
        error(ctx, PROJ_ERR_OTHER_API_MISUSE);
        return nullptr;
    }
    try {
        auto result = std::unique_ptr<PROJ_EXT_GRID>(new PROJ_EXT_GRID());
        result->set = GenericShiftGridSet::open(ctx, filename);
        if (result->set)
            return result.release();
        error(ctx, PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID);
    } catch (...) {
        error(ctx, PROJ_ERR_OTHER);
    }
    return nullptr;
}

void proj_ext_grid_close(PROJ_EXT_GRID *grid) { delete grid; }

int proj_ext_grid_sample(PJ_CONTEXT *ctx, PROJ_EXT_GRID *handle, int geographic,
                         const char *type, double x, double y,
                         const char *interpolation, int count, const int *bands,
                         double *values) {
    if (!ctx)
        ctx = pj_get_default_ctx();
    if (!handle || !std::isfinite(x) || !std::isfinite(y) ||
        (geographic != 0 && geographic != 1))
        return error(ctx, PROJ_ERR_OTHER_API_MISUSE);
    try {
        const auto callback = pj_ext_find_interpolator(interpolation);
        if (!callback)
            return error(ctx, PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE);
        handle->set->reassign_context(ctx);
        while (true) {
            const auto *grid = type ? handle->set->gridAt(type, x, y)
                                    : handle->set->gridAt(x, y);
            if (!grid)
                return error(ctx, PROJ_ERR_COORD_TRANSFM_OUTSIDE_GRID);
            const auto &extent = grid->extentAndRes();
            if (extent.isGeographic != static_cast<bool>(geographic))
                return error(ctx, PROJ_ERR_OTHER_API_MISUSE);
            double normalized_x = x;
            if (geographic) {
                if (normalized_x < extent.west)
                    normalized_x += 2 * M_PI;
                else if (normalized_x > extent.east)
                    normalized_x -= 2 * M_PI;
            }
            const int code = pj_ext_interpolate(
                ctx, callback, grid, normalized_x, y, count, bands, values);
            if (!grid->hasChanged())
                return code;
            if (!handle->set->reopen(ctx))
                return error(ctx,
                             PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID);
            proj_context_errno_set(ctx, 0);
        }
    } catch (...) {
        return error(ctx, PROJ_ERR_OTHER);
    }
}
