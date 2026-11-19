/*
 * test_platform.c — headless platform-layer tests
 *
 * Exercises the surface and text halves of the platform module with no
 * display connection: offscreen surface creation, raw pixel access,
 * damage declaration semantics (an offscreen present resets damage),
 * font discovery, measurement, and text drawing with and without a
 * clip. The window half needs a display and lives in the display tier
 * (build/halcyon --smoke under Xvfb, run by `make test-display`).
 */

#include "hal_test.h"
#include "platform/platform.h"

#include <stdlib.h>
#include <string.h>

static void test_offscreen_lifecycle(void) {
    hal_surface *s = NULL;
    HAL_TEST_CHECK_EQ_I(hal_surface_offscreen_create(64, 48, &s), HAL_OK);
    HAL_TEST_CHECK(s != NULL);

    hal_surface_info info;
    HAL_TEST_CHECK_EQ_I(hal_surface_info_get(s, &info), HAL_OK);
    HAL_TEST_CHECK_EQ_I(info.width, 64);
    HAL_TEST_CHECK_EQ_I(info.height, 48);
    HAL_TEST_CHECK(info.stride >= 64);
    HAL_TEST_CHECK(info.pixels != NULL);

    /* Out-of-range sizes refuse instead of clamping. */
    hal_surface *bad = NULL;
    HAL_TEST_CHECK_EQ_I(hal_surface_offscreen_create(0, 10, &bad),
                        HAL_ERR_INVALID_ARGUMENT);
    HAL_TEST_CHECK_EQ_I(hal_surface_offscreen_create(10, 0, &bad),
                        HAL_ERR_INVALID_ARGUMENT);
    HAL_TEST_CHECK(bad == NULL);

    hal_surface_destroy(s);
    /* NULL is a documented safe no-op. */
    hal_surface_destroy(NULL);
}

static void test_pixel_write_and_readback(void) {
    hal_surface *s = NULL;
    HAL_TEST_CHECK_EQ_I(hal_surface_offscreen_create(32, 32, &s), HAL_OK);

    hal_surface_info info;
    HAL_TEST_CHECK_EQ_I(hal_surface_info_get(s, &info), HAL_OK);

    /* Fill with a known color through the raw pointer: XRGB8888. */
    for (hal_i32 y = 0; y < info.height; y++)
        for (hal_i32 x = 0; x < info.width; x++)
            info.pixels[(size_t)y * (size_t)info.stride + (size_t)x] =
                0x123456u;

    /* Raw writes need an explicit damage declaration; then present (a
     * damage-resetting no-op offscreen). Both must succeed. */
    hal_surface_invalidate_all(s);
    HAL_TEST_CHECK_EQ_I(hal_surface_present(s), HAL_OK);

    /* Reacquire and verify the pixels survived the present cycle. */
    HAL_TEST_CHECK_EQ_I(hal_surface_info_get(s, &info), HAL_OK);
    HAL_TEST_CHECK_EQ_I(info.pixels[0] & 0xFFFFFFu, 0x123456u);
    HAL_TEST_CHECK_EQ_I(
        info.pixels[(size_t)(info.height - 1) * (size_t)info.stride +
                    (size_t)(info.width - 1)] & 0xFFFFFFu,
        0x123456u);

    /* Partial invalidation of a rect is accepted without error. */
    hal_surface_invalidate(s, hal_rect_make(2, 2, 4, 4));
    /* An empty rect is a documented no-op. */
    hal_surface_invalidate(s, hal_rect_make(0, 0, 0, 0));
    HAL_TEST_CHECK_EQ_I(hal_surface_present(s), HAL_OK);

    hal_surface_destroy(s);
}

static void test_font_and_text(void) {
    hal_font *font = hal_font_system_default(16);
    HAL_TEST_CHECK(font != NULL);
    if (!font)
        return; /* a stripped container has no font; skip the rest */

    hal_font_metrics metrics;
    hal_font_get_metrics(font, &metrics);
    HAL_TEST_CHECK(metrics.ascent > 0);
    HAL_TEST_CHECK(metrics.descent > 0);
    HAL_TEST_CHECK(metrics.line_height >= metrics.ascent + metrics.descent);

    static const char text[] = "Halcyon 3D";
    hal_text_metrics tm;
    HAL_TEST_CHECK_EQ_I(
        hal_font_measure(font, text, sizeof(text) - 1, &tm), HAL_OK);
    HAL_TEST_CHECK(tm.advance_width > 0);

    /* Measuring a prefix measures less. */
    hal_text_metrics tm2;
    HAL_TEST_CHECK_EQ_I(hal_font_measure(font, text, 3, &tm2), HAL_OK);
    HAL_TEST_CHECK(tm2.advance_width < tm.advance_width);

    /* Drawing leaves ink: draw dark text on a light surface and count
     * changed pixels. */
    hal_surface *s = NULL;
    HAL_TEST_CHECK_EQ_I(hal_surface_offscreen_create(200, 60, &s), HAL_OK);

    hal_surface_info info;
    HAL_TEST_CHECK_EQ_I(hal_surface_info_get(s, &info), HAL_OK);
    for (hal_i32 y = 0; y < info.height; y++)
        for (hal_i32 x = 0; x < info.width; x++)
            info.pixels[(size_t)y * (size_t)info.stride + (size_t)x] =
                0xF0F0F0u;
    hal_surface_invalidate_all(s);

    HAL_TEST_CHECK_EQ_I(
        hal_surface_draw_text(s, font, text, sizeof(text) - 1, 10, 30,
                              hal_color_hex(0x201030), NULL),
        HAL_OK);

    HAL_TEST_CHECK_EQ_I(hal_surface_info_get(s, &info), HAL_OK);
    long changed = 0;
    for (hal_i32 y = 0; y < info.height; y++)
        for (hal_i32 x = 0; x < info.width; x++)
            if ((info.pixels[(size_t)y * (size_t)info.stride + (size_t)x] &
                 0xFFFFFFu) != 0xF0F0F0u)
                changed++;
    HAL_TEST_CHECK(changed > 50);

    /* Clipped drawing: a clip rect that excludes the text's row must
     * leave the surface unchanged. */
    for (hal_i32 y = 0; y < info.height; y++)
        for (hal_i32 x = 0; x < info.width; x++)
            info.pixels[(size_t)y * (size_t)info.stride + (size_t)x] =
                0xF0F0F0u;
    hal_surface_invalidate_all(s);
    hal_rect clip = hal_rect_make(0, 0, 200, 5); /* above the baseline ink */
    HAL_TEST_CHECK_EQ_I(
        hal_surface_draw_text(s, font, text, sizeof(text) - 1, 10, 30,
                              hal_color_hex(0x201030), &clip),
        HAL_OK);
    HAL_TEST_CHECK_EQ_I(hal_surface_info_get(s, &info), HAL_OK);
    long changed2 = 0;
    for (hal_i32 y = 0; y < info.height; y++)
        for (hal_i32 x = 0; x < info.width; x++)
            if ((info.pixels[(size_t)y * (size_t)info.stride + (size_t)x] &
                 0xFFFFFFu) != 0xF0F0F0u)
                changed2++;
    HAL_TEST_CHECK_EQ_I(changed2, 0);

    hal_surface_destroy(s);
    hal_font_destroy(font);
    hal_font_destroy(NULL); /* safe no-op */
}

static void test_color_helpers(void) {
    /* The packing helpers are the bridge between theme files and the
     * painter; pin their rounding. */
    hal_color c = hal_color_hex(0x8B7EC8u);
    HAL_TEST_CHECK_EQ_F(c.r, 139.0 / 255.0, 1e-6);
    HAL_TEST_CHECK_EQ_F(c.g, 126.0 / 255.0, 1e-6);
    HAL_TEST_CHECK_EQ_F(c.b, 200.0 / 255.0, 1e-6);
    HAL_TEST_CHECK_EQ_F(c.a, 1.0, 1e-6);

    hal_color a = hal_rect_contains(hal_rect_make(0, 0, 10, 10), 5, 5)
                      ? hal_color_rgb8(1, 2, 3)
                      : hal_color_rgb8(0, 0, 0);
    HAL_TEST_CHECK_EQ_F(a.b, 3.0 / 255.0, 1e-6);

    hal_rect i = hal_rect_intersect(hal_rect_make(0, 0, 10, 10),
                                    hal_rect_make(5, 5, 10, 10));
    HAL_TEST_CHECK_EQ_I(i.width, 5);
    HAL_TEST_CHECK_EQ_I(i.height, 5);
    hal_rect disjoint = hal_rect_intersect(hal_rect_make(0, 0, 5, 5),
                                           hal_rect_make(10, 10, 5, 5));
    HAL_TEST_CHECK(hal_rect_empty(disjoint));
}

int main(void) {
    test_offscreen_lifecycle();
    test_pixel_write_and_readback();
    test_font_and_text();
    test_color_helpers();
    HAL_TEST_SUMMARY("test_platform");
}
