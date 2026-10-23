#ifdef GEMU_GTK
#include "gemu/video.h"
#include <epoxy/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct GemuVideoGtk {
    GtkWidget       *window;
    GtkWidget       *vbox;
    GtkWidget       *menubar;
    GtkWidget       *gl_area;
    GtkWidget       *drawing_area;  /* kept for API compat */
    uint32_t        *frame_argb;    /* staging buffer - new pixels land here */
    bool             frame_dirty;   /* frame_argb has a frame not yet uploaded */
    int              width;
    int              height;
    int              scale;
    int              window_width;
    int              window_height;
    int              aspect_w, aspect_h;  /* display shape; 0 = width:height */
    bool             active;
    /* OpenGL */
    GLuint           tex;
    GLuint           prog;
    GLuint           vao;
    GLuint           vbo;
    bool             gl_ready;
    GLint            u_tex_size;    /* sharp-bilinear shader uniforms */
    GLint            u_prescale;
};

/* ── Shaders ─────────────────────────────────────────────────────────────── */

static const char *vs_src =
    "#version 130\n"
    "in vec2 aPos;\n"
    "in vec2 aUV;\n"
    "out vec2 vUV;\n"
    "void main() {\n"
    "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "    vUV = aUV;\n"
    "}\n";

/* Sharp bilinear: behaves like nearest-neighbour scaling by the integer
 * factor uPrescale followed by a bilinear resize to the output, so pixels
 * stay crisp at any window size without uneven rows/columns.  Each texel's
 * interior samples its own center; only the band at its edge (whose width
 * shrinks as the prescale grows) blends with the neighbour. */
static const char *fs_src =
    "#version 130\n"
    "uniform sampler2D uTex;\n"
    "uniform vec2 uTexSize;\n"
    "uniform vec2 uPrescale;\n"
    "in vec2 vUV;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    vec2 texel = vUV * uTexSize;\n"
    "    vec2 center_dist = fract(texel) - 0.5;\n"
    "    vec2 range = 0.5 - 0.5 / uPrescale;\n"
    "    vec2 f = (center_dist - clamp(center_dist, -range, range)) * uPrescale + 0.5;\n"
    "    fragColor = texture(uTex, (floor(texel) + f) / uTexSize);\n"
    "}\n";

static GLuint gl_compile(GLenum type, const char *src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);
    GLint ok;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "gemu/gtk: shader compile error: %s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint gl_link(GLuint vs, GLuint fs) {
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint ok;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "gemu/gtk: shader link error: %s\n", log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

/* Fullscreen quad: two triangles covering [-1,1]² with UV [0,1]² */
static const float quad_data[] = {
    /* pos (x,y)    uv (u,v) */
    -1.0f, -1.0f,   0.0f, 1.0f,
     1.0f, -1.0f,   1.0f, 1.0f,
     1.0f,  1.0f,   1.0f, 0.0f,

    -1.0f, -1.0f,   0.0f, 1.0f,
     1.0f,  1.0f,   1.0f, 0.0f,
    -1.0f,  1.0f,   0.0f, 0.0f,
};

static void gl_setup(GemuVideoGtk *v) {
    GLuint vs = gl_compile(GL_VERTEX_SHADER,   vs_src);
    GLuint fs = gl_compile(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) return;
    v->prog = gl_link(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!v->prog) return;

    glUseProgram(v->prog);
    glUniform1i(glGetUniformLocation(v->prog, "uTex"), 0);
    v->u_tex_size = glGetUniformLocation(v->prog, "uTexSize");
    v->u_prescale = glGetUniformLocation(v->prog, "uPrescale");

    /* VAO + VBO */
    glGenVertexArrays(1, &v->vao);
    glBindVertexArray(v->vao);
    glGenBuffers(1, &v->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, v->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad_data), quad_data, GL_STATIC_DRAW);

    GLint aPos = glGetAttribLocation(v->prog, "aPos");
    GLint aUV  = glGetAttribLocation(v->prog, "aUV");
    glVertexAttribPointer((GLuint)aPos, 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(float), (void *)0);
    glEnableVertexAttribArray((GLuint)aPos);
    glVertexAttribPointer((GLuint)aUV, 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(float), (void *)(2 * sizeof(float)));
    glEnableVertexAttribArray((GLuint)aUV);

    /* Texture */
    glGenTextures(1, &v->tex);
    glBindTexture(GL_TEXTURE_2D, v->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, v->width, v->height,
                 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);

    v->gl_ready = true;
}

static void gl_teardown(GemuVideoGtk *v) {
    if (v->vao) { glDeleteVertexArrays(1, &v->vao); v->vao = 0; }
    if (v->vbo) { glDeleteBuffers(1, &v->vbo);       v->vbo = 0; }
    if (v->tex) { glDeleteTextures(1, &v->tex);       v->tex = 0; }
    if (v->prog){ glDeleteProgram(v->prog);            v->prog = 0; }
    v->gl_ready = false;
}

/* ── Picture placement ───────────────────────────────────────────────────── */

/* Largest rectangle with the framebuffer's aspect ratio that fits w x h,
 * centered. */
static void fit_rect(const GemuVideoGtk *v, int w, int h,
                     int *rx, int *ry, int *rw, int *rh) {
    int aw = v->aspect_w > 0 && v->aspect_h > 0 ? v->aspect_w : v->width;
    int ah = v->aspect_w > 0 && v->aspect_h > 0 ? v->aspect_h : v->height;
    if ((long)w * ah <= (long)h * aw) {
        *rw = w;
        *rh = (int)((long)w * ah / aw);
    } else {
        *rh = h;
        *rw = (int)((long)h * aw / ah);
    }
    if (*rw < 1) *rw = 1;
    if (*rh < 1) *rh = 1;
    *rx = (w - *rw) / 2;
    *ry = (h - *rh) / 2;
}

void gemu_video_gtk_picture_rect(GemuVideoGtk *v, int *x, int *y, int *w, int *h) {
    int aw = gtk_widget_get_allocated_width(v->gl_area);
    int ah = gtk_widget_get_allocated_height(v->gl_area);
    fit_rect(v, aw, ah, x, y, w, h);
}

/* ── GTK callbacks ───────────────────────────────────────────────────────── */

static gboolean on_realize(GtkWidget *w, gpointer data) {
    GemuVideoGtk *v = data;
    gtk_gl_area_make_current(GTK_GL_AREA(w));
    if (gtk_gl_area_get_error(GTK_GL_AREA(w)) != NULL) {
        fprintf(stderr, "gemu/gtk: GL area init failed\n");
        return FALSE;
    }
    gl_setup(v);
    return TRUE;
}

static void on_unrealize(GtkWidget *w, gpointer data) {
    GemuVideoGtk *v = data;
    gtk_gl_area_make_current(GTK_GL_AREA(w));
    gl_teardown(v);
}

static gboolean on_render(GtkGLArea *area, GdkGLContext *ctx, gpointer data) {
    (void)area;
    (void)ctx;
    GemuVideoGtk *v = data;
    if (!v->gl_ready) return TRUE;

    /* Upload pending frame - must happen here where the GL context is current.
     * GTK3 can leave GL_UNPACK_SKIP_ROWS non-zero after compositing widgets
     * in this same context; reset all unpack state so Mesa reads exactly the
     * right number of rows and doesn't overrun v->frame_argb. */
    if (v->frame_dirty) {
        glPixelStorei(GL_UNPACK_ALIGNMENT,   4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH,  0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS,   0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glBindTexture(GL_TEXTURE_2D, v->tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, v->width, v->height,
                        GL_BGRA, GL_UNSIGNED_BYTE, v->frame_argb);
        v->frame_dirty = false;
    }

    /* The GL framebuffer is in device pixels; allocations are in logical
     * pixels (they differ on HiDPI). */
    int sf = gtk_widget_get_scale_factor(GTK_WIDGET(area));
    int w = gtk_widget_get_allocated_width(GTK_WIDGET(area)) * sf;
    int h = gtk_widget_get_allocated_height(GTK_WIDGET(area)) * sf;

    glViewport(0, 0, w, h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (v->active) {
        int rx, ry, rw, rh;
        fit_rect(v, w, h, &rx, &ry, &rw, &rh);
        glViewport(rx, h - ry - rh, rw, rh);   /* GL's origin is bottom-left */
        float pre_x = (float)(rw / v->width), pre_y = (float)(rh / v->height);
        glUseProgram(v->prog);
        glUniform2f(v->u_tex_size, (float)v->width, (float)v->height);
        glUniform2f(v->u_prescale, pre_x < 1.0f ? 1.0f : pre_x, pre_y < 1.0f ? 1.0f : pre_y);
        glBindVertexArray(v->vao);
        glBindTexture(GL_TEXTURE_2D, v->tex);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    return TRUE;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

GemuVideoGtk *gemu_video_gtk_create(const GemuVideoGtkSpec *spec) {
    if (!spec || spec->width <= 0 || spec->height <= 0) return NULL;
    if (!gtk_init_check(NULL, NULL)) return NULL;

    GemuVideoGtk *v = calloc(1, sizeof(*v));
    if (!v) return NULL;

    v->width    = spec->width;
    v->height   = spec->height;
    v->scale    = spec->scale > 0 ? spec->scale : 1;
    v->window_width  = spec->window_width  > 0 ? spec->window_width
                                                : v->width * v->scale;
    v->window_height = spec->window_height > 0 ? spec->window_height
                                                : v->height * v->scale;
    v->aspect_w = spec->aspect_w;
    v->aspect_h = spec->aspect_h;
    if (spec->window_height <= 0 && v->aspect_w > 0 && v->aspect_h > 0)
        v->window_height = v->window_width * v->aspect_h / v->aspect_w;

    /* ARGB frame buffer for indexed/mono conversions */
    v->frame_argb = malloc((size_t)v->width * (size_t)v->height *
                           sizeof(*v->frame_argb));
    if (!v->frame_argb) {
        free(v);
        return NULL;
    }

    /* Window */
    v->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(v->window),
                         spec->title ? spec->title : "GEMU");
    gtk_window_set_resizable(GTK_WINDOW(v->window), TRUE);   /* picture scales to fit */
    /* Best-effort: gemu.png at the repo root, same convention as every other
     * relative path in this project. gdk-pixbuf (part of GTK) handles the
     * PNG decode, so no extra dependency is needed here. Missing/unreadable
     * icon just falls back to the platform default. */
    gtk_window_set_icon_from_file(GTK_WINDOW(v->window), "gemu.png", NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    v->vbox = vbox;
    gtk_container_add(GTK_CONTAINER(v->window), vbox);

    /* GL area - must not be focusable: if it grabs focus, key events go to
     * the GL area (which has no key handler) and don't reliably propagate
     * back to the window where our on_key signal handler lives. */
    v->gl_area = gtk_gl_area_new();
    gtk_widget_set_can_focus(v->gl_area, FALSE);
    /* The size request is the minimum size; the initial size is applied
     * after the first show via gtk_window_resize. */
    gtk_widget_set_size_request(v->gl_area, v->width < 160 ? v->width : 160,
                                            v->height < 100 ? v->height : 100);
    gtk_gl_area_set_required_version(GTK_GL_AREA(v->gl_area), 3, 2);
    gtk_box_pack_start(GTK_BOX(vbox), v->gl_area, TRUE, TRUE, 0);

    g_signal_connect(v->gl_area, "realize",   G_CALLBACK(on_realize),   v);
    g_signal_connect(v->gl_area, "unrealize", G_CALLBACK(on_unrealize), v);
    g_signal_connect(v->gl_area, "render",    G_CALLBACK(on_render),    v);

    /* For API compat - some callers expect a drawing_area widget */
    v->drawing_area = v->gl_area;

    gtk_widget_show_all(v->window);
    gemu_video_gtk_poll();

    gtk_window_resize(GTK_WINDOW(v->window), v->window_width, v->window_height);
    gemu_video_gtk_poll();

    return v;
}

/* Size the window so the picture area is w x h (adds the menu bar). */
static void resize_picture(GemuVideoGtk *v, int w, int h) {
    int bar_h = 0;
    if (v->menubar) gtk_widget_get_preferred_height(v->menubar, NULL, &bar_h);
    gtk_window_resize(GTK_WINDOW(v->window), w, h + bar_h);
}

void gemu_video_gtk_set_menubar(GemuVideoGtk *v, GtkWidget *bar) {
    if (!v || !bar || v->menubar) return;
    v->menubar = bar;
    gtk_box_pack_start(GTK_BOX(v->vbox), bar, FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(v->vbox), bar, 0);
    gtk_widget_show_all(bar);
    resize_picture(v, gtk_widget_get_allocated_width(v->gl_area),
                      gtk_widget_get_allocated_height(v->gl_area));
    gemu_video_gtk_poll();
}

int gemu_video_gtk_get_scale(GemuVideoGtk *v) {
    int w = v ? gtk_widget_get_allocated_width(v->gl_area) : 0;
    return v && v->width > 0 ? w / v->width : 0;
}

void gemu_video_gtk_set_scale(GemuVideoGtk *v, int scale) {
    if (!v || scale < 1) return;
    int h = v->height * scale;
    if (v->aspect_w > 0 && v->aspect_h > 0)
        h = v->width * scale * v->aspect_h / v->aspect_w;
    resize_picture(v, v->width * scale, h);
}

void gemu_video_gtk_destroy(GemuVideoGtk *v) {
    if (!v) return;
    if (v->window) gtk_widget_destroy(v->window);
    free(v->frame_argb);
    free(v);
}

void gemu_video_gtk_present_argb(GemuVideoGtk *v, const uint32_t *pixels,
                                 int w, int h) {
    if (!v || !pixels || w != v->width || h != v->height || !v->gl_ready)
        return;

    /* Stage the frame - upload happens in on_render where the GL context is
     * guaranteed to be current (GTK makes it current before the callback). */
    if (pixels != v->frame_argb)
        memcpy(v->frame_argb, pixels, (size_t)w * h * sizeof(*v->frame_argb));
    v->frame_dirty = true;
    v->active = true;
    gtk_gl_area_queue_render(GTK_GL_AREA(v->gl_area));
}


void gemu_video_gtk_poll(void) {
    while (gtk_events_pending())
        gtk_main_iteration_do(FALSE);
}

GtkWidget *gemu_video_gtk_window(GemuVideoGtk *v) {
    return v ? v->window : NULL;
}

GtkWidget *gemu_video_gtk_drawing_area(GemuVideoGtk *v) {
    return v ? v->drawing_area : NULL;
}
#endif /* GEMU_GTK */
