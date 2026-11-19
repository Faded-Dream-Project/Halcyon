/* gl_loader.c — runtime GL loading (the FDK example's pattern).
 *
 * dlopen the platform GL library, resolve through
 * glXGetProcAddressARB / eglGetProcAddress (whichever exists), fall
 * back to dlsym. POSIX blesses the object->function store idiom
 * (the conditional-call form trips -Wpedantic); FDK's own lane does
 * the same (x11_gl.c's FDK_GL_STORE_FN).
 */
#include "gl_loader.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

hal_gl_table hal_gl;

static bool g_loaded;
static bool g_ready;
static char g_version[128];

/* Which library + loader worked, for --gpu-info honesty. */
static char g_backend[16];

static void *open_lib(const char *const *names) {
    for (int i = 0; names[i]; i++) {
        void *lib = dlopen(names[i], RTLD_LAZY | RTLD_LOCAL);
        if (lib)
            return lib;
    }
    return NULL;
}

typedef void *(*hal_gl_gpa_fn)(const char *);

static hal_gl_gpa_fn resolve_loader(void *lib, const char *const *names) {
    for (int i = 0; names[i]; i++) {
        void *sym = dlsym(lib, names[i]);
        if (sym) {
            hal_gl_gpa_fn fn;
            /* POSIX-blessed store (see the header comment). */
            *(void **)&fn = sym;
            return fn;
        }
    }
    return NULL;
}

bool hal_gl_load(void) {
    if (g_loaded)
        return g_ready;
    g_loaded = true;
    g_ready = false;
    g_version[0] = '\0';
    g_backend[0] = '\0';

    /* Desktop GL first (libGL carries glX), then GLES/ANGLE-style
     * (libEGL carries eglGetProcAddress). The engaged FDK context
     * decides which functions actually work at call time; the loader
     * just needs to find the entry points. */
    static const char *const gl_names[] = { "libGL.so.1", "libGL.so",
                                            NULL };
    static const char *const glx_loader[] = { "glXGetProcAddressARB",
                                              "glXGetProcAddress",
                                              NULL };
    static const char *const egl_names[] = { "libEGL.so.1",
                                             "libEGL.so", NULL };
    static const char *const egl_loader[] = { "eglGetProcAddress", NULL };

    void *lib = open_lib(gl_names);
    hal_gl_gpa_fn gpa = NULL;
    const char *via = NULL;
    if (lib) {
        gpa = resolve_loader(lib, glx_loader);
        if (gpa) {
            via = "glX";
        } else {
            gpa = resolve_loader(lib, egl_loader);
            if (gpa)
                via = "egl(libGL)";
        }
    }
    if (!gpa) {
        void *elib = open_lib(egl_names);
        if (elib) {
            gpa = resolve_loader(elib, egl_loader);
            if (gpa)
                via = "egl";
            if (!lib)
                lib = elib;
        }
    }
    if (!gpa || !lib) {
        fprintf(stderr,
                "hal-gl: no GL library/loader available "
                "(libGL glX/egl or libEGL)\n");
        return false;
    }
    snprintf(g_backend, sizeof g_backend, "%s", via);

#define RESOLVE(field, name)                                            \
    do {                                                                \
        if (!hal_gl.field)                                              \
            *(void **)&hal_gl.field = dlsym(lib, name);                 \
    } while (0)

#define X(fn, glname, ret, args)                                         \
    do {                                                                 \
        if (gpa)                                                         \
            *(void **)&hal_gl.fn = gpa("gl" #glname);                    \
        RESOLVE(fn, "gl" #glname);                                       \
    } while (0);
    HAL_GL_FUNCS(X)
#undef X
#undef RESOLVE

    /* The core minimum the renderer needs: shaders, buffers, VAOs,
     * textures, framebuffers, readback. Timer queries + MSAA are
     * optional (reported through their own function pointers). */
    const bool core_ok =
        hal_gl.create_shader && hal_gl.create_program &&
        hal_gl.link_program && hal_gl.use_program &&
        hal_gl.gen_buffers && hal_gl.bind_buffer &&
        hal_gl.buffer_data && hal_gl.gen_vertex_arrays &&
        hal_gl.bind_vertex_array && hal_gl.vertex_attrib_pointer &&
        hal_gl.draw_elements && hal_gl.gen_textures &&
        hal_gl.tex_image_2d && hal_gl.clear && hal_gl.viewport &&
        hal_gl.read_pixels && hal_gl.get_error;
    if (!core_ok) {
        fprintf(stderr,
                "hal-gl: the GL library lacks core functions "
                "(shaders/buffers/VAOs) — cannot run the GPU "
                "viewport\n");
        return false;
    }
    g_ready = true;
    return true;
}

bool hal_gl_ready(void) {
    return g_ready;
}

const char *hal_gl_version_string(void) {
    if (!g_ready || !hal_gl.get_string)
        return "<none>";
    if (g_version[0] == '\0') {
        const char *v = hal_gl.get_string(HAL_GL_VERSION);
        snprintf(g_version, sizeof g_version, "%s", v ? v : "<null>");
    }
    return g_version;
}

const char *hal_gl_loader_backend(void) {
    return g_backend[0] ? g_backend : "<none>";
}
