/*
Copyright (C) 2003-2006 Andrey Nazarov

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "gl.h"

drawStatic_t draw;

static inline void _GL_StretchPic(
    float x, float y, float w, float h,
    float s1, float t1, float s2, float t2,
    uint32_t color, int texnum, int flags)
{
    vec_t *dst_vert;
    uint32_t *dst_color;
    int *dst_indices;

    if (tess.numverts + 4 > TESS_MAX_VERTICES ||
        tess.numindices + 6 > TESS_MAX_INDICES ||
        (tess.numverts && tess.texnum[0] != texnum)) {
        GL_Flush2D();
    }

    tess.texnum[0] = texnum;

    dst_vert = tess.vertices + tess.numverts * 4;
    Vector4Set(dst_vert,      x,     y,     s1, t1);
    Vector4Set(dst_vert +  4, x + w, y,     s2, t1);
    Vector4Set(dst_vert +  8, x + w, y + h, s2, t2);
    Vector4Set(dst_vert + 12, x,     y + h, s1, t2);

    dst_color = (uint32_t *)tess.colors + tess.numverts;
    dst_color[0] = color;
    dst_color[1] = color;
    dst_color[2] = color;
    dst_color[3] = color;

    dst_indices = tess.indices + tess.numindices;
    dst_indices[0] = tess.numverts + 0;
    dst_indices[1] = tess.numverts + 2;
    dst_indices[2] = tess.numverts + 3;
    dst_indices[3] = tess.numverts + 0;
    dst_indices[4] = tess.numverts + 1;
    dst_indices[5] = tess.numverts + 2;

    if (flags & if_transparent) {
        if ((flags & if_paletted) && draw.scale == 1) {
            tess.flags |= 1;
        } else {
            tess.flags |= 2;
        }
    }

#define U32_ALPHA   MakeColor(0, 0, 0, 255)

    if ((color & U32_ALPHA) != U32_ALPHA) {
        tess.flags |= 2;
    }

    tess.numverts += 4;
    tess.numindices += 6;
}

#define GL_StretchPic(x,y,w,h,s1,t1,s2,t2,color,image) \
    _GL_StretchPic(x,y,w,h,s1,t1,s2,t2,color,(image)->texnum,(image)->flags)

void GL_Blend(void)
{
    color_t color;

    color.u8[0] = glr.fd.blend[0] * 255;
    color.u8[1] = glr.fd.blend[1] * 255;
    color.u8[2] = glr.fd.blend[2] * 255;
    color.u8[3] = glr.fd.blend[3] * 255;

    _GL_StretchPic(glr.fd.x, glr.fd.y, glr.fd.width, glr.fd.height, 0, 0, 1, 1,
                   color.u32, TEXNUM_WHITE, 0);
}

void R_ClearColor(void)
{
    draw.colors[0].u32 = U32_WHITE;
    draw.colors[1].u32 = U32_WHITE;
}

void R_SetAlpha(float alpha)
{
    draw.colors[0].u8[3] =
        draw.colors[1].u8[3] = alpha * 255;
}

void R_SetColor(uint32_t color)
{
    draw.colors[0].u32 = color;
    draw.colors[1].u8[3] = draw.colors[0].u8[3];
}

void R_SetClipRect(int flags, const clipRect_t *clip)
{
    clipRect_t rc;
    float scale;

    if ((draw.flags & DRAW_CLIP_MASK) == flags) {
        return;
    }

    GL_Flush2D();

    if (flags == DRAW_CLIP_DISABLED) {
        qglDisable(GL_SCISSOR_TEST);
        draw.flags &= ~DRAW_CLIP_MASK;
        return;
    }

    scale = 1 / draw.scale;

    rc.left = 0;
    rc.top = 0;
    if (flags & DRAW_CLIP_LEFT) {
        rc.left = clip->left * scale;
        if (rc.left < 0) {
            rc.left = 0;
        }
    }
    if (flags & DRAW_CLIP_TOP) {
        rc.top = clip->top * scale;
        if (rc.top < 0) {
            rc.top = 0;
        }
    }

    rc.right = r_config.width;
    rc.bottom = r_config.height;
    if (flags & DRAW_CLIP_RIGHT) {
        rc.right = clip->right * scale;
        if (rc.right > r_config.width) {
            rc.right = r_config.width;
        }
    }
    if (flags & DRAW_CLIP_BOTTOM) {
        rc.bottom = clip->bottom * scale;
        if (rc.bottom > r_config.height) {
            rc.bottom = r_config.height;
        }
    }

    if (rc.right < rc.left) {
        rc.right = rc.left;
    }
    if (rc.bottom < rc.top) {
        rc.bottom = rc.top;
    }

    qglEnable(GL_SCISSOR_TEST);
    qglScissor(rc.left, r_config.height - rc.bottom,
               rc.right - rc.left, rc.bottom - rc.top);
    draw.flags = (draw.flags & ~DRAW_CLIP_MASK) | flags;
}

void R_SetScale(float *scale)
{
    float f = scale ? *scale : 1;

    if (draw.scale == f) {
        return;
    }

    GL_Flush2D();

    qglMatrixMode(GL_PROJECTION);
    qglLoadIdentity();

    qglOrtho(0, Q_rint(r_config.width * f),
             Q_rint(r_config.height * f), 0, -1, 1);

    draw.scale = f;
}

qboolean R_GetPicSize(int *w, int *h, qhandle_t pic)
{
    image_t *image = IMG_ForHandle(pic);

    if (w) {
        *w = image->width;
    }
    if (h) {
        *h = image->height;
    }
    return image->flags & if_transparent;
}

void R_DrawStretchPic(int x, int y, int w, int h, qhandle_t pic)
{
    image_t *image = IMG_ForHandle(pic);

    GL_StretchPic(x, y, w, h, image->sl, image->tl, image->sh, image->th,
                  draw.colors[0].u32, image);
}

void R_DrawPic(int x, int y, qhandle_t pic)
{
    image_t *image = IMG_ForHandle(pic);

    GL_StretchPic(x, y, image->width, image->height,
                  image->sl, image->tl, image->sh, image->th, draw.colors[0].u32, image);
}

#define DIV64 (1.0f / 64.0f)

void R_TileClear(int x, int y, int w, int h, qhandle_t pic)
{
    GL_StretchPic(x, y, w, h, x * DIV64, y * DIV64,
                  (x + w) * DIV64, (y + h) * DIV64, U32_WHITE, IMG_ForHandle(pic));
}

void R_DrawFill8(int x, int y, int w, int h, int c)
{
    if (!w || !h)
        return;
    _GL_StretchPic(x, y, w, h, 0, 0, 1, 1, d_8to24table[c & 0xff], TEXNUM_WHITE, 0);
}

void R_DrawFill32(int x, int y, int w, int h, uint32_t color)
{
    if (!w || !h)
        return;
    _GL_StretchPic(x, y, w, h, 0, 0, 1, 1, color, TEXNUM_WHITE, 0);
}

static inline void draw_char(int x, int y, int c, qboolean alt, image_t *image)
{
    float s, t;

    if ((c & 127) == 32) {
        return;
    }

    c |= alt << 7;
    s = (c & 15) * 0.0625f;
    t = (c >> 4) * 0.0625f;
    GL_StretchPic(x, y, CHAR_WIDTH, CHAR_HEIGHT, s, t,
                  s + 0.0625f, t + 0.0625f, draw.colors[alt].u32, image);
}

void R_DrawChar(int x, int y, int flags, int c, qhandle_t font)
{
    qboolean alt = (flags & UI_ALTCOLOR) ? qtrue : qfalse;
    draw_char(x, y, c & 255, alt, IMG_ForHandle(font));
}

int R_DrawString(int x, int y, int flags, size_t maxlen, const char *s, qhandle_t font)
{
    image_t *image = IMG_ForHandle(font);
    qboolean alt = (flags & UI_ALTCOLOR) ? qtrue : qfalse;

    while (maxlen-- && *s) {
        byte c = *s++;
        draw_char(x, y, c, alt, image);
        x += CHAR_WIDTH;
    }

    return x;
}

#ifdef _DEBUG

image_t *r_charset;

void Draw_Stringf(int x, int y, const char *fmt, ...)
{
    va_list argptr;
    char buffer[MAX_STRING_CHARS];
    char *string;
    byte c;
    float s, t;

    va_start(argptr, fmt);
    Q_vsnprintf(buffer, sizeof(buffer), fmt, argptr);
    va_end(argptr);

    string = buffer;
    while (*string) {
        c = *string++;

        s = (c & 15) * 0.0625f;
        t = (c >> 4) * 0.0625f;

        GL_StretchPic(x, y, CHAR_WIDTH, CHAR_HEIGHT, s, t,
                      s + 0.0625f, t + 0.0625f, U32_WHITE, r_charset);
        x += CHAR_WIDTH;
    }
}

void Draw_Stats(void)
{
    int x = 10, y = 10;

    if (!r_charset) {
        qhandle_t tmp;
        tmp = R_RegisterFont("conchars");
        if (!tmp) return;
        r_charset = IMG_ForHandle(tmp);
    }

    Draw_Stringf(x, y, "Nodes visible  : %i", c.nodesVisible); y += 10;
    Draw_Stringf(x, y, "Nodes culled   : %i", c.nodesCulled); y += 10;
    Draw_Stringf(x, y, "Nodes drawn    : %i", c.nodesDrawn); y += 10;
    Draw_Stringf(x, y, "Leaves drawn   : %i", c.leavesDrawn); y += 10;
    Draw_Stringf(x, y, "Faces drawn    : %i", c.facesDrawn); y += 10;
    if (c.facesCulled) {
        Draw_Stringf(x, y, "Faces culled   : %i", c.facesCulled); y += 10;
    }
    if (c.boxesCulled) {
        Draw_Stringf(x, y, "Boxes culled   : %i", c.boxesCulled); y += 10;
    }
    if (c.spheresCulled) {
        Draw_Stringf(x, y, "Spheres culled : %i", c.spheresCulled); y += 10;
    }
    if (c.rotatedBoxesCulled) {
        Draw_Stringf(x, y, "RtBoxes culled : %i", c.rotatedBoxesCulled); y += 10;
    }
    Draw_Stringf(x, y, "Tris drawn   : %i", c.trisDrawn); y += 10;
    Draw_Stringf(x, y, "Tex switches : %i", c.texSwitches); y += 10;
    if (c.texUploads) {
        Draw_Stringf(x, y, "Tex uploads  : %i", c.texUploads); y += 10;
    }
    if (c.batchesDrawn) {
        Draw_Stringf(x, y, "Batches drawn: %i", c.batchesDrawn); y += 10;
        Draw_Stringf(x, y, "Faces / batch: %i", c.facesDrawn / c.batchesDrawn);
        y += 10;
        Draw_Stringf(x, y, "Tris / batch : %i", c.trisDrawn / c.batchesDrawn);
        y += 10;
    }
    Draw_Stringf(x, y, "2D batches   : %i", c.batchesDrawn2D); y += 10;
}

void Draw_Lightmaps(void)
{
    int i, x, y;

    for (i = 0; i < lm.nummaps; i++) {
        x = i & 1;
        y = i >> 1;
        _GL_StretchPic(256 * x, 256 * y, 256, 256,
                       0, 0, 1, 1, U32_WHITE, TEXNUM_LIGHTMAP + i, 0);
    }
}

void Draw_Scrap(void)
{
    _GL_StretchPic(0, 0, 256, 256,
                   0, 0, 1, 1, U32_WHITE, TEXNUM_SCRAP, if_paletted | if_transparent);
}

#endif
