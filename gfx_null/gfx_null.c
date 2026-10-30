/* Null RDP plugin: drops every display list for CPU/RSP benchmarking. */
#include <stdint.h>
#include <string.h>

#include "m64p_plugin.h"
#include "m64p_types.h"

#define DP_INTERRUPT 0x20

extern GFX_INFO gfx_info;

/* Signals RDP completion so the game's scheduler never waits on drawing. */
static void null_signal_dp(void)
{
   *gfx_info.MI_INTR_REG |= DP_INTERRUPT;
   gfx_info.CheckInterrupts();
}

m64p_error nullPluginGetVersion(m64p_plugin_type *type, int *version,
      int *api, const char **name, int *caps)
{
   if (type)    *type = M64PLUGIN_GFX;
   if (version) *version = 0x0100;
   if (api)     *api = 0x020200;
   if (name)    *name = "null";
   if (caps)    *caps = 0;
   return M64ERR_SUCCESS;
}

void nullChangeWindow(void) { }
int  nullInitiateGFX(GFX_INFO info) { (void)info; return 1; }
void nullMoveScreen(int x, int y) { (void)x; (void)y; }
void nullProcessDList(void) { null_signal_dp(); }

/* Consumes the queued RDP commands without drawing them. */
void nullProcessRDPList(void)
{
   *gfx_info.DPC_START_REG = *gfx_info.DPC_END_REG;
   *gfx_info.DPC_CURRENT_REG = *gfx_info.DPC_END_REG;
   null_signal_dp();
}

void nullRomClosed(void) { }
int  nullRomOpen(void) { return 1; }
void nullShowCFB(void) { }
void nullUpdateScreen(void) { }
void nullViStatusChanged(void) { }
void nullViWidthChanged(void) { }
void nullReadScreen2(void *dest, int *w, int *h, int front)
{
   (void)dest; (void)front;
   if (w) *w = 0;
   if (h) *h = 0;
}
void nullSetRenderingCallback(void (*cb)(int)) { (void)cb; }
void nullFBRead(unsigned int addr) { (void)addr; }
void nullFBWrite(unsigned int addr, unsigned int size) { (void)addr; (void)size; }
void nullFBGetFrameBufferInfo(void *info) { (void)info; }
