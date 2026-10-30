#include <string.h>

#include "test.h"
#include "soft_xform.h"

static void test_scale_for_wide_color_image(void)
{
   CHECK(soft_scale_shift(320) == 1);
   CHECK(soft_scale_shift(640) == 2);
   CHECK(soft_scale_shift(256) == 1);
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

int main(void)
{
   RUN(test_scale_for_wide_color_image);
   RUN(test_center_vertex_maps_to_center);
   RUN(test_behind_camera_rejected);
   TEST_EXIT();
}
