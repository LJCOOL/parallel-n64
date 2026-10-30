#include <string.h>

#include "../OpenGL.h"
#include "../Textures.h"
#include "../ShaderCombiner.h"
#include "../FrameBuffer.h"
#include "../DepthBuffer.h"
#include "../gDP.h"
#include "../RSP.h"
#include "../gles2N64.h"
#include "soft_backend.h"

GLInfo OGL;
TextureCache cache;
DepthBufferInfo depthBuffer;
struct FrameBufferInfo frameBuffer;
CachedTexture *g_RDRAMtoFB;
ShaderProgram *scProgramRoot;
ShaderProgram *scProgramCurrent;
int scProgramChanged;
int scProgramCount;

static uint16_t soft_color[SOFT_WIDTH * SOFT_HEIGHT];

const uint16_t *soft_frame(void)
{
   return soft_color;
}

bool OGL_Start(void)
{
   memset(soft_color, 0, sizeof(soft_color));
   DepthBuffer_Init();
   return true;
}

void OGL_Stop(void) { }

void OGL_AddTriangle(int v0, int v1, int v2) { }

void OGL_DrawTriangles(void)
{
   OGL.triangles.num = 0;
}

void OGL_DrawDMATriangles(uint32_t _numVtx) { }
void OGL_DrawTriangle(struct SPVertex *vertices, int v0, int v1, int v2) { }
void OGL_DrawLLETriangle(uint32_t _numVtx) { }
void OGL_DrawLine(int v0, int v1, float width) { }
void OGL_DrawRect(int ulx, int uly, int lrx, int lry, float *color) { }
void OGL_DrawTexturedRect(const struct TexturedRectParams *_params) { }
void OGL_UpdateFrameTime(void) { }
void OGL_UpdateScale(void) { }
void OGL_ClearDepthBuffer(bool _fullsize) { }
void OGL_ClearColorBuffer(float *color) { }
void OGL_ResizeWindow(int x, int y, int width, int height) { }

void OGL_SwapBuffers(void)
{
   int retro_return(bool a);

   if (renderCallback)
      (*renderCallback)();
   retro_return(true);

   scProgramChanged = 0;
   gDP.otherMode.l = 0;
   gln64gDPSetTextureLUT(G_TT_NONE);
   ++__RSP.DList;
}

void OGL_ReadScreen(void *dest, int *width, int *height)
{
   *width  = 0;
   *height = 0;
}

int OGL_CheckError(void) { return 0; }
int OGL_IsExtSupported(const char *extension) { return 0; }
float OGL_GetScaleX(void) { return 1.0f; }
float OGL_GetScaleY(void) { return 1.0f; }
uint32_t OGL_GetHeightOffset(void) { return 0; }
uint32_t OGL_GetScreenWidth(void) { return SOFT_WIDTH; }
uint32_t OGL_GetScreenHeight(void) { return SOFT_HEIGHT; }

CachedTexture *TextureCache_AddTop(void) { return NULL; }
void TextureCache_MoveToTop(CachedTexture *newtop) { }
void TextureCache_Remove(CachedTexture *texture) { }
void TextureCache_RemoveBottom(void) { }
void TextureCache_Init(void) { }
void TextureCache_Destroy(void) { }
void TextureCache_Update(uint32_t t) { }
void TextureCache_ActivateTexture(uint32_t t, CachedTexture *texture) { }
void TextureCache_ActivateNoise(uint32_t t) { }
void TextureCache_ActivateDummy(uint32_t t) { }
bool TextureCache_Verify(void) { return true; }

void ShaderCombiner_Init(void) { }
void ShaderCombiner_Destroy(void) { }
void ShaderCombiner_Set(DecodedMux *dmux, int flags) { }
void ShaderCombiner_UpdateBlendColor(void) { }
void ShaderCombiner_UpdateEnvColor(void) { }
void ShaderCombiner_UpdateFogColor(void) { }
void ShaderCombiner_UpdatePrimColor(void) { }
void ShaderCombiner_UpdateKeyColor(void) { }
void ShaderCombiner_UpdateLightParameters(void) { }
void ShaderCombiner_UpdateConvertColor(void) { }

void FrameBuffer_Init(void) { }
void FrameBuffer_Destroy(void) { }
void FrameBuffer_CopyToRDRAM(uint32_t _address) { }
void FrameBuffer_CopyFromRDRAM(uint32_t _address, bool _bUseAlpha) { }
void FrameBuffer_CopyDepthBuffer(uint32_t _address) { }
void FrameBuffer_ActivateBufferTexture(int16_t t, struct FrameBuffer *buffer) { }
void FrameBuffer_ActivateBufferTextureBG(int16_t t, struct FrameBuffer *buffer) { }
void FrameBuffer_SaveBuffer(uint32_t address, uint16_t format, uint16_t size,
      uint16_t width, uint16_t height, bool unknown) { }
void FrameBuffer_RenderBuffer(uint32_t address) { }
void FrameBuffer_RestoreBuffer(uint32_t address, uint16_t size, uint16_t width) { }
void FrameBuffer_RemoveBuffer(uint32_t address) { }
struct FrameBuffer *FrameBuffer_FindBuffer(uint32_t address) { return NULL; }
struct FrameBuffer *FrameBuffer_GetCurrent(void) { return NULL; }

/* One depth buffer record; the front end assumes SetBuffer leaves a current one. */
static DepthBuffer soft_depth_record;

void DepthBuffer_Init(void)
{
   memset(&soft_depth_record, 0, sizeof(soft_depth_record));
   depthBuffer.top = depthBuffer.bottom = depthBuffer.current = &soft_depth_record;
   depthBuffer.numBuffers = 1;
}

void DepthBuffer_Destroy(void) { }

void DepthBuffer_SetBuffer(uint32_t address)
{
   if (soft_depth_record.address != address)
   {
      soft_depth_record.address = address;
      soft_depth_record.cleared = false;
   }
   depthBuffer.current = &soft_depth_record;
}

void DepthBuffer_RemoveBuffer(uint32_t address) { }

DepthBuffer *DepthBuffer_FindBuffer(uint32_t address)
{
   return soft_depth_record.address == address ? &soft_depth_record : NULL;
}
