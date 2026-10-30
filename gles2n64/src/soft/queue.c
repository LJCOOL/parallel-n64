#include <string.h>

#include "queue.h"

void soft_queue_reset(SoftQueue *q)
{
   q->count = 0;
}

int soft_queue_full(const SoftQueue *q)
{
   return q->count >= SOFT_QUEUE_MAX;
}

static SoftCmd *push(SoftQueue *q, SoftCmdKind kind, const SoftTarget *t)
{
   SoftCmd *c = &q->cmd[q->count++];
   c->kind = kind;
   if (t)
   {
      c->sx0 = t->sx0; c->sy0 = t->sy0;
      c->sx1 = t->sx1; c->sy1 = t->sy1;
   }
   return c;
}

void soft_queue_tri(SoftQueue *q, const SoftTarget *t, const SoftVert *v0,
      const SoftVert *v1, const SoftVert *v2, const SoftPipe *p)
{
   SoftCmd *c = push(q, SOFT_CMD_TRI, t);
   c->u.tri.v[0] = *v0;
   c->u.tri.v[1] = *v1;
   c->u.tri.v[2] = *v2;
   c->u.tri.pipe = *p;
   if (p->prog)
      c->u.tri.prog = *(const SoftCombineProgram*)p->prog;
}

void soft_queue_fill(SoftQueue *q, const SoftTarget *t, int x0, int y0, int x1, int y1, uint16_t c)
{
   SoftCmd *cmd = push(q, SOFT_CMD_FILL, t);
   cmd->u.fill.x0 = x0 < t->sx0 ? t->sx0 : x0;
   cmd->u.fill.y0 = y0 < t->sy0 ? t->sy0 : y0;
   cmd->u.fill.x1 = x1 > t->sx1 ? t->sx1 : x1;
   cmd->u.fill.y1 = y1 > t->sy1 ? t->sy1 : y1;
   cmd->u.fill.c = c;
}

void soft_queue_clear_color(SoftQueue *q, uint16_t c)
{
   push(q, SOFT_CMD_CLEAR_COLOR, NULL)->u.fill.c = c;
}

void soft_queue_clear_depth(SoftQueue *q)
{
   push(q, SOFT_CMD_CLEAR_DEPTH, NULL);
}

static void fill_rows(uint16_t *buf, int width, int x0, int y0, int x1, int y1,
      uint16_t c, int band)
{
   int x, y;
   for (y = y0; y < y1; y++)
   {
      uint16_t *row = buf + y * width;
      if (!soft_row_in_band(y, band))
         continue;
      for (x = x0; x < x1; x++)
         row[x] = c;
   }
}

void soft_queue_replay(const SoftQueue *q, SoftTarget *t, int band)
{
   SoftTarget local = *t;
   int i;

   for (i = 0; i < q->count; i++)
   {
      const SoftCmd *c = &q->cmd[i];
      switch (c->kind)
      {
         case SOFT_CMD_TRI:
         {
            SoftPipe p = c->u.tri.pipe;
            if (p.prog)
               p.prog = &c->u.tri.prog;
            local.sx0 = c->sx0; local.sy0 = c->sy0;
            local.sx1 = c->sx1; local.sy1 = c->sy1;
            soft_draw_tri_band(&local, &c->u.tri.v[0], &c->u.tri.v[1], &c->u.tri.v[2], &p, band);
            break;
         }
         case SOFT_CMD_FILL:
            fill_rows(t->color, t->width, c->u.fill.x0, c->u.fill.y0,
                  c->u.fill.x1, c->u.fill.y1, c->u.fill.c, band);
            break;
         case SOFT_CMD_CLEAR_COLOR:
            fill_rows(t->color, t->width, 0, 0, t->width, t->height, c->u.fill.c, band);
            break;
         case SOFT_CMD_CLEAR_DEPTH:
            fill_rows(t->depth, t->width, 0, 0, t->width, t->height, 0xffff, band);
            break;
      }
   }
}
