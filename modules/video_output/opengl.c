/*****************************************************************************
 * opengl.c: OpenGL and OpenGL ES output common code
 *****************************************************************************
 * Copyright (C) 2004 the VideoLAN team
 * Copyright (C) 2009 Laurent Aimar
 *
 * Authors: Cyril Deguet <asmax@videolan.org>
 *          Gildas Bazin <gbazin@videolan.org>
 *          Eric Petit <titer@m0k.org>
 *          Cedric Cocquebert <cedric.cocquebert@supelec.fr>
 *          Laurent Aimar <fenrir _AT_ videolan _DOT_ org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_picture_pool.h>
#include <vlc_opengl.h>

#include "opengl.h"
// Define USE_OPENGL_ES to the GL ES Version you want to select

#if !defined (__APPLE__)
# if USE_OPENGL_ES == 2
#  include <GLES2/gl2ext.h>
# elif USE_OPENGL_ES == 1
#  include <GLES/glext.h>
//# else
//# include <GL/glext.h>
# endif
#else
# if USE_OPENGL_ES == 2
#  include <OpenGLES/ES2/gl.h>
# elif USE_OPENGL_ES == 1
#  include <OpenGLES/ES1/gl.h>
# else
#  define MACOS_OPENGL
#  include <OpenGL/glext.h>
# endif
#endif

/* RV16 */
#ifndef GL_UNSIGNED_SHORT_5_6_5
# define GL_UNSIGNED_SHORT_5_6_5 0x8363
#endif
#ifndef GL_CLAMP_TO_EDGE
# define GL_CLAMP_TO_EDGE 0x812F
#endif

#if USE_OPENGL_ES
#   define VLCGL_TEXTURE_COUNT 1
#elif defined(MACOS_OPENGL)
#   define VLCGL_TEXTURE_COUNT 2
#else
#   define VLCGL_TEXTURE_COUNT 1
#endif

struct vout_display_opengl_t {
    vlc_gl_t   *gl;

    video_format_t fmt;
    const vlc_chroma_description_t *chroma;

    int        tex_target;
    int        tex_format;
    int        tex_type;
    int        tex_width;
    int        tex_height;

    GLuint     texture[VLCGL_TEXTURE_COUNT];
    uint8_t    *buffer[VLCGL_TEXTURE_COUNT];
    void       *buffer_base[VLCGL_TEXTURE_COUNT];

    picture_pool_t *pool;

    GLuint     program;

    /* fragment_program */
    void (*GenProgramsARB)(GLuint, GLuint *);
    void (*BindProgramARB)(GLuint, GLuint);
    void (*ProgramStringARB)(GLuint, GLuint, GLint, const GLbyte *);
    void (*DeleteProgramsARB)(GLuint, GLuint *);
};

static inline int GetAlignedSize(unsigned size)
{
    /* Return the smallest larger or equal power of 2 */
    unsigned align = 1 << (8 * sizeof (unsigned) - clz(size));
    return ((align >> 1) == size) ? size : align;
}

vout_display_opengl_t *vout_display_opengl_New(video_format_t *fmt,
                                               vlc_gl_t *gl)
{
    vout_display_opengl_t *vgl = calloc(1, sizeof(*vgl));
    if (!vgl)
        return NULL;

    vgl->gl = gl;
    if (vlc_gl_Lock(vgl->gl)) {
        free(vgl);
        return NULL;
    }

    const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
    if (!extensions)
        extensions = "";

    /* Load extensions */
    bool supports_fp = false;
    if (strstr(extensions, "GL_ARB_fragment_program")) {
        vgl->GenProgramsARB    = (void (*)(GLuint, GLuint *))vlc_gl_GetProcAddress(vgl->gl, "glGenProgramsARB");
        vgl->BindProgramARB    = (void (*)(GLuint, GLuint))vlc_gl_GetProcAddress(vgl->gl, "glBindProgramARB");
        vgl->ProgramStringARB  = (void (*)(GLuint, GLuint, GLint, const GLbyte *))vlc_gl_GetProcAddress(vgl->gl, "glProgramStringARB");
        vgl->DeleteProgramsARB = (void (*)(GLuint, GLuint *))vlc_gl_GetProcAddress(vgl->gl, "glDeleteProgramsARB");

        supports_fp = vgl->GenProgramsARB &&
                      vgl->BindProgramARB &&
                      vgl->ProgramStringARB &&
                      vgl->DeleteProgramsARB;
    }

    /* Find the chroma we will use and update fmt */
    vgl->fmt = *fmt;
#if USE_OPENGL_ES
    vgl->fmt.i_chroma = VLC_CODEC_RGB16;
#   if defined(WORDS_BIGENDIAN)
    vgl->fmt.i_rmask  = 0x001f;
    vgl->fmt.i_gmask  = 0x07e0;
    vgl->fmt.i_bmask  = 0xf800;
#   else
    vgl->fmt.i_rmask  = 0xf800;
    vgl->fmt.i_gmask  = 0x07e0;
    vgl->fmt.i_bmask  = 0x001f;
#   endif
    vgl->tex_target   = GL_TEXTURE_2D;
    vgl->tex_format   = GL_RGB;
    vgl->tex_type     = GL_UNSIGNED_SHORT_5_6_5;
#elif defined(MACOS_OPENGL)
#   if defined(WORDS_BIGENDIAN)
    vgl->fmt.i_chroma = VLC_CODEC_YUYV;
#   else
    vgl->fmt.i_chroma = VLC_CODEC_UYVY;
#   endif
    vgl->tex_target   = GL_TEXTURE_RECTANGLE_EXT;
    vgl->tex_format   = GL_YCBCR_422_APPLE;
    vgl->tex_type     = GL_UNSIGNED_SHORT_8_8_APPLE;
#else
    vgl->fmt.i_chroma = VLC_CODEC_RGB32;
#   if defined(WORDS_BIGENDIAN)
    vgl->fmt.i_rmask  = 0xff000000;
    vgl->fmt.i_gmask  = 0x00ff0000;
    vgl->fmt.i_bmask  = 0x0000ff00;
#   else
    vgl->fmt.i_rmask  = 0x000000ff;
    vgl->fmt.i_gmask  = 0x0000ff00;
    vgl->fmt.i_bmask  = 0x00ff0000;
#   endif
    vgl->tex_target   = GL_TEXTURE_2D;
    vgl->tex_format   = GL_RGBA;
    vgl->tex_type     = GL_UNSIGNED_BYTE;
#endif

    vgl->chroma = vlc_fourcc_GetChromaDescription(vgl->fmt.i_chroma);

    bool supports_npot = false;
#if USE_OPENGL_ES == 2
    supports_npot = true;
#elif defined(MACOS_OPENGL)
    supports_npot = true;
#else
    supports_npot |= strstr(extensions, "GL_APPLE_texture_2D_limited_npot") != NULL ||
                     strstr(extensions, "GL_ARB_texture_non_power_of_two");
#endif

    /* Texture size */
    if (supports_npot) {
        vgl->tex_width  = vgl->fmt.i_width;
        vgl->tex_height = vgl->fmt.i_height;
    }
    else {
        /* A texture must have a size aligned on a power of 2 */
        vgl->tex_width  = GetAlignedSize(vgl->fmt.i_width);
        vgl->tex_height = GetAlignedSize(vgl->fmt.i_height);
    }

    /* Build fragment program if needed */
    vgl->program = 0;
    if (supports_fp) {
        char *code = NULL;
        if (code) {
            vgl->GenProgramsARB(1, &vgl->program);
            vgl->BindProgramARB(GL_FRAGMENT_PROGRAM_ARB, vgl->program);
            vgl->ProgramStringARB(GL_FRAGMENT_PROGRAM_ARB,
                                  GL_PROGRAM_FORMAT_ASCII_ARB,
                                  strlen(code), (const GLbyte*)code);
            if (glGetError() == GL_INVALID_OPERATION) {
                /* FIXME if the program was needed for YUV, the video will be broken */
#if 1
                GLint position;
                glGetIntegerv(GL_PROGRAM_ERROR_POSITION_ARB, &position);

                const char *msg = (const char *)glGetString(GL_PROGRAM_ERROR_STRING_ARB);
                fprintf(stderr, "GL_INVALID_OPERATION: error at %d: %s\n", position, msg);
#endif
                vgl->DeleteProgramsARB(1, &vgl->program);
                vgl->program = 0;
            }
            free(code);
        }
    }

    /* */
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    vlc_gl_Unlock(vgl->gl);

    /* */
    for (int i = 0; i < VLCGL_TEXTURE_COUNT; i++) {
        vgl->texture[i] = 0;
        vgl->buffer[i]  = NULL;
        vgl->buffer_base[i]  = NULL;
    }
    vgl->pool = NULL;

    *fmt = vgl->fmt;
    return vgl;
}

void vout_display_opengl_Delete(vout_display_opengl_t *vgl)
{
    /* */
    if (!vlc_gl_Lock(vgl->gl)) {

        glFinish();
        glFlush();
        glDeleteTextures(VLCGL_TEXTURE_COUNT, vgl->texture);

        if (vgl->program)
            vgl->DeleteProgramsARB(1, &vgl->program);

        vlc_gl_Unlock(vgl->gl);
    }
    if (vgl->pool) {
        picture_pool_Delete(vgl->pool);
        for (int i = 0; i < VLCGL_TEXTURE_COUNT; i++)
            free(vgl->buffer_base[i]);
    }
    free(vgl);
}

#ifdef MACOS_OPENGL
/* XXX See comment vout_display_opengl_Prepare */
struct picture_sys_t {
    vout_display_opengl_t *vgl;
    GLuint *texture;
};

/* Small helper */
static inline GLuint get_texture(picture_t *picture)
{
    return *picture->p_sys->texture;
}

static int PictureLock(picture_t *picture)
{
    if (!picture->p_sys)
        return VLC_SUCCESS;

    vout_display_opengl_t *vgl = picture->p_sys->vgl;
    if (!vlc_gl_Lock(vgl->gl)) {
        glBindTexture(vgl->tex_target, get_texture(picture));
        glTexSubImage2D(vgl->tex_target, 0, 0, 0,
                        picture->p[0].i_pitch / vgl->chroma->pixel_size,
                        picture->p[0].i_lines,
                        vgl->tex_format, vgl->tex_type, picture->p[0].p_pixels);

        vlc_gl_Unlock(vgl->gl);
    }
    return VLC_SUCCESS;
}

static void PictureUnlock(picture_t *picture)
{
    VLC_UNUSED(picture);
}
#endif

picture_pool_t *vout_display_opengl_GetPool(vout_display_opengl_t *vgl)
{
    if (vgl->pool)
        return vgl->pool;

    picture_t *picture[VLCGL_TEXTURE_COUNT];

    int i;
    for (i = 0; i < VLCGL_TEXTURE_COUNT; i++) {
        vgl->buffer[i] = vlc_memalign(&vgl->buffer_base[i], 16,
                                      vgl->tex_width * vgl->tex_height * vgl->chroma->pixel_size);
        if (!vgl->buffer[i])
            break;

        picture_resource_t rsc;
        memset(&rsc, 0, sizeof(rsc));
#ifdef MACOS_OPENGL
        rsc.p_sys = malloc(sizeof(*rsc.p_sys));
        if (rsc.p_sys)
        {
            rsc.p_sys->vgl = vgl;
            rsc.p_sys->texture = &vgl->texture[i];
        }
#endif
        rsc.p[0].p_pixels = vgl->buffer[i];
        rsc.p[0].i_pitch  = vgl->fmt.i_width * vgl->chroma->pixel_size;
        rsc.p[0].i_lines  = vgl->fmt.i_height;

        picture[i] = picture_NewFromResource(&vgl->fmt, &rsc);
        if (!picture[i]) {
            free(vgl->buffer[i]);
            vgl->buffer[i] = NULL;
            break;
        }
    }
    if (i < VLCGL_TEXTURE_COUNT)
        goto error;

    /* */
    picture_pool_configuration_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.picture_count = i;
    cfg.picture = picture;
#ifdef MACOS_OPENGL
    cfg.lock = PictureLock;
    cfg.unlock = PictureUnlock;
#endif
    vgl->pool = picture_pool_NewExtended(&cfg);
    if (!vgl->pool)
        goto error;

    if (vlc_gl_Lock(vgl->gl))
        return vgl->pool;

    glGenTextures(VLCGL_TEXTURE_COUNT, vgl->texture);
    for (int i = 0; i < VLCGL_TEXTURE_COUNT; i++) {
        glBindTexture(vgl->tex_target, vgl->texture[i]);

#if !USE_OPENGL_ES
        /* Set the texture parameters */
        glTexParameterf(vgl->tex_target, GL_TEXTURE_PRIORITY, 1.0);
        glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
#endif

        glTexParameteri(vgl->tex_target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(vgl->tex_target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(vgl->tex_target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(vgl->tex_target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

#ifdef MACOS_OPENGL
        /* Tell the driver not to make a copy of the texture but to use
           our buffer */
        glEnable(GL_UNPACK_CLIENT_STORAGE_APPLE);
        glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, GL_TRUE);

#if 0
        /* Use VRAM texturing */
        glTexParameteri(vgl->tex_target, GL_TEXTURE_STORAGE_HINT_APPLE,
                         GL_STORAGE_CACHED_APPLE);
#else
        /* Use AGP texturing */
        glTexParameteri(vgl->tex_target, GL_TEXTURE_STORAGE_HINT_APPLE,
                         GL_STORAGE_SHARED_APPLE);
#endif
#endif

        /* Call glTexImage2D only once, and use glTexSubImage2D later */
        if (vgl->buffer[i]) {
            glTexImage2D(vgl->tex_target, 0, vgl->tex_format, vgl->tex_width,
                         vgl->tex_height, 0, vgl->tex_format, vgl->tex_type,
                         vgl->buffer[i]);
        }
    }

    vlc_gl_Unlock(vgl->gl);

    return vgl->pool;

error:
    for (int j = 0; j < i; j++) {
        picture_Delete(picture[j]);
        vgl->buffer[j] = NULL;
    }
    return NULL;
}

int vout_display_opengl_Prepare(vout_display_opengl_t *vgl,
                                picture_t *picture)
{
    /* On Win32/GLX, we do this the usual way:
       + Fill the buffer with new content,
       + Reload the texture,
       + Use the texture.

       On OS X with VRAM or AGP texturing, the order has to be:
       + Reload the texture,
       + Fill the buffer with new content,
       + Use the texture.

       (Thanks to gcc from the Arstechnica forums for the tip)

       Therefore on OSX, we have to use two buffers and textures and use a
       lock(/unlock) managed picture pool.
     */

    if (vlc_gl_Lock(vgl->gl))
        return VLC_EGENERIC;

#ifdef MACOS_OPENGL
    /* Bind to the texture for drawing */
    glBindTexture(vgl->tex_target, get_texture(picture));
#else
    /* Update the texture */
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    picture->p[0].i_pitch / vgl->chroma->pixel_size,
                    picture->p[0].i_lines,
                    vgl->tex_format, vgl->tex_type, picture->p[0].p_pixels);
#endif

    vlc_gl_Unlock(vgl->gl);
    return VLC_SUCCESS;
}

int vout_display_opengl_Display(vout_display_opengl_t *vgl,
                                const video_format_t *source)
{
    if (vlc_gl_Lock(vgl->gl))
        return VLC_EGENERIC;

    /* glTexCoord works differently with GL_TEXTURE_2D and
       GL_TEXTURE_RECTANGLE_EXT */
    float f_normw, f_normh;

    if (vgl->tex_target == GL_TEXTURE_2D) {
        f_normw = vgl->tex_width;
        f_normh = vgl->tex_height;
    } else {
        f_normw = 1.0;
        f_normh = 1.0;
    }

    float f_x      = (source->i_x_offset +                       0 ) / f_normw;
    float f_y      = (source->i_y_offset +                       0 ) / f_normh;
    float f_width  = (source->i_x_offset + source->i_visible_width ) / f_normw;
    float f_height = (source->i_y_offset + source->i_visible_height) / f_normh;

    /* Why drawing here and not in Render()? Because this way, the
       OpenGL providers can call vout_display_opengl_Display to force redraw.i
       Currently, the OS X provider uses it to get a smooth window resizing */

    glClear(GL_COLOR_BUFFER_BIT);

    if (vgl->program)
        glEnable(GL_FRAGMENT_PROGRAM_ARB);
    else
        glEnable(vgl->tex_target);

#if USE_OPENGL_ES
    static const GLfloat vertexCoord[] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
         1.0f,  1.0f,
    };

    const GLfloat textureCoord[8] = {
        f_x,     f_height,
        f_width, f_height,
        f_x,     f_y,
        f_width, f_y
    };

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, vertexCoord);
    glTexCoordPointer(2, GL_FLOAT, 0, textureCoord);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
#else
    glBegin(GL_POLYGON);
    glTexCoord2f(f_x,      f_y);      glVertex2f(-1.0,  1.0);
    glTexCoord2f(f_width,  f_y);      glVertex2f( 1.0,  1.0);
    glTexCoord2f(f_width,  f_height); glVertex2f( 1.0, -1.0);
    glTexCoord2f(f_x,      f_height); glVertex2f(-1.0, -1.0);
    glEnd();
#endif

    if (vgl->program)
        glDisable(GL_FRAGMENT_PROGRAM_ARB);
    else
        glDisable(vgl->tex_target);

    vlc_gl_Swap(vgl->gl);

    vlc_gl_Unlock(vgl->gl);
    return VLC_SUCCESS;
}

