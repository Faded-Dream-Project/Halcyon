/*
 * gl_renderer.c — the GPU viewport renderer (GL 3.3 core).
 *
 * Session notes (PROGRESS.md carries the record):
 *  - Meshes upload ONCE as per-loop vertex buffers + triangle/line
 *    index buffers and re-upload only when the scene serial changes
 *    (an edit). Skin weights ride the vertices; bone skin matrices
 *    (back * posed * bind * mesh_to_armature, mirroring
 *    hal_mesh_apply_pose_ex's math exactly) are uniforms — GPU
 *    skinning with the CPU path's numerics.
 *  - The scene renders into an MSAA offscreen framebuffer when the
 *    context allows (samples reported honestly), then blits into the
 *    window framebuffer; the UI layer composites after the blit.
 *  - GL state is set EXPLICITLY every frame: the context is shared
 *    with FDK's machinery, which leaves bindings behind.
 *  - Index buffers use UNSIGNED_INT — UNSIGNED_BYTE indices silently
 *    rasterized nothing on the tested llvmpipe core context.
 *  - Timer queries bracket the scene pass; results are read a frame
 *    later (AVAILABLE-gated).
 */
#include "gl_renderer.h"
#include "../platform/gl_loader.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define HAL_GL_MAX_MESHES 512
#define HAL_GL_MAX_BONES 64

/* ---- shaders ---- */

#define HAL_GL_STR2(x) #x
#define HAL_GL_STR(x) HAL_GL_STR2(x)

static const char MESH_VS[] =
    "#version 330 core\n"
    "layout(location=0) in vec3 a_pos;\n"
    "layout(location=1) in vec3 a_normal;\n"
    "layout(location=2) in vec2 a_uv;\n"
    "layout(location=3) in vec4 a_weight;\n"
    "layout(location=4) in vec4 a_bone;\n"
    "uniform mat4 u_mvp;\n"
    "uniform mat4 u_world;\n"
    "uniform int u_skinned;\n"
    "uniform mat4 u_bones[" HAL_GL_STR(HAL_GL_MAX_BONES) "];\n"
    "out vec3 v_normal;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    vec4 wp;\n"
    "    vec3 n;\n"
    "    if (u_skinned == 1) {\n"
    "        mat4 skin = mat4(0.0);\n"
    "        skin += a_weight.x * u_bones[int(a_bone.x + 0.5)];\n"
    "        skin += a_weight.y * u_bones[int(a_bone.y + 0.5)];\n"
    "        skin += a_weight.z * u_bones[int(a_bone.z + 0.5)];\n"
    "        skin += a_weight.w * u_bones[int(a_bone.w + 0.5)];\n"
    "        wp = skin * vec4(a_pos, 1.0);\n"
    "        n = mat3(skin) * a_normal;\n"
    "    } else {\n"
    "        wp = vec4(a_pos, 1.0);\n"
    "        n = a_normal;\n"
    "    }\n"
    "    v_normal = mat3(u_world) * n;\n"
    "    v_uv = a_uv;\n"
    "    gl_Position = u_mvp * wp;\n"
    "}\n";

static const char MESH_FS[] =
    "#version 330 core\n"
    "uniform vec4 u_base;\n"
    "uniform vec3 u_light_dir;\n"
    "uniform int u_textured;\n"
    "uniform int u_selected;\n"
    "uniform sampler2D u_tex;\n"
    "in vec3 v_normal;\n"
    "in vec2 v_uv;\n"
    "out vec4 o_color;\n"
    "void main() {\n"
    "    vec4 base = u_base;\n"
    "    if (u_textured == 1)\n"
    "        base = texture(u_tex, v_uv);\n"
    "    vec3 n = normalize(v_normal);\n"
    "    float diff = max(dot(n, -u_light_dir), 0.0);\n"
    "    float amb = 0.35;\n"
    "    vec3 c = base.rgb * (amb + diff * 0.65);\n"
    "    if (u_selected == 1) {\n"
    "        float rim = pow(1.0 - max(abs(n.z), 0.0), 2.0);\n"
    "        c = mix(c, vec3(0.545, 0.494, 0.784), rim * 0.55);\n"
    "    }\n"
    "    o_color = vec4(c, base.a);\n"
    "}\n";

static const char LINE_VS[] =
    "#version 330 core\n"
    "layout(location=0) in vec2 a_pos;\n"
    "layout(location=1) in vec4 a_color;\n"
    "uniform float u_point_size;\n"
    "out vec4 v_color;\n"
    "void main() {\n"
    "    v_color = a_color;\n"
    "    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "    gl_PointSize = u_point_size;\n"
    "}\n";

static const char LINE_FS[] =
    "#version 330 core\n"
    "in vec4 v_color;\n"
    "out vec4 o_color;\n"
    "void main() { o_color = v_color; }\n";

static const char UI_VS[] =
    "#version 330 core\n"
    "layout(location=0) in vec2 a_pos;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    v_uv = vec2(a_pos.x * 0.5 + 0.5, 0.5 - a_pos.y * 0.5);\n"
    "    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

static const char UI_FS[] =
    "#version 330 core\n"
    "uniform sampler2D u_tex;\n"
    "in vec2 v_uv;\n"
    "out vec4 o_color;\n"
    "void main() { o_color = texture(u_tex, v_uv); }\n";

/* The overlay vertex (defined here; gl_renderer_overlays.h uses it). */
typedef struct {
    float x, y; /* NDC */
    float r, g, b, a;
} hal_gl_ov_vert;

/* ---- per-mesh GPU cache ---- */

typedef struct hal_gl_mesh {
    const hal_mesh *mesh; /* key: identity */
    hal_u64 serial;       /* upload serial (scene serial) */
    hal_gl_uint vbo, ibo_tri, ibo_line, vao;
    hal_gl_sizei tri_count, line_count;
    hal_gl_sizei vert_count;
} hal_gl_mesh;

struct hal_gl_renderer {
    hal_gl_uint mesh_prog, line_prog, ui_prog;
    hal_gl_int u_mvp, u_world, u_skinned, u_bones, u_base, u_light,
        u_textured, u_selected, u_tex;
    hal_gl_int u_line_psize, u_ui_tex;
    hal_gl_mesh meshes[HAL_GL_MAX_MESHES];
    size_t mesh_count;
    /* overlay streaming (screen-space lines/points) */
    hal_gl_uint line_vao, line_vbo;
    hal_gl_ov_vert *ov;
    size_t ov_len, ov_cap;
    /* UI layer */
    hal_gl_uint ui_vao, ui_vbo, ui_tex;
    int ui_tex_w, ui_tex_h;
    bool ui_valid;
    /* MSAA */
    hal_gl_uint msaa_fbo, msaa_color, msaa_depth;
    int msaa_w, msaa_h, samples;
    /* timing */
    hal_gl_uint timer_q;
    bool timer_started, timer_ok;
    /* stats */
    hal_gl_stats stats;
    bool failed;
    bool inited;
};

/* The overlay batch reaches into the renderer struct + the hal_gl
 * table — include after both exist. */
#include "gl_renderer_overlays.h"

/* ---- shader helpers ---- */

static hal_gl_uint gl_compile(hal_gl_enum type, const char *src) {
    hal_gl_uint s = hal_gl.create_shader(type);
    if (!s)
        return 0;
    const char *const p[] = { src };
    hal_gl.shader_source(s, 1, p, NULL);
    hal_gl.compile_shader(s);
    hal_gl_int ok = 0;
    hal_gl.get_shaderiv(s, HAL_GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        hal_gl.get_shader_info_log(s, sizeof log, NULL, log);
        fprintf(stderr, "hal-gl: shader compile failed: %s\n", log);
        hal_gl.delete_shader(s);
        return 0;
    }
    return s;
}

static hal_gl_uint gl_link(const char *vs_src, const char *fs_src) {
    hal_gl_uint vs = gl_compile(HAL_GL_VERTEX_SHADER, vs_src);
    hal_gl_uint fs = gl_compile(HAL_GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) {
        if (vs)
            hal_gl.delete_shader(vs);
        if (fs)
            hal_gl.delete_shader(fs);
        return 0;
    }
    hal_gl_uint p = hal_gl.create_program();
    hal_gl.attach_shader(p, vs);
    hal_gl.attach_shader(p, fs);
    hal_gl.link_program(p);
    hal_gl.delete_shader(vs);
    hal_gl.delete_shader(fs);
    hal_gl_int ok = 0;
    hal_gl.get_programiv(p, HAL_GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        hal_gl.get_program_info_log(p, sizeof log, NULL, log);
        fprintf(stderr, "hal-gl: program link failed: %s\n", log);
        hal_gl.delete_program(p);
        return 0;
    }
    return p;
}

/* ---- lifecycle ---- */

hal_gl_renderer *hal_gl_renderer_create(void) {
    if (!hal_gl_ready()) {
        fprintf(stderr, "hal-gl: renderer create before loader\n");
        return NULL;
    }
    hal_gl_renderer *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    return r;
}

/* All GL object creation happens HERE — inside the frame callback,
 * where the lane context is current (there is no context at app
 * create time; the first shaders compiled against nothing). */
static bool gl_init(hal_gl_renderer *r) {
    if (r->inited)
        return !r->failed;
    r->inited = true;
    r->mesh_prog = gl_link(MESH_VS, MESH_FS);
    r->line_prog = gl_link(LINE_VS, LINE_FS);
    r->ui_prog = gl_link(UI_VS, UI_FS);
    if (!r->mesh_prog || !r->line_prog || !r->ui_prog) {
        r->failed = true;
        return r;
    }
    r->u_mvp = hal_gl.get_uniform_location(r->mesh_prog, "u_mvp");
    r->u_world = hal_gl.get_uniform_location(r->mesh_prog, "u_world");
    r->u_skinned = hal_gl.get_uniform_location(r->mesh_prog, "u_skinned");
    r->u_bones = hal_gl.get_uniform_location(r->mesh_prog, "u_bones");
    r->u_base = hal_gl.get_uniform_location(r->mesh_prog, "u_base");
    r->u_light = hal_gl.get_uniform_location(r->mesh_prog, "u_light_dir");
    r->u_textured = hal_gl.get_uniform_location(r->mesh_prog, "u_textured");
    r->u_selected = hal_gl.get_uniform_location(r->mesh_prog, "u_selected");
    r->u_tex = hal_gl.get_uniform_location(r->mesh_prog, "u_tex");
    r->u_line_psize = hal_gl.get_uniform_location(r->line_prog, "u_point_size");
    r->u_ui_tex = hal_gl.get_uniform_location(r->ui_prog, "u_tex");

    /* overlay VAO: pos2 + rgba4 interleaved */
    hal_gl.gen_vertex_arrays(1, &r->line_vao);
    hal_gl.bind_vertex_array(r->line_vao);
    hal_gl.gen_buffers(1, &r->line_vbo);
    hal_gl.bind_buffer(HAL_GL_ARRAY_BUFFER, r->line_vbo);
    hal_gl.buffer_data(HAL_GL_ARRAY_BUFFER,
                       (ptrdiff_t)(64 * 1024 * sizeof(hal_gl_ov_vert)),
                       NULL, HAL_GL_DYNAMIC_DRAW);
    hal_gl.vertex_attrib_pointer(0, 2, HAL_GL_FLOAT, HAL_GL_TRUE, 0, 0);
    hal_gl.enable_vertex_attrib_array(0);
    hal_gl.vertex_attrib_pointer(1, 4, HAL_GL_FLOAT, HAL_GL_FALSE, 0,
                                 (ptrdiff_t)(2 * sizeof(float)));
    hal_gl.enable_vertex_attrib_array(1);

    /* UI quad (NDC) */
    static const float quad[] = { -1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1 };
    hal_gl.gen_vertex_arrays(1, &r->ui_vao);
    hal_gl.bind_vertex_array(r->ui_vao);
    hal_gl.gen_buffers(1, &r->ui_vbo);
    hal_gl.bind_buffer(HAL_GL_ARRAY_BUFFER, r->ui_vbo);
    hal_gl.buffer_data(HAL_GL_ARRAY_BUFFER, sizeof quad, quad,
                       HAL_GL_STATIC_DRAW);
    hal_gl.vertex_attrib_pointer(0, 2, HAL_GL_FLOAT, HAL_GL_FALSE, 0, 0);
    hal_gl.enable_vertex_attrib_array(0);

    /* UI texture */
    hal_gl.gen_textures(1, &r->ui_tex);
    hal_gl.bind_texture(HAL_GL_TEXTURE_2D, r->ui_tex);
    hal_gl.tex_parameteri(HAL_GL_TEXTURE_2D, HAL_GL_TEXTURE_MIN_FILTER,
                          HAL_GL_NEAREST);
    hal_gl.tex_parameteri(HAL_GL_TEXTURE_2D, HAL_GL_TEXTURE_MAG_FILTER,
                          HAL_GL_NEAREST);
    hal_gl.tex_parameteri(HAL_GL_TEXTURE_2D, HAL_GL_TEXTURE_WRAP_S,
                          HAL_GL_CLAMP_TO_EDGE);
    hal_gl.tex_parameteri(HAL_GL_TEXTURE_2D, HAL_GL_TEXTURE_WRAP_T,
                          HAL_GL_CLAMP_TO_EDGE);

    /* MSAA samples */
    if (hal_gl.get_integerv && hal_gl.renderbuffer_storage_multisample) {
        hal_gl_int max = 0;
        hal_gl.get_integerv(HAL_GL_MAX_SAMPLES, &max);
        r->samples = max > 4 ? 4 : max;
    }
    r->timer_ok = hal_gl.gen_queries && hal_gl.begin_query &&
                  hal_gl.end_query && hal_gl.get_query_object_uiv;
    if (r->timer_ok)
        hal_gl.gen_queries(1, &r->timer_q);

    r->stats.samples = r->samples;
    r->stats.gpu_timer_ok = r->timer_ok;
    if (r->failed) {
        fprintf(stderr,
                "hal-gl: the GL 3.3 core programs failed to build — "
                "the GPU viewport cannot run\n");
    }
    return !r->failed;
}

static void free_mesh(hal_gl_renderer *r, hal_gl_mesh *e) {
    (void)r;
    if (e->vao)
        hal_gl.delete_vertex_arrays(1, &e->vao);
    if (e->vbo)
        hal_gl.delete_buffers(1, &e->vbo);
    if (e->ibo_tri)
        hal_gl.delete_buffers(1, &e->ibo_tri);
    if (e->ibo_line)
        hal_gl.delete_buffers(1, &e->ibo_line);
    memset(e, 0, sizeof(*e));
}

void hal_gl_renderer_drop_all(hal_gl_renderer *r) {
    if (!r)
        return;
    for (size_t i = 0; i < r->mesh_count; i++)
        free_mesh(r, &r->meshes[i]);
    r->mesh_count = 0;
}

void hal_gl_renderer_destroy(hal_gl_renderer *r) {
    if (!r)
        return;
    hal_gl_renderer_drop_all(r);
    if (r->mesh_prog)
        hal_gl.delete_program(r->mesh_prog);
    if (r->line_prog)
        hal_gl.delete_program(r->line_prog);
    if (r->ui_prog)
        hal_gl.delete_program(r->ui_prog);
    if (r->line_vao)
        hal_gl.delete_vertex_arrays(1, &r->line_vao);
    if (r->line_vbo)
        hal_gl.delete_buffers(1, &r->line_vbo);
    if (r->ui_vao)
        hal_gl.delete_vertex_arrays(1, &r->ui_vao);
    if (r->ui_vbo)
        hal_gl.delete_buffers(1, &r->ui_vbo);
    if (r->ui_tex)
        hal_gl.delete_textures(1, &r->ui_tex);
    if (r->msaa_fbo)
        hal_gl.delete_framebuffers(1, &r->msaa_fbo);
    if (r->msaa_color)
        hal_gl.delete_renderbuffers(1, &r->msaa_color);
    if (r->msaa_depth)
        hal_gl.delete_renderbuffers(1, &r->msaa_depth);
    if (r->timer_q)
        hal_gl.delete_queries(1, &r->timer_q);
    free(r->ov);
    free(r);
}

const hal_gl_stats *hal_gl_renderer_stats(const hal_gl_renderer *r) {
    return r ? &r->stats : NULL;
}

bool hal_gl_renderer_failed(const hal_gl_renderer *r) {
    return r ? r->failed : true;
}

void hal_gl_renderer_gc(hal_gl_renderer *r, hal_scene *scene) {
    if (!r || !scene)
        return;
    size_t out = 0;
    for (size_t i = 0; i < r->mesh_count; i++) {
        bool live = false;
        for (size_t o = 0; o < scene->count; o++)
            if (scene->objects[o].mesh == r->meshes[i].mesh) {
                live = true;
                break;
            }
        if (live)
            r->meshes[out++] = r->meshes[i];
        else
            free_mesh(r, &r->meshes[i]);
    }
    r->mesh_count = out;
    r->stats.meshes = out;
}

/* ---- mesh upload (per-loop vertices) ---- */

#define HAL_GL_VF 16 /* floats per loop-vertex */

static bool upload_mesh(hal_gl_renderer *r, hal_gl_mesh *e,
                        hal_mesh *m) {
    (void)r;
    /* Warm the normal caches the same way the CPU path does. */
    const hal_vec3f *normals = hal_mesh_face_normals(m);
    const hal_vec3f *vn = hal_mesh_vert_normals(m);

    const size_t lc = m->loop_count;
    const bool has_uv = m->uvs && m->uvs_valid;
    if (!lc) {
        e->tri_count = 0;
        e->line_count = 0;
        return true;
    }
    float *verts = malloc(lc * HAL_GL_VF * sizeof(float));
    hal_u32 *first_loop_of_vert =
        malloc((m->vert_count ? m->vert_count : 1) * sizeof(hal_u32));
    if (!verts || !first_loop_of_vert) {
        free(verts);
        free(first_loop_of_vert);
        return false;
    }
    for (size_t v = 0; v < m->vert_count; v++)
        first_loop_of_vert[v] = 0xFFFFFFFFu;
    for (size_t l = 0; l < lc; l++) {
        hal_u32 vi = m->loops[l];
        if (vi < m->vert_count && first_loop_of_vert[vi] == 0xFFFFFFFFu)
            first_loop_of_vert[vi] = (hal_u32)l;
        float *dst = verts + l * HAL_GL_VF;
        dst[0] = m->verts[vi].x;
        dst[1] = m->verts[vi].y;
        dst[2] = m->verts[vi].z;
        /* Per-loop normal: smooth (vertex) normals when computed,
         * else the face normal of this loop's face. */
        {
            const hal_vec3f *n = vn ? &vn[vi] : NULL;
            if (!n) {
                /* find the face owning this loop */
                for (size_t f = 0; f < m->face_count && !n; f++) {
                    if (l >= m->face_off[f] && l < m->face_off[f + 1])
                        n = &normals[f];
                }
            }
            if (!n)
                n = &normals[0];
            dst[3] = n->x;
            dst[4] = n->y;
            dst[5] = n->z;
        }
        if (has_uv) {
            dst[6] = m->uvs[l].x;
            dst[7] = m->uvs[l].y;
        } else {
            dst[6] = 0;
            dst[7] = 0;
        }
        for (int k = 0; k < 4; k++) {
            float w = 0.0f, b = 0.0f;
            if (m->skin_weights && m->skin_bones) {
                hal_u32 si = vi * 4u + (hal_u32)k;
                hal_i32 bi = m->skin_bones[si];
                w = m->skin_weights[si];
                b = bi > 0 ? (float)bi : 0.0f;
            }
            dst[8 + k] = w;
            dst[12 + k] = b;
        }
    }

    /* fan-triangulate each face over its loops */
    size_t tri_n = 0;
    for (size_t f = 0; f < m->face_count; f++) {
        size_t n = m->face_off[f + 1] - m->face_off[f];
        if (n >= 3)
            tri_n += n - 2;
    }
    hal_u32 *tris = malloc((tri_n ? tri_n : 1) * 3 * sizeof(hal_u32));
    if (!tris) {
        free(verts);
        free(first_loop_of_vert);
        return false;
    }
    size_t t = 0;
    for (size_t f = 0; f < m->face_count; f++) {
        size_t b0 = m->face_off[f], e0 = m->face_off[f + 1];
        for (size_t k = b0 + 1; k + 1 < e0; k++) {
            tris[t * 3 + 0] = (hal_u32)m->loops[b0];
            tris[t * 3 + 1] = (hal_u32)m->loops[k];
            tris[t * 3 + 2] = (hal_u32)m->loops[k + 1];
            t++;
        }
    }

    /* edge lines mapped through first_loop_of_vert */
    hal_u32 *lines = malloc((m->edge_count ? m->edge_count : 1) * 2 *
                            sizeof(hal_u32));
    if (!lines) {
        free(verts);
        free(first_loop_of_vert);
        free(tris);
        return false;
    }
    size_t ln = 0;
    for (size_t k = 0; k < m->edge_count; k++) {
        hal_u32 a = m->edges[k * 2], b = m->edges[k * 2 + 1];
        if (a >= m->vert_count || b >= m->vert_count)
            continue;
        hal_u32 la = first_loop_of_vert[a], lb = first_loop_of_vert[b];
        if (la == 0xFFFFFFFFu || lb == 0xFFFFFFFFu)
            continue;
        lines[ln * 2 + 0] = la;
        lines[ln * 2 + 1] = lb;
        ln++;
    }

    if (!e->vao) {
        hal_gl.gen_vertex_arrays(1, &e->vao);
        hal_gl.gen_buffers(1, &e->vbo);
        hal_gl.gen_buffers(1, &e->ibo_tri);
        hal_gl.gen_buffers(1, &e->ibo_line);
    }
    hal_gl.bind_vertex_array(e->vao);
    hal_gl.bind_buffer(HAL_GL_ARRAY_BUFFER, e->vbo);
    hal_gl.buffer_data(HAL_GL_ARRAY_BUFFER,
                       (ptrdiff_t)(lc * HAL_GL_VF * sizeof(float)), verts,
                       HAL_GL_STATIC_DRAW);
    static const int strides[5] = { 3, 3, 2, 4, 4 };
    size_t off = 0;
    for (int loc = 0; loc < 5; loc++) {
        hal_gl.vertex_attrib_pointer((hal_gl_uint)loc, strides[loc],
                                     HAL_GL_FLOAT, HAL_GL_TRUE, 0,
                                     (ptrdiff_t)(off * sizeof(float)));
        hal_gl.enable_vertex_attrib_array((hal_gl_uint)loc);
        off += (size_t)strides[loc];
    }
    hal_gl.bind_buffer(HAL_GL_ELEMENT_ARRAY_BUFFER, e->ibo_tri);
    hal_gl.buffer_data(HAL_GL_ELEMENT_ARRAY_BUFFER,
                       (ptrdiff_t)(tri_n * 3 * sizeof(hal_u32)), tris,
                       HAL_GL_STATIC_DRAW);
    hal_gl.bind_buffer(HAL_GL_ELEMENT_ARRAY_BUFFER, e->ibo_line);
    hal_gl.buffer_data(HAL_GL_ELEMENT_ARRAY_BUFFER,
                       (ptrdiff_t)(ln * 2 * sizeof(hal_u32)), lines,
                       HAL_GL_STATIC_DRAW);
    e->tri_count = (hal_gl_sizei)(tri_n * 3);
    e->line_count = (hal_gl_sizei)(ln * 2);
    e->vert_count = (hal_gl_sizei)lc;
    free(verts);
    free(first_loop_of_vert);
    free(tris);
    free(lines);
    return true;
}

static hal_gl_mesh *mesh_entry(hal_gl_renderer *r, hal_mesh *m,
                               hal_u64 serial) {
    for (size_t i = 0; i < r->mesh_count; i++)
        if (r->meshes[i].mesh == m) {
            if (r->meshes[i].serial != serial) {
                if (!upload_mesh(r, &r->meshes[i], m))
                    return NULL;
                r->meshes[i].serial = serial;
            }
            return &r->meshes[i];
        }
    if (r->mesh_count >= HAL_GL_MAX_MESHES)
        return NULL;
    hal_gl_mesh *e = &r->meshes[r->mesh_count++];
    memset(e, 0, sizeof(*e));
    e->mesh = m;
    e->serial = serial;
    if (!upload_mesh(r, e, m))
        return NULL;
    return e;
}

/* ---- MSAA ---- */

static bool ensure_msaa(hal_gl_renderer *r, int w, int h) {
    if (r->samples == 0)
        return false;
    if (r->msaa_fbo && r->msaa_w == w && r->msaa_h == h)
        return true;
    if (!hal_gl.gen_framebuffers || !hal_gl.gen_renderbuffers)
        return false;
    if (r->msaa_fbo) {
        hal_gl.delete_framebuffers(1, &r->msaa_fbo);
        hal_gl.delete_renderbuffers(1, &r->msaa_color);
        hal_gl.delete_renderbuffers(1, &r->msaa_depth);
        r->msaa_fbo = r->msaa_color = r->msaa_depth = 0;
    }
    hal_gl.gen_framebuffers(1, &r->msaa_fbo);
    hal_gl.gen_renderbuffers(1, &r->msaa_color);
    hal_gl.gen_renderbuffers(1, &r->msaa_depth);
    hal_gl.bind_renderbuffer(HAL_GL_RENDERBUFFER, r->msaa_color);
    hal_gl.renderbuffer_storage_multisample(
        HAL_GL_RENDERBUFFER, (hal_gl_sizei)r->samples, HAL_GL_RGBA8,
        (hal_gl_sizei)w, (hal_gl_sizei)h);
    hal_gl.bind_renderbuffer(HAL_GL_RENDERBUFFER, r->msaa_depth);
    hal_gl.renderbuffer_storage_multisample(
        HAL_GL_RENDERBUFFER, (hal_gl_sizei)r->samples,
        HAL_GL_DEPTH_COMPONENT24, (hal_gl_sizei)w, (hal_gl_sizei)h);
    hal_gl.bind_framebuffer(HAL_GL_FRAMEBUFFER, r->msaa_fbo);
    hal_gl.framebuffer_renderbuffer(HAL_GL_FRAMEBUFFER,
                                    HAL_GL_COLOR_ATTACHMENT0,
                                    HAL_GL_RENDERBUFFER, r->msaa_color);
    hal_gl.framebuffer_renderbuffer(HAL_GL_FRAMEBUFFER,
                                    HAL_GL_DEPTH_ATTACHMENT,
                                    HAL_GL_RENDERBUFFER, r->msaa_depth);
    hal_gl_enum st = hal_gl.check_framebuffer_status(HAL_GL_FRAMEBUFFER);
    hal_gl.bind_framebuffer(HAL_GL_FRAMEBUFFER, 0);
    if (st != HAL_GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr,
                "hal-gl: MSAA framebuffer incomplete (0x%x) — "
                "rendering without MSAA\n",
                (unsigned)st);
        r->samples = 0;
        r->stats.samples = 0;
        hal_gl.delete_framebuffers(1, &r->msaa_fbo);
        hal_gl.delete_renderbuffers(1, &r->msaa_color);
        hal_gl.delete_renderbuffers(1, &r->msaa_depth);
        r->msaa_fbo = r->msaa_color = r->msaa_depth = 0;
        return false;
    }
    r->msaa_w = w;
    r->msaa_h = h;
    return true;
}

/* ---- skin matrices (hal_mesh_apply_pose_ex's math, per bone) ---- */

static int skin_bones_for(hal_gl_renderer *r, hal_scene *scene,
                          hal_object *obj, float *out) {
    (void)r;
    if (obj->skin_armature == HAL_OBJECT_ID_NONE)
        return 0;
    hal_object *arm_obj = hal_scene_object(scene, obj->skin_armature);
    if (!arm_obj || !arm_obj->armature ||
        arm_obj->armature->count == 0 ||
        arm_obj->armature->count > HAL_GL_MAX_BONES)
        return 0;
    hal_armature *arm = arm_obj->armature;
    hal_bone_pose *posed = malloc(arm->count * sizeof(*posed));
    hal_bone_pose *rest = malloc(arm->count * sizeof(*rest));
    if (!posed || !rest) {
        free(posed);
        free(rest);
        return 0;
    }
    hal_armature_pose(arm, posed);
    hal_armature_rest_pose(arm, rest);
    hal_mat4f mesh_to_arm = hal_mat4f_mul(
        hal_mat4f_invert(hal_object_world_matrix(scene, arm_obj)),
        hal_object_world_matrix(scene, obj));
    hal_mat4f back = hal_mat4f_invert(mesh_to_arm);
    for (size_t k = 0; k < arm->count; k++) {
        hal_mat4f bind = hal_mat4f_invert(rest[k].matrix);
        hal_mat4f s = hal_mat4f_mul(back, posed[k].matrix);
        s = hal_mat4f_mul(s, bind);
        s = hal_mat4f_mul(s, mesh_to_arm);
        memcpy(out + k * 16, s.m, sizeof(s.m));
    }
    free(posed);
    free(rest);
    return (int)arm->count;
}

/* ---- the frame ---- */

void hal_gl_render_view(hal_gl_renderer *r, hal_scene *scene,
                        hal_camera *cam, hal_rectf viewport, hal_f32 scale,
                        int fb_width, int fb_height,
                        const hal_render_opts *opts, hal_u64 scene_serial,
                        const hal_gl_seg2 *segs, size_t seg_count,
                        const hal_gl_pt2 *pts, size_t pt_count) {
    (void)fb_width;
    if (!r || !scene || !cam || !opts)
        return;
    if (!gl_init(r) || r->failed)
        return;

    /* Drain stale errors so this frame's failures are visible. */
    while (hal_gl.get_error() != 0) {
    }

    const int px = (int)(viewport.x * scale);
    const int py = (int)(viewport.y * scale);
    const int pw = (int)(viewport.width * scale);
    const int ph = (int)(viewport.height * scale);
    if (pw <= 0 || ph <= 0)
        return;

    hal_mat4f view = hal_camera_view(cam);
    hal_mat4f proj = hal_camera_projection(cam, (hal_f32)pw / (hal_f32)ph);
    hal_mat4f vpm = hal_mat4f_mul(proj, view);

    /* timer: read the previous frame's result, start this one */
    if (r->timer_ok) {
        if (r->timer_started) {
            hal_gl_uint avail = 0;
            hal_gl.get_query_object_uiv(r->timer_q,
                                        HAL_GL_QUERY_RESULT_AVAILABLE,
                                        &avail);
            if (avail) {
                hal_gl_uint ns = 0;
                hal_gl.get_query_object_uiv(r->timer_q,
                                            HAL_GL_QUERY_RESULT, &ns);
                r->stats.gpu_ns = ns;
            }
        }
        hal_gl.begin_query(HAL_GL_TIME_ELAPSED, r->timer_q);
        r->timer_started = true;
    }

    const bool msaa = ensure_msaa(r, pw, ph);
    if (msaa) {
        hal_gl.bind_framebuffer(HAL_GL_FRAMEBUFFER, r->msaa_fbo);
    } else {
        hal_gl.bind_framebuffer(HAL_GL_FRAMEBUFFER, 0);
        /* Direct render into the window framebuffer: place the GL
         * viewport at the rect's bottom-up position. */
        hal_gl.viewport(px, fb_height - (py + ph), (hal_gl_sizei)pw,
                        (hal_gl_sizei)ph);
        hal_gl.scissor(px, fb_height - (py + ph), (hal_gl_sizei)pw,
                       (hal_gl_sizei)ph);
    }

    /* Full explicit state each frame (the context is shared). */
    hal_gl.disable(HAL_GL_BLEND);
    hal_gl.enable(HAL_GL_CULL_FACE);
    hal_gl.enable(HAL_GL_DEPTH_TEST);
    hal_gl.depth_func(HAL_GL_LESS);
    hal_gl.depth_mask(HAL_GL_TRUE);
    if (msaa)
        hal_gl.viewport(0, 0, (hal_gl_sizei)pw, (hal_gl_sizei)ph);
    /* The clear is SCISSORED to the viewport rect (the placed GL
     * viewport in the direct path, 0,0 in the MSAA FBO). */
    hal_gl.enable(HAL_GL_SCISSOR_TEST);
    if (msaa)
        hal_gl.scissor(0, 0, (hal_gl_sizei)pw, (hal_gl_sizei)ph);
    hal_gl.clear_color(opts->bg.r, opts->bg.g, opts->bg.b, 1.0f);
    hal_gl.clear(HAL_GL_COLOR_BUFFER_BIT | HAL_GL_DEPTH_BUFFER_BIT);
    hal_gl.disable(HAL_GL_SCISSOR_TEST);

    if (opts->draw_grid)
        hal_gl_grid_world(r, vpm, opts->grid);

    /* meshes */
    hal_gl.use_program(r->mesh_prog);
    hal_gl.uniform_3f(r->u_light, -0.45f, -0.75f, -0.5f);
    hal_gl.uniform_1i(r->u_tex, 0);
    hal_gl.uniform_1i(r->u_textured, 0);
    hal_gl.active_texture(HAL_GL_TEXTURE0);

    hal_u32 tris = 0;
    hal_object_id sel = hal_scene_selected(scene);
    float bones[HAL_GL_MAX_BONES * 16];
    for (size_t i = 0; i < scene->count; i++) {
        hal_object *obj = &scene->objects[i];
        if (!obj->mesh || !obj->visible)
            continue;
        hal_gl_mesh *e = mesh_entry(r, obj->mesh, scene_serial);
        if (!e || !e->tri_count)
            continue;

        hal_mat4f world = hal_object_world_matrix(scene, obj);
        hal_mat4f mvp = hal_mat4f_mul(vpm, world);
        hal_gl.uniform_matrix_4fv(r->u_mvp, 1, 0, mvp.m);
        hal_gl.uniform_matrix_4fv(r->u_world, 1, 0, world.m);

        int bone_n = 0;
        if (obj->mesh->skin_weights && obj->mesh->skin_bones)
            bone_n = skin_bones_for(r, scene, obj, bones);
        hal_gl.uniform_1i(r->u_skinned, bone_n > 0 ? 1 : 0);
        if (bone_n > 0)
            hal_gl.uniform_matrix_4fv(
                r->u_bones, (hal_gl_sizei)HAL_GL_MAX_BONES, 0, bones);

        hal_color base = opts->object;
        hal_material *mat = obj->material >= 0
                                ? hal_scene_material(scene, obj->material)
                                : NULL;
        if (mat)
            base = mat->base_color;
        hal_gl.uniform_4f(r->u_base, base.r, base.g, base.b, 1.0f);
        hal_gl.uniform_1i(r->u_selected,
                          (hal_object_id)i == sel ? 1 : 0);

        hal_gl.bind_vertex_array(e->vao);
        hal_gl.bind_buffer(HAL_GL_ELEMENT_ARRAY_BUFFER, e->ibo_tri);
        hal_gl.draw_elements(HAL_GL_TRIANGLES, e->tri_count,
                             HAL_GL_UNSIGNED_INT, NULL);
        tris += (hal_u32)(e->tri_count / 3);
    }
    r->stats.tris_drawn = tris;

    /* wireframe: every object's edge lines over the fill */
    if (opts->shading == HAL_SHADING_WIREFRAME) {
        hal_gl.use_program(r->line_prog);
        /* the line program is NDC-space; wireframe needs world->NDC:
         * reuse the MESH program's path by drawing lines with it is
         * not possible (it has no flat color mode), so build the
         * edge lines as projected segments through the shell's
         * overlay batch instead — see the segs pass below. */
    }

    /* resolve MSAA into the window framebuffer. GL Y is bottom-up:
     * the viewport rect's window-space y (top-down) flips. */
    if (r->timer_ok)
        hal_gl.end_query(HAL_GL_TIME_ELAPSED);
    if (msaa) {
        const int dst_y0 = fb_height - (py + ph);
        const int dst_y1 = fb_height - py;
        hal_gl.bind_framebuffer(HAL_GL_READ_FRAMEBUFFER, r->msaa_fbo);
        hal_gl.bind_framebuffer(HAL_GL_DRAW_FRAMEBUFFER, 0);
        hal_gl.blit_framebuffer(0, 0, pw, ph, px, dst_y0, px + pw,
                                dst_y1,
                                HAL_GL_COLOR_BUFFER_BIT |
                                    HAL_GL_DEPTH_BUFFER_BIT,
                                HAL_GL_NEAREST);
        hal_gl.bind_framebuffer(HAL_GL_FRAMEBUFFER, 0);
    }

    /* Screen-space overlays (depth off): armature sticks, edit
     * selection, gizmo, axis widget — the shell's projected
     * geometry. Wireframe mode arrives here as every object's
     * projected edges. */
    hal_gl.disable(HAL_GL_DEPTH_TEST);
    hal_gl.use_program(r->line_prog);
    ov_reset(r);
    for (size_t i = 0; i < seg_count; i++) {
        hal_gl_ov_vert a = ov_from_screen(
            (hal_vec2f){ segs[i].x0, segs[i].y0 }, viewport,
            segs[i].color);
        hal_gl_ov_vert b = ov_from_screen(
            (hal_vec2f){ segs[i].x1, segs[i].y1 }, viewport,
            segs[i].color);
        ov_push(r, a);
        ov_push(r, b);
    }
    ov_draw(r, HAL_GL_LINES);
    for (size_t i = 0; i < pt_count; i++) {
        hal_gl_ov_vert p = ov_from_screen((hal_vec2f){ pts[i].x, pts[i].y },
                                          viewport, pts[i].color);
        ov_push(r, p);
    }
    /* points need their sizes; group by rounded size (the shell
     * uses few distinct sizes) */
    hal_gl.uniform_1f(r->u_line_psize, 5.0f * scale);
    ov_draw(r, HAL_GL_POINTS);
    hal_gl.uniform_1f(r->u_line_psize, 1.0f);
    hal_gl.enable(HAL_GL_DEPTH_TEST);
}

void hal_gl_composite_ui(hal_gl_renderer *r, const hal_u32 *pixels,
                         int width, int height, int stride,
                         bool content_dirty) {
    if (!r || !pixels || width <= 0 || height <= 0)
        return;
    if (!gl_init(r) || r->failed)
        return;

    while (hal_gl.get_error() != 0) {
    }

    hal_gl.bind_framebuffer(HAL_GL_FRAMEBUFFER, 0);
    hal_gl.disable(HAL_GL_DEPTH_TEST);
    hal_gl.depth_mask(HAL_GL_FALSE);
    hal_gl.disable(HAL_GL_CULL_FACE);
    hal_gl.disable(HAL_GL_SCISSOR_TEST);
    hal_gl.enable(HAL_GL_BLEND);
    hal_gl.blend_func(HAL_GL_SRC_ALPHA, HAL_GL_ONE_MINUS_SRC_ALPHA);
    hal_gl.viewport(0, 0, (hal_gl_sizei)width, (hal_gl_sizei)height);

    if (content_dirty || !r->ui_valid || r->ui_tex_w != width ||
        r->ui_tex_h != height) {
        hal_gl.bind_texture(HAL_GL_TEXTURE_2D, r->ui_tex);
        if (hal_gl.pixel_storei && stride != width)
            hal_gl.pixel_storei(0x0CF2 /* GL_UNPACK_ROW_LENGTH */,
                                stride);
        hal_gl.tex_image_2d(HAL_GL_TEXTURE_2D, 0, HAL_GL_RGBA,
                            (hal_gl_sizei)width, (hal_gl_sizei)height, 0,
                            HAL_GL_BGRA, HAL_GL_UNSIGNED_BYTE, pixels);
        if (hal_gl.pixel_storei && stride != width)
            hal_gl.pixel_storei(0x0CF2, 0);
        r->ui_tex_w = width;
        r->ui_tex_h = height;
        r->ui_valid = true;
    } else {
        hal_gl.bind_texture(HAL_GL_TEXTURE_2D, r->ui_tex);
    }

    hal_gl.use_program(r->ui_prog);
    hal_gl.uniform_1i(r->u_ui_tex, 0);
    hal_gl.active_texture(HAL_GL_TEXTURE0);
    hal_gl.bind_vertex_array(r->ui_vao);
    hal_gl.draw_arrays(HAL_GL_TRIANGLES, 0, 6);

    hal_gl.depth_mask(HAL_GL_TRUE);
    hal_gl.disable(HAL_GL_BLEND);
}

bool hal_gl_read_pixel(int x, int y, hal_u8 out_rgba[4]) {
    if (!hal_gl_ready() || !out_rgba)
        return false;
    hal_u8 px[4] = { 0, 0, 0, 0 };
    hal_gl.read_pixels(x, y, 1, 1, HAL_GL_RGBA, HAL_GL_UNSIGNED_BYTE, px);
    out_rgba[0] = px[0];
    out_rgba[1] = px[1];
    out_rgba[2] = px[2];
    out_rgba[3] = px[3];
    return true;
}
