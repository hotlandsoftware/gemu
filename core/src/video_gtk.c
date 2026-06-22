#ifdef GEMU_GTK
#include "gemu/video.h"
#include "gemu/gtk_menu.h"
#include <epoxy/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct GemuVideoGtk {
    GtkWidget       *window;
    GtkWidget       *gl_area;
    GtkWidget       *drawing_area;  /* kept for API compat */
    uint32_t        *frame_argb;    /* staging buffer — new pixels land here */
    bool             frame_dirty;   /* frame_argb has a frame not yet uploaded */
    const uint32_t  *palette;
    int              n_colors;
    int              width;
    int              height;
    int              scale;
    int              window_width;
    int              window_height;
    bool             active;
    /* OpenGL */
    GLuint           tex;
    GLuint           prog;
    GLuint           vao;
    GLuint           vbo;
    bool             gl_ready;
    /* Integer-scale resize (resizable windows only) */
    bool             resizable;
    bool             setup_done;    /* ignore size-allocate during initial layout */
    int              pending_snap_w;
    int              pending_snap_h;
    guint            snap_source_id;
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

static const char *fs_src =
    "#version 130\n"
    "uniform sampler2D uTex;\n"
    "in vec2 vUV;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    fragColor = texture(uTex, vUV);\n"
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

/* ── Integer-scale resize ────────────────────────────────────────────────── */

static gboolean do_gtk_snap(gpointer data) {
    GemuVideoGtk *v = data;
    v->snap_source_id = 0;
    if (!v->width || !v->height) return G_SOURCE_REMOVE;

    int scale = (v->pending_snap_w + v->width / 2) / v->width;
    if (scale < 1) scale = 1;
    int target_w = v->width  * scale;
    int target_h = v->height * scale;

    int gl_w  = gtk_widget_get_allocated_width(v->gl_area);
    int gl_h  = gtk_widget_get_allocated_height(v->gl_area);
    int win_w = gtk_widget_get_allocated_width(GTK_WIDGET(v->window));
    int win_h = gtk_widget_get_allocated_height(GTK_WIDGET(v->window));

    if (target_w == gl_w && target_h == gl_h) return G_SOURCE_REMOVE;

    /* Resize window by the same delta the GL area needs to change — this
     * keeps the menu bar height out of the calculation entirely. */
    gtk_window_resize(GTK_WINDOW(v->window),
                      win_w + (target_w - gl_w),
                      win_h + (target_h - gl_h));
    return G_SOURCE_REMOVE;
}

static void on_gl_size_allocate(GtkWidget *widget, GtkAllocation *alloc,
                                gpointer data) {
    (void)widget;
    GemuVideoGtk *v = data;
    if (!v->resizable || !v->setup_done) return;
    if (v->snap_source_id)
        g_source_remove(v->snap_source_id);
    v->pending_snap_w = alloc->width;
    v->pending_snap_h = alloc->height;
    v->snap_source_id = g_timeout_add(150, do_gtk_snap, v);
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

    /* Upload pending frame — must happen here where the GL context is current.
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

    int w = gtk_widget_get_allocated_width(GTK_WIDGET(area));
    int h = gtk_widget_get_allocated_height(GTK_WIDGET(area));

    glViewport(0, 0, w, h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (v->active) {
        glUseProgram(v->prog);
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
    v->palette  = spec->palette;
    v->n_colors = spec->n_colors;

    /* ARGB frame buffer for indexed/mono conversions */
    v->frame_argb = malloc((size_t)v->width * (size_t)v->height *
                           sizeof(*v->frame_argb));
    if (!v->frame_argb) {
        free(v);
        return NULL;
    }

    /* Scale-based windows (window_width == 0 in spec) are resizable and snap
     * to integer multiples.  Fixed-size windows (KIM-1 keypad, etc.) are not. */
    v->resizable = (spec->window_width == 0);

    /* Window */
    v->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(v->window),
                         spec->title ? spec->title : "GEMU");
    gtk_window_set_resizable(GTK_WINDOW(v->window), v->resizable);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(v->window), vbox);
    gemu_gtk_add_action_menu(vbox, spec->monitor,
                              spec->hex_toggle_cb, spec->hex_toggle_ud);

    /* GL area — must not be focusable: if it grabs focus, key events go to
     * the GL area (which has no key handler) and don't reliably propagate
     * back to the window where our on_key signal handler lives. */
    v->gl_area = gtk_gl_area_new();
    gtk_widget_set_can_focus(v->gl_area, FALSE);
    /* For resizable windows, set minimum size to 1x; the initial scale is
     * applied after the first show via gtk_window_resize.
     * For fixed-size windows, the size request IS the window size. */
    if (v->resizable)
        gtk_widget_set_size_request(v->gl_area, v->width, v->height);
    else
        gtk_widget_set_size_request(v->gl_area, v->window_width, v->window_height);
    gtk_gl_area_set_required_version(GTK_GL_AREA(v->gl_area), 3, 2);
    gtk_box_pack_start(GTK_BOX(vbox), v->gl_area, TRUE, TRUE, 0);

    /* size-allocate connected before show so it fires during initial layout;
     * setup_done is false so those early firings are ignored. */
    if (v->resizable)
        g_signal_connect(v->gl_area, "size-allocate",
                         G_CALLBACK(on_gl_size_allocate), v);

    g_signal_connect(v->gl_area, "realize",   G_CALLBACK(on_realize),   v);
    g_signal_connect(v->gl_area, "unrealize", G_CALLBACK(on_unrealize), v);
    g_signal_connect(v->gl_area, "render",    G_CALLBACK(on_render),    v);

    /* For API compat — some callers expect a drawing_area widget */
    v->drawing_area = v->gl_area;

    gtk_widget_show_all(v->window);
    gemu_video_gtk_poll();

    if (v->resizable) {
        /* Resize to the configured initial scale.  We can now measure the
         * menu bar height as (window height - GL area height) and add it
         * back when resizing to the target dimensions. */
        int gl_h  = gtk_widget_get_allocated_height(v->gl_area);
        int win_h = gtk_widget_get_allocated_height(GTK_WIDGET(v->window));
        gtk_window_resize(GTK_WINDOW(v->window),
                          v->window_width,
                          v->window_height + (win_h - gl_h));
        gemu_video_gtk_poll();
        /* Initial layout complete — future size-allocates are user resizes. */
        v->setup_done = true;
    }

    return v;
}

void gemu_video_gtk_destroy(GemuVideoGtk *v) {
    if (!v) return;
    if (v->snap_source_id)
        g_source_remove(v->snap_source_id);
    if (v->window) gtk_widget_destroy(v->window);
    free(v->frame_argb);
    free(v);
}

void gemu_video_gtk_present_argb(GemuVideoGtk *v, const uint32_t *pixels,
                                 int w, int h) {
    if (!v || !pixels || w != v->width || h != v->height || !v->gl_ready)
        return;

    /* Stage the frame — upload happens in on_render where the GL context is
     * guaranteed to be current (GTK makes it current before the callback). */
    if (pixels != v->frame_argb)
        memcpy(v->frame_argb, pixels, (size_t)w * h * sizeof(*v->frame_argb));
    v->frame_dirty = true;
    v->active = true;
    gtk_gl_area_queue_render(GTK_GL_AREA(v->gl_area));
}

void gemu_video_gtk_present_indexed(GemuVideoGtk *v, const uint8_t *pixels,
                                    int w, int h) {
    if (!v || !pixels || w != v->width || h != v->height || !v->palette)
        return;

    /* Convert indexed → ARGB using palette, then copy rows via memcpy */
    int total = w * h;
    for (int i = 0; i < total; i++) {
        uint8_t idx = pixels[i];
        if (v->n_colors > 0 && idx >= v->n_colors)
            idx = (uint8_t)(v->n_colors - 1);
        v->frame_argb[i] = v->palette[idx];
    }
    gemu_video_gtk_present_argb(v, v->frame_argb, w, h);
}

void gemu_video_gtk_present_mono(GemuVideoGtk *v, const uint8_t *pixels,
                                 int w, int h, uint32_t on, uint32_t off) {
    if (!v || !pixels || w != v->width || h != v->height) return;
    int total = w * h;
    for (int i = 0; i < total; i++)
        v->frame_argb[i] = pixels[i] ? on : off;
    gemu_video_gtk_present_argb(v, v->frame_argb, w, h);
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
