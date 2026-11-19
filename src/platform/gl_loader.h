/* gl_loader.h — Halcyon's own OpenGL 3.3 core loader.
 *
 * Loads GL function pointers at runtime the way FDK's examples do
 * (examples/gl_viewport_3d.c): dlopen the GL library and resolve
 * through the platform's glX/eglGetProcAddress, falling back to
 * dlsym. NO link-time GL dependency — the build never links -lGL.
 *
 * FDK deliberately does not ship a GL loader (fdk_gl.h's CONTEXT
 * FLAVOR note); the engaged context may be core 3.3+, legacy compat
 * or GLES2, so every function this header exposes is resolved by
 * name at runtime and its presence is reported honestly
 * (hal_gl_ready()).
 *
 * Only the functions Halcyon's renderer uses are declared — a small,
 * explicit GL 3.3 core subset. All FDK access stays in src/platform/
 * (the house rule); this file is the GL side of that boundary.
 */
#ifndef HAL_GL_LOADER_H
#define HAL_GL_LOADER_H

#include <stdbool.h>
#include <stddef.h>
#include "types.h"

/* Load the GL function table. Returns false when the GL library or
 * the loader entry point is unavailable (the app must then exit —
 * no CPU fallback). Safe to call once; later calls are no-ops. */
bool hal_gl_load(void);

/* True when hal_gl_load() succeeded and the core functions the
 * renderer needs are resolved. */
bool hal_gl_ready(void);

/* The loaded GL version identity (glGetString(GL_VERSION)), or
 * "<none>" before/without a context. */
const char *hal_gl_version_string(void);

/* Which loader resolved ("glX", "egl", ...); for --gpu-info. */
const char *hal_gl_loader_backend(void);

/* ---- GL types (own declarations; no system GL headers) ---- */
typedef unsigned int hal_gl_enum;
typedef unsigned int hal_gl_uint;
typedef int hal_gl_int;
typedef int hal_gl_sizei;
typedef float hal_gl_float;
typedef unsigned char hal_gl_ubyte;

/* ---- The function table (resolved by hal_gl_load) ---- */
#define HAL_GL_FUNCS(X)                                                        \
    /* state + queries */                                                      \
    X(viewport, Viewport,void, (int, int, int, int))                                    \
    X(clear_color, ClearColor,void, (float, float, float, float))                         \
    X(clear, Clear,void, (hal_gl_uint))                                              \
    X(enable, Enable,void, (hal_gl_enum))                                             \
    X(disable, Disable,void, (hal_gl_enum))                                            \
    X(get_error, GetError,hal_gl_enum, (void))                                          \
    X(get_string, GetString,const char *, (hal_gl_enum))                                 \
    X(get_integerv, GetIntegerv,void, (hal_gl_enum, hal_gl_int *))                         \
    X(read_pixels, ReadPixels,void, (int, int, int, int, hal_gl_enum, hal_gl_enum,        \
                          void *))                                             \
    X(finish, Finish,void, (void))                                                    \
    X(scissor, Scissor,void, (int, int, int, int))                                     \
    X(depth_func, DepthFunc,void, (hal_gl_enum))                                         \
    X(depth_mask, DepthMask,void, (unsigned char))                                       \
    X(blend_func, BlendFunc,void, (hal_gl_enum, hal_gl_enum))                            \
    X(blend_equation, BlendEquation,void, (hal_gl_enum))                                     \
    X(line_width, LineWidth,void, (float))                                               \
    X(color_mask, ColorMask,void, (unsigned char, unsigned char, unsigned char,          \
                         unsigned char))                                       \
    X(polygon_mode, PolygonMode,void, (hal_gl_enum, hal_gl_enum))                          \
    /* shaders + programs */                                                   \
    X(create_shader, CreateShader,hal_gl_uint, (hal_gl_enum))                               \
    X(shader_source, ShaderSource,void, (hal_gl_uint, hal_gl_sizei,                         \
                            const char *const *, const hal_gl_int *))          \
    X(compile_shader, CompileShader,void, (hal_gl_uint))                                     \
    X(get_shaderiv, GetShaderiv,void, (hal_gl_uint, hal_gl_enum, hal_gl_int *))            \
    X(get_shader_info_log, GetShaderInfoLog,void, (hal_gl_uint, hal_gl_sizei,                   \
                                  hal_gl_sizei *, char *))                      \
    X(delete_shader, DeleteShader,void, (hal_gl_uint))                                      \
    X(create_program, CreateProgram,hal_gl_uint, (void))                                     \
    X(attach_shader, AttachShader,void, (hal_gl_uint, hal_gl_uint))                         \
    X(link_program, LinkProgram,void, (hal_gl_uint))                                       \
    X(get_programiv, GetProgramiv,void, (hal_gl_uint, hal_gl_enum, hal_gl_int *))           \
    X(get_program_info_log, GetProgramInfoLog,void, (hal_gl_uint, hal_gl_sizei,                  \
                                   hal_gl_sizei *, char *))                     \
    X(delete_program, DeleteProgram,void, (hal_gl_uint))                                     \
    X(use_program, UseProgram,void, (hal_gl_uint))                                        \
    X(get_uniform_location, GetUniformLocation,hal_gl_int, (hal_gl_uint, const char *))           \
    X(uniform_1i, Uniform1i,void, (hal_gl_int, hal_gl_int))                              \
    X(uniform_1f, Uniform1f,void, (hal_gl_int, float))                                   \
    X(uniform_2f, Uniform2f,void, (hal_gl_int, float, float))                            \
    X(uniform_3f, Uniform3f,void, (hal_gl_int, float, float, float))                      \
    X(uniform_4f, Uniform4f,void, (hal_gl_int, float, float, float, float))              \
    X(uniform_matrix_4fv, UniformMatrix4fv,void, (hal_gl_int, hal_gl_sizei,                     \
                                 unsigned char, const float *))                \
    X(uniform_1ui, Uniform1ui,void, (hal_gl_int, hal_gl_uint))                            \
    /* buffers + VAOs */                                                       \
    X(gen_vertex_arrays, GenVertexArrays,void, (hal_gl_sizei, hal_gl_uint *))                  \
    X(delete_vertex_arrays, DeleteVertexArrays,void, (hal_gl_sizei, const hal_gl_uint *))         \
    X(bind_vertex_array, BindVertexArray,void, (hal_gl_uint))                                  \
    X(gen_buffers, GenBuffers,void, (hal_gl_sizei, hal_gl_uint *))                        \
    X(delete_buffers, DeleteBuffers,void, (hal_gl_sizei, const hal_gl_uint *))               \
    X(bind_buffer, BindBuffer,void, (hal_gl_enum, hal_gl_uint))                           \
    X(buffer_data, BufferData,void, (hal_gl_enum, ptrdiff_t, const void *,                \
                          hal_gl_enum))                                        \
    X(buffer_sub_data, BufferSubData,void, (hal_gl_enum, ptrdiff_t, ptrdiff_t,               \
                              const void *))                                   \
    X(vertex_attrib_pointer, VertexAttribPointer,void, (hal_gl_uint, hal_gl_int,                   \
                                    hal_gl_enum, unsigned char,                \
                                    hal_gl_int, ptrdiff_t))                    \
    X(enable_vertex_attrib_array, EnableVertexAttribArray,void, (hal_gl_uint))                         \
    X(draw_elements, DrawElements,void, (hal_gl_enum, hal_gl_sizei, hal_gl_enum,            \
                            const void *))                                     \
    X(draw_arrays, DrawArrays,void, (hal_gl_enum, hal_gl_int, hal_gl_sizei))              \
    /* textures */                                                             \
    X(gen_textures, GenTextures,void, (hal_gl_sizei, hal_gl_uint *))                       \
    X(delete_textures, DeleteTextures,void, (hal_gl_sizei, const hal_gl_uint *))              \
    X(bind_texture, BindTexture,void, (hal_gl_enum, hal_gl_uint))                          \
    X(tex_image_2d, TexImage2D,void, (hal_gl_enum, hal_gl_int, hal_gl_int, hal_gl_sizei,  \
                          hal_gl_sizei, hal_gl_int, hal_gl_enum,               \
                          hal_gl_enum, const void *))                          \
    X(tex_sub_image_2d, TexSubImage2D,void, (hal_gl_enum, hal_gl_int, hal_gl_int,            \
                               hal_gl_int, hal_gl_sizei, hal_gl_sizei,         \
                               hal_gl_enum, hal_gl_enum, const void *))        \
    X(tex_parameteri, TexParameteri,void, (hal_gl_enum, hal_gl_enum, hal_gl_int))            \
    X(active_texture, ActiveTexture,void, (hal_gl_enum))                                     \
    X(pixel_storei, PixelStorei,void, (hal_gl_enum, hal_gl_int))                           \
    /* framebuffers (MSAA + resolve) */                                        \
    X(gen_framebuffers, GenFramebuffers,void, (hal_gl_sizei, hal_gl_uint *))                   \
    X(delete_framebuffers, DeleteFramebuffers,void, (hal_gl_sizei, const hal_gl_uint *))          \
    X(bind_framebuffer, BindFramebuffer,void, (hal_gl_enum, hal_gl_uint))                      \
    X(framebuffer_texture_2d, FramebufferTexture2D,void, (hal_gl_enum, hal_gl_enum,                 \
                                     hal_gl_enum, hal_gl_uint,                 \
                                     hal_gl_int))                              \
    X(framebuffer_renderbuffer, FramebufferRenderbuffer,void, (hal_gl_enum, hal_gl_enum,               \
                                       hal_gl_enum, hal_gl_uint))              \
    X(check_framebuffer_status, CheckFramebufferStatus,hal_gl_enum, (hal_gl_enum))                    \
    X(gen_renderbuffers, GenRenderbuffers,void, (hal_gl_sizei, hal_gl_uint *))                   \
    X(delete_renderbuffers, DeleteRenderbuffers,void, (hal_gl_sizei, const hal_gl_uint *))         \
    X(bind_renderbuffer, BindRenderbuffer,void, (hal_gl_enum, hal_gl_uint))                     \
    X(renderbuffer_storage, RenderbufferStorage,void, (hal_gl_enum, hal_gl_int,                    \
                                   hal_gl_sizei, hal_gl_sizei))                \
    X(renderbuffer_storage_multisample, RenderbufferStorageMultisample,void, (hal_gl_enum, hal_gl_sizei,      \
                                               hal_gl_int, hal_gl_sizei,      \
                                               hal_gl_sizei))                 \
    X(blit_framebuffer, BlitFramebuffer,void, (hal_gl_int, hal_gl_int, hal_gl_int,             \
                               hal_gl_int, hal_gl_int, hal_gl_int,             \
                               hal_gl_int, hal_gl_int, hal_gl_int,             \
                               hal_gl_uint))                                   \
    /* timer queries (frame timing) */                                         \
    X(gen_queries, GenQueries,void, (hal_gl_sizei, hal_gl_uint *))                        \
    X(delete_queries, DeleteQueries,void, (hal_gl_sizei, const hal_gl_uint *))               \
    X(begin_query, BeginQuery,void, (hal_gl_enum, hal_gl_uint))                           \
    X(end_query, EndQuery,void, (hal_gl_enum))                                          \
    X(get_query_object_iv, GetQueryObjectIv,void, (hal_gl_uint, hal_gl_enum,                    \
                                  hal_gl_int *))                               \
    X(get_query_object_uiv, GetQueryObjectuiv,void, (hal_gl_uint, hal_gl_enum,                   \
                                   hal_gl_uint *))                             \

/* The table: hal_gl.viewport(...) after hal_gl_load(). */
typedef struct hal_gl_table {
#define X(name, Name,ret, args) ret (*name) args;
    HAL_GL_FUNCS(X)
#undef X
} hal_gl_table;

extern hal_gl_table hal_gl;

/* ---- GL constants (values from the GL 3.3 core spec) ---- */
enum {
    HAL_GL_COLOR_BUFFER_BIT = 0x4000,
    HAL_GL_DEPTH_BUFFER_BIT = 0x0100,
    HAL_GL_STENCIL_BUFFER_BIT = 0x0400,
    HAL_GL_FALSE = 0,
    HAL_GL_TRUE = 1,
    HAL_GL_TRIANGLES = 0x0004,
    HAL_GL_LINES = 0x0001,
    HAL_GL_POINTS = 0x0000,
    HAL_GL_FLOAT = 0x1406,
    HAL_GL_UNSIGNED_BYTE = 0x1401,
    HAL_GL_UNSIGNED_SHORT = 0x1403,
    HAL_GL_UNSIGNED_INT = 0x1405,
    HAL_GL_RGBA = 0x1908,
    HAL_GL_BGRA = 0x80E1,
    HAL_GL_RGB = 0x1907,
    HAL_GL_VERTEX_SHADER = 0x8B31,
    HAL_GL_FRAGMENT_SHADER = 0x8B30,
    HAL_GL_COMPILE_STATUS = 0x8B81,
    HAL_GL_LINK_STATUS = 0x8B82,
    HAL_GL_INFO_LOG_LENGTH = 0x8B84,
    HAL_GL_ARRAY_BUFFER = 0x8892,
    HAL_GL_ELEMENT_ARRAY_BUFFER = 0x8893,
    HAL_GL_STATIC_DRAW = 0x88E4,
    HAL_GL_DYNAMIC_DRAW = 0x88E8,
    HAL_GL_TEXTURE_2D = 0x0DE1,
    HAL_GL_TEXTURE_MIN_FILTER = 0x2801,
    HAL_GL_TEXTURE_MAG_FILTER = 0x2800,
    HAL_GL_TEXTURE_WRAP_S = 0x2802,
    HAL_GL_TEXTURE_WRAP_T = 0x2803,
    HAL_GL_NEAREST = 0x2600,
    HAL_GL_LINEAR = 0x2601,
    HAL_GL_CLAMP_TO_EDGE = 0x812F,
    HAL_GL_TEXTURE0 = 0x84C0,
    HAL_GL_DEPTH_TEST = 0x0B71,
    HAL_GL_DEPTH_FUNC = 0x0B74,
    HAL_GL_LESS = 0x0201,
    HAL_GL_LEQUAL = 0x0203,
    HAL_GL_BLEND = 0x0BE2,
    HAL_GL_SCISSOR_TEST = 0x0B11,
    HAL_GL_SRC_ALPHA = 0x0302,
    HAL_GL_ONE_MINUS_SRC_ALPHA = 0x0303,
    HAL_GL_FUNC_ADD = 0x8006,
    HAL_GL_FRONT_AND_BACK = 0x0408,
    HAL_GL_FILL = 0x1B02,
    HAL_GL_LINE = 0x1B01,
    HAL_GL_CULL_FACE = 0x0B44,
    HAL_GL_BACK = 0x0405,
    HAL_GL_CURRENT_PROGRAM = 0x8B8D,
    HAL_GL_VERTEX_ARRAY_BINDING = 0x8074,
    HAL_GL_VIEWPORT = 0x0BA2,
    HAL_GL_MAX_SAMPLES = 0x8D36,
    /* framebuffers */
    HAL_GL_FRAMEBUFFER = 0x8D40,
    HAL_GL_READ_FRAMEBUFFER = 0x8CA8,
    HAL_GL_DRAW_FRAMEBUFFER = 0x8CA9,
    HAL_GL_RENDERBUFFER = 0x8D41,
    HAL_GL_COLOR_ATTACHMENT0 = 0x8CE0,
    HAL_GL_DEPTH_ATTACHMENT = 0x8D00,
    HAL_GL_FRAMEBUFFER_COMPLETE = 0x8CD5,
    HAL_GL_RGBA8 = 0x8058,
    HAL_GL_DEPTH_COMPONENT24 = 0x81A6,
    HAL_GL_DEPTH_COMPONENT = 0x1902,
    /* queries */
    HAL_GL_TIME_ELAPSED = 0x88BF,
    HAL_GL_QUERY_RESULT = 0x8866,
    HAL_GL_QUERY_RESULT_AVAILABLE = 0x8867,
    /* info */
    HAL_GL_VENDOR = 0x1F00,
    HAL_GL_RENDERER = 0x1F01,
    HAL_GL_VERSION = 0x1F02,
};

#endif /* HAL_GL_LOADER_H */
