#include <string.h>

#include "test.h"
#include "soft_xform.h"

static void test_scale_for_wide_color_image(void)
{
   CHECK(soft_scale_shift(320) == 0);
   CHECK(soft_scale_shift(640) == 1);
   CHECK(soft_scale_shift(256) == 0);
}

static void test_center_vertex_maps_to_center(void)
{
   struct SPVertex in;
   SoftViewport vp = { 160.0f, -120.0f, 160.0f, 120.0f, 0.0f, 0.0f };
   SoftVert out;

   memset(&in, 0, sizeof(in));
   in.w = 1.0f;
   CHECK(soft_vertex_to_screen(&in, &vp, 1, &out));
   CHECK(out.x == 1280);
   CHECK(out.y == 960);
}

static void test_behind_camera_rejected(void)
{
   struct SPVertex in;
   SoftViewport vp = { 160.0f, -120.0f, 160.0f, 120.0f, 0.0f, 0.0f };
   SoftVert out;

   memset(&in, 0, sizeof(in));
   in.w = -1.0f;
   CHECK(!soft_vertex_to_screen(&in, &vp, 1, &out));
}

static struct SPVertex clipv(float x, float y, float z, float w, float s)
{
   struct SPVertex v;
   memset(&v, 0, sizeof(v));
   v.x = x; v.y = y; v.z = z; v.w = w; v.s = s;
   return v;
}

static void test_near_clip_keeps_visible_part(void)
{
   /* One vertex behind the eye: clipping yields a quad, not a dropped triangle. */
   struct SPVertex in[3], out[SOFT_CLIP_MAX];
   int n, i;
   in[0] = clipv(0.0f, 0.0f, 0.5f, 1.0f, 0.0f);
   in[1] = clipv(0.5f, 0.0f, 0.5f, 1.0f, 8.0f);
   in[2] = clipv(0.0f, -0.5f, -3.0f, -1.0f, 16.0f);
   n = soft_clip_near(in, out);
   CHECK(n == 4);
   for (i = 0; i < n; i++)
      CHECK(out[i].z + out[i].w >= -1e-4f);
}

static void test_near_clip_passes_and_rejects(void)
{
   struct SPVertex in[3], out[SOFT_CLIP_MAX];
   in[0] = clipv(0, 0, 0.5f, 1, 0); in[1] = clipv(1, 0, 0.5f, 1, 0); in[2] = clipv(0, 1, 0.5f, 1, 0);
   CHECK(soft_clip_near(in, out) == 3);
   in[0].z = in[1].z = in[2].z = -2.0f;
   CHECK(soft_clip_near(in, out) == 0);
}

int main(void)
{
   RUN(test_near_clip_keeps_visible_part);
   RUN(test_near_clip_passes_and_rejects);
   RUN(test_scale_for_wide_color_image);
   RUN(test_center_vertex_maps_to_center);
   RUN(test_behind_camera_rejected);
   TEST_EXIT();
}
