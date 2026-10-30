/* On-device span cost breakdown: times each pixel stage over full 320x240 frames. */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../spans_neon.c"

#define W 320
#define H 240
#define REPS 8

static uint16_t col[W * H], dep[W * H], trgb[32 * 32];
static uint8_t ta[32 * 32];
static const float lanes[4] = { 0.0f, 1.0f, 2.0f, 3.0f };
static volatile uint32_t sink;
static volatile int spin_stop;
static SoftTex tex;
static SoftCombineProgram prog;
static SoftPipe pipe_;
static SoftTri tri;
static SoftTarget tgt;

static double mhz = 1200.0;

static uint64_t now(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static void grad(SoftGrad *g, float c, float dx)
{
   g->c = c; g->dx = dx; g->dy = 0.0f;
}

typedef void (*RowFn)(int y);

static void report(const char *name, RowFn fn)
{
   uint64_t best = ~0ull;
   int r, y;
   for (r = 0; r < REPS; r++)
   {
      uint64_t t0;
      memset(dep, 0xff, sizeof(dep));
      t0 = now();
      for (y = 0; y < H; y++)
         fn(y);
      t0 = now() - t0;
      if (t0 < best) best = t0;
   }
   printf("%-28s %7.1f ns/px %6.0f cyc/px\n", name, best / (double)(W * H),
         best / (double)(W * H) * mhz / 1000.0);
   fflush(stdout);
}

/* Stage kernels mirror soft_span_combine_neon's loop, cumulatively. */
static void k_empty(int y)
{
   float32x4_t k = vld1q_f32(lanes), acc = vdupq_n_f32(0);
   int x;
   for (x = 0; x < W; x += 4, k = vaddq_f32(k, vdupq_n_f32(4)))
      acc = vaddq_f32(acc, k);
   sink += (uint32_t)vgetq_lane_f32(acc, 0) + y;
}

static void k_lerp(int y)
{
   float32x4_t k = vld1q_f32(lanes);
   float r[4], g[4], b[4], a[4], f[4];
   int x;
   for (x = 0; x < W; x += 4, k = vaddq_f32(k, vdupq_n_f32(4)))
   {
      vst1q_f32(r, lerp4(tri.r.c, tri.r.dx, k));
      vst1q_f32(g, lerp4(tri.g.c, tri.g.dx, k));
      vst1q_f32(b, lerp4(tri.b.c, tri.b.dx, k));
      vst1q_f32(a, lerp4(tri.a.c, tri.a.dx, k));
      vst1q_f32(f, lerp4(tri.f.c, tri.f.dx, k));
      sink += (uint32_t)(r[x & 3] + g[1] + b[2] + a[3] + f[0]);
   }
}

static void k_uv(int y)
{
   float32x4_t k = vld1q_f32(lanes);
   int32_t si[4], ti[4];
   int x;
   for (x = 0; x < W; x += 4, k = vaddq_f32(k, vdupq_n_f32(4)))
   {
      float32x4_t rw = recip4(lerp4(tri.w.c, tri.w.dx, k));
      vst1q_s32(si, floor4(vmulq_f32(lerp4(tri.sw.c, tri.sw.dx, k), rw)));
      vst1q_s32(ti, floor4(vmulq_f32(lerp4(tri.tw.c + y, tri.tw.dx, k), rw)));
      sink += si[x & 3] + ti[1];
   }
}

static void k_depth(int y)
{
   float32x4_t k = vld1q_f32(lanes);
   uint16_t *zb = dep + y * W;
   int32_t zi[4];
   int x, j, live = 0;
   for (x = 0; x < W; x += 4, k = vaddq_f32(k, vdupq_n_f32(4)))
   {
      int32x4_t z = vcvtq_s32_f32(lerp4(tri.z.c, tri.z.dx, k));
      z = vminq_s32(vmaxq_s32(z, vdupq_n_s32(0)), vdupq_n_s32(65535));
      vst1q_s32(zi, z);
      for (j = 0; j < 4; j++)
         live += zi[j] <= zb[x + j];
   }
   sink += live;
}

static void k_fetch(int y)
{
   int x, acc = 0;
   for (x = 0; x < W; x++)
   {
      int i = wrap_texel(y, tex.h, tex.wrap_t) * tex.w + wrap_texel(x, tex.w, tex.wrap_s);
      acc += tex.rgb[i] + tex.a[i];
   }
   sink += acc;
}

static void k_store(int y)
{
   uint16_t *d = col + y * W;
   int x;
   for (x = 0; x < W; x++)
      d[x] = (uint16_t)(x + y);
}

static void k_fixed(int y) { soft_span_fixed(&tgt, &pipe_, &tri, y, 0, W); }
static void k_neon(int y)  { soft_span_combine_neon(&tgt, &pipe_, &tri, y, 0, W); }
static void k_scalar(int y) { soft_span_combine(&tgt, &pipe_, &tri, y, 0, W); }


static void run_all(const char *tag)
{
   char name[64];
   printf("-- %s\n", tag);
   report("empty loop", k_empty);
   report("lerp 5 attrs", k_lerp);
   report("1/w + uv floor", k_uv);
   report("depth test", k_depth);
   report("texel fetch (scalar)", k_fetch);
   report("plain u16 store", k_store);
   prog.mode = SC_TEX_MUL_SHADE;
   report("fixed span tex*shade", k_fixed);
   pipe_.fog = 1;
   report("fixed span tex*shade+fog", k_fixed);
   pipe_.fog = 0; pipe_.blend = SB_ALPHA;
   report("fixed span tex*shade+blend", k_fixed);
   pipe_.blend = SB_OPAQUE;
   prog.mode = SC_GENERIC;
   report("fixed span generic 1cyc", k_fixed);
   prog.cycles = 2;
   report("fixed span generic 2cyc", k_fixed);
   prog.cycles = 1;
   prog.mode = SC_TEX_MUL_SHADE;
   report("neon span tex*shade", k_neon);
   pipe_.fog = 1;
   report("neon span tex*shade+fog", k_neon);
   pipe_.fog = 0; pipe_.blend = SB_ALPHA;
   report("neon span tex*shade+blend", k_neon);
   pipe_.blend = SB_OPAQUE;
   prog.mode = SC_GENERIC;
   report("neon span generic", k_neon);
   report("scalar span generic", k_scalar);
   prog.mode = SC_TEX_MUL_SHADE;
   report("scalar span tex*shade", k_scalar);
   pipe_.z_test = 0;
   snprintf(name, sizeof(name), "neon span tex*shade no z");
   report(name, k_neon);
   pipe_.z_test = 1;
}

int main(void)
{
   pthread_t th;
   int i;
   FILE *fq = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r");

   if (fq)
   {
      long khz;
      if (fscanf(fq, "%ld", &khz) == 1)
         mhz = khz / 1000.0;
      fclose(fq);
   }
   printf("cpu %.0f MHz\n", mhz);

   for (i = 0; i < 32 * 32; i++) { trgb[i] = (uint16_t)(i * 2654435761u >> 16); ta[i] = 255; }
   tex.rgb = trgb; tex.a = ta; tex.w = 32; tex.h = 32; tex.mask_s = 31; tex.mask_t = 31;
   prog.cycles = 1; prog.uses_tex = 1; prog.uses_shade = 1;
   prog.sel[0][0][0] = TEXEL0; prog.sel[0][0][1] = ZERO; prog.sel[0][0][2] = SHADE; prog.sel[0][0][3] = ZERO;
   prog.sel[0][1][0] = TEXEL0; prog.sel[0][1][1] = ZERO; prog.sel[0][1][2] = SHADE; prog.sel[0][1][3] = ZERO;
   pipe_.tex = &tex; pipe_.prog = &prog; pipe_.z_test = 1; pipe_.z_update = 1;
   pipe_.fog_color = 0x808080;
   for (i = 0; i < 4; i++) { pipe_.prim[i] = 200; pipe_.env[i] = 100; }
   soft_target_init(&tgt, col, dep, W, H);
   grad(&tri.z, 1000.0f, 1.0f);
   grad(&tri.r, 200.0f, 0.1f); grad(&tri.g, 150.0f, 0.1f); grad(&tri.b, 100.0f, 0.1f);
   grad(&tri.a, 255.0f, 0.0f); grad(&tri.f, 10.0f, 0.3f);
   grad(&tri.w, 1.0f, 0.0001f); grad(&tri.sw, 0.0f, 0.2f); grad(&tri.tw, 0.0f, 0.0f);

   run_all("idle second core");
   (void)th;
   return 0;
}
