#ifndef SOFT_COMBINE_H
#define SOFT_COMBINE_H

#include <stdint.h>

#include "../Combiner.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
   SC_SHADE,
   SC_TEX,
   SC_TEX_MUL_SHADE,
   SC_TEX_MUL_PRIM,
   SC_TEX_MUL_ENV,
   SC_LERP_ENV_PRIM,
   SC_GENERIC
} SoftCombineMode;

typedef enum
{
   SB_OPAQUE,
   SB_ALPHA,           /* src * a + dst * (1 - a) */
   SB_FOG,             /* fog-tagged interpolated blend, same maths as SB_ALPHA */
   SB_ADD_FOG_ALPHA,   /* src * a + dst */
   SB_KEEP             /* dst unchanged */
} SoftBlendMode;

/* Selectors per cycle for colour [0..2] and alpha [3]: A, B, C, D of (A - B) * C + D. */
typedef struct
{
   uint8_t sel[2][2][4];   /* [cycle][0 = colour, 1 = alpha][A, B, C, D] */
   int cycles;
   SoftCombineMode mode;
   int uses_tex, uses_shade;
} SoftCombineProgram;

/* Per-pixel inputs; colours are R, G, B, A bytes. */
typedef struct
{
   const SoftCombineProgram *prog;
   uint8_t tex0[4], tex1[4], shade[4], prim[4], env[4];
   uint8_t prim_lod;
} SoftCombineInputs;

typedef struct
{
   int enabled;
   uint8_t ref;      /* keep alpha >= ref */
} SoftAlphaTest;

typedef void (*SoftLogFn)(const char *msg);

/* Routes the once-per-mux fallback messages; NULL silences them. */
void soft_combine_set_logger(SoftLogFn fn);

/* Unsupported inputs fall back to texture times shade, logged once per mux. */
SoftCombineMode soft_combine_classify(const DecodedMux *m);

/* Compiles a decoded mux; two_cycle selects whether cycle 2 runs. */
void soft_combine_compile(const DecodedMux *m, int two_cycle, SoftCombineProgram *out);

/* Evaluates the program; returns R | G << 8 | B << 16 | A << 24. */
uint32_t soft_combine_generic(const SoftCombineInputs *in);

/* Same packing as soft_combine_generic, using the classified fast path. */
uint32_t soft_combine_eval(const SoftCombineInputs *in);

/* Unknown blender words return SB_ALPHA (the GL default), logged once. */
SoftBlendMode soft_blend_classify(uint32_t other_mode_l, int force_blend,
      int alpha_cvg_sel, int cycle_type);

/* Matches the GL shader: greater keeps alpha >= ref, otherwise alpha > ref. */
SoftAlphaTest soft_alpha_test_setup(int enabled, int greater, int cvg_x_alpha, float blend_alpha);

static inline int soft_alpha_pass(const SoftAlphaTest *t, uint8_t alpha)
{
   return !t->enabled || alpha >= t->ref;
}

/* Mixes RGB toward fog_color by factor / 255; alpha is kept. */
uint32_t soft_fog_apply(uint32_t color, uint32_t fog_color, uint8_t factor);

#ifdef __cplusplus
}
#endif

#endif
