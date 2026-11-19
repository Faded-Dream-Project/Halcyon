/*
 * pick.c — ray and screen-space picking
 */

#include "pick.h"

#include <math.h>

hal_f32 hal_ray_triangle(hal_ray r, hal_vec3f v0, hal_vec3f v1,
                         hal_vec3f v2) {
    hal_vec3f e1 = hal_vec3f_sub(v1, v0);
    hal_vec3f e2 = hal_vec3f_sub(v2, v0);
    hal_vec3f h = hal_vec3f_cross(r.dir, e2);
    hal_f32 det = hal_vec3f_dot(e1, h);
    if (det > -1e-8f && det < 1e-8f)
        return -1.0f; /* parallel */

    hal_f32 inv = 1.0f / det;
    hal_vec3f s = hal_vec3f_sub(r.origin, v0);
    hal_f32 u = hal_vec3f_dot(s, h) * inv;
    if (u < 0.0f || u > 1.0f)
        return -1.0f;

    hal_vec3f q = hal_vec3f_cross(s, e1);
    hal_f32 v = hal_vec3f_dot(r.dir, q) * inv;
    if (v < 0.0f || u + v > 1.0f)
        return -1.0f;

    hal_f32 t = hal_vec3f_dot(e2, q) * inv;
    return t > 1e-5f ? t : -1.0f;
}

int hal_pick_face(hal_mesh *mesh, hal_mat4f world, hal_ray ray,
                  hal_f32 *out_t) {
    if (!mesh)
        return -1;

    int best = -1;
    hal_f32 best_t = 1e30f;
    for (size_t f = 0; f < mesh->face_count; f++) {
        size_t n = mesh->face_off[f + 1] - mesh->face_off[f];
        if (n < 3)
            continue;
        /* Fan-triangulate (Halcyon's faces are convex). */
        hal_vec3f v0 = hal_mat4f_transform_point(
            world, mesh->verts[mesh->loops[mesh->face_off[f]]]);
        for (size_t i = 1; i + 1 < n; i++) {
            hal_vec3f v1 = hal_mat4f_transform_point(
                world,
                mesh->verts[mesh->loops[mesh->face_off[f] + i]]);
            hal_vec3f v2 = hal_mat4f_transform_point(
                world,
                mesh->verts[mesh->loops[mesh->face_off[f] + i + 1]]);
            hal_f32 t = hal_ray_triangle(ray, v0, v1, v2);
            if (t > 0.0f && t < best_t) {
                best_t = t;
                best = (int)f;
            }
        }
    }
    if (out_t && best >= 0)
        *out_t = best_t;
    return best;
}

hal_pick hal_pick_scene(hal_scene *scene, hal_camera *cam,
                        hal_mat4f view_proj, hal_rectf viewport,
                        hal_f32 px, hal_f32 py) {
    hal_pick pick = { .kind = HAL_PICK_NONE, .object = 0, .index = 0,
                      .depth = 0.0f };
    if (!scene || !cam)
        return pick;

    hal_ray ray = hal_camera_ray(cam, view_proj, viewport, px, py);
    hal_f32 best_t = 1e30f;

    for (size_t i = 0; i < scene->count; i++) {
        hal_object *obj = &scene->objects[i];
        if (!obj->visible || !obj->mesh)
            continue;
        hal_mat4f world = hal_object_world_matrix(scene, obj);
        hal_f32 t = 0.0f;
        int face = hal_pick_face(obj->mesh, world, ray, &t);
        if (face >= 0 && t < best_t) {
            best_t = t;
            pick.kind = HAL_PICK_OBJECT;
            pick.object = obj->id;
            pick.index = (size_t)face;
            pick.depth = t;
        }
    }
    return pick;
}

int hal_pick_vert(hal_mesh *mesh, hal_mat4f world, hal_camera *cam,
                  hal_mat4f view_proj, hal_rectf viewport, hal_f32 px,
                  hal_f32 py, hal_f32 max_dist) {
    if (!mesh)
        return -1;
    hal_f32 best = max_dist * max_dist;
    int best_idx = -1;
    for (size_t v = 0; v < mesh->vert_count; v++) {
        hal_vec3f wp =
            hal_mat4f_transform_point(world, mesh->verts[v]);
        hal_f32 w = 0.0f;
        hal_vec3f sp = hal_camera_project(cam, view_proj, wp, viewport,
                                          &w);
        if (w <= 0.0f)
            continue; /* behind the camera */
        hal_f32 dx = sp.x - px;
        hal_f32 dy = sp.y - py;
        hal_f32 d2 = dx * dx + dy * dy;
        if (d2 < best) {
            best = d2;
            best_idx = (int)v;
        }
    }
    return best_idx;
}

int hal_pick_edge(hal_mesh *mesh, hal_mat4f world, hal_camera *cam,
                  hal_mat4f view_proj, hal_rectf viewport, hal_f32 px,
                  hal_f32 py, hal_f32 max_dist) {
    if (!mesh)
        return -1;
    hal_f32 best = max_dist * max_dist;
    int best_idx = -1;
    for (size_t e = 0; e < mesh->edge_count; e++) {
        hal_vec3f wa = hal_mat4f_transform_point(
            world, mesh->verts[mesh->edges[e * 2]]);
        hal_vec3f wb = hal_mat4f_transform_point(
            world, mesh->verts[mesh->edges[e * 2 + 1]]);
        hal_f32 wa_w = 0.0f, wb_w = 0.0f;
        hal_vec3f sa = hal_camera_project(cam, view_proj, wa, viewport,
                                          &wa_w);
        hal_vec3f sb = hal_camera_project(cam, view_proj, wb, viewport,
                                          &wb_w);
        if (wa_w <= 0.0f || wb_w <= 0.0f)
            continue;

        /* Point-to-segment distance in screen space. */
        hal_f32 ex = sb.x - sa.x;
        hal_f32 ey = sb.y - sa.y;
        hal_f32 len2 = ex * ex + ey * ey;
        hal_f32 t = 0.0f;
        if (len2 > 1e-9f) {
            t = ((px - sa.x) * ex + (py - sa.y) * ey) / len2;
            if (t < 0.0f)
                t = 0.0f;
            if (t > 1.0f)
                t = 1.0f;
        }
        hal_f32 cx = sa.x + ex * t;
        hal_f32 cy = sa.y + ey * t;
        hal_f32 dx = cx - px;
        hal_f32 dy = cy - py;
        hal_f32 d2 = dx * dx + dy * dy;
        if (d2 < best) {
            best = d2;
            best_idx = (int)e;
        }
    }
    return best_idx;
}
