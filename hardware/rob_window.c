#include "rob_window.h"
#include <epoxy/gl.h>
#include <assimp/cimport.h>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/material.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROB_WIN_W 360
#define ROB_WIN_H 480
#define ROB_MAX_MESHES 16
#define ROB_GYRO_SEGMENTS 32
#define ROB_GYRO_VERTS ((ROB_GYRO_SEGMENTS + 1) * 6)
#define ROB_GYRO_IDX (ROB_GYRO_SEGMENTS * 12)
#define ROB_BOX_IDX 36

/* ── Simple column-major mat4 ────────────────────────────────────────────── */
typedef struct { float m[16]; } Mat4;

static Mat4 mat4_identity(void) {
    Mat4 r = {{0}};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

static Mat4 mat4_mul(Mat4 a, Mat4 b) {
    Mat4 r = {{0}};
    for (int col = 0; col < 4; col++)
        for (int row = 0; row < 4; row++)
            for (int k = 0; k < 4; k++)
                r.m[col*4+row] += a.m[k*4+row] * b.m[col*4+k];
    return r;
}

static Mat4 mat4_translate(float x, float y, float z) {
    Mat4 r = mat4_identity();
    r.m[12] = x; r.m[13] = y; r.m[14] = z;
    return r;
}

static Mat4 mat4_rotate_y(float angle) {
    float c = cosf(angle), s = sinf(angle);
    Mat4 r = mat4_identity();
    r.m[0] = c;  r.m[2] = -s;
    r.m[8] = s;  r.m[10] = c;
    return r;
}

static Mat4 mat4_rotate_z(float angle) {
    float c = cosf(angle), s = sinf(angle);
    Mat4 r = mat4_identity();
    r.m[0] = c;  r.m[1] = s;
    r.m[4] = -s; r.m[5] = c;
    return r;
}

static Mat4 mat4_scale(float x, float y, float z) {
    Mat4 r = mat4_identity();
    r.m[0] = x; r.m[5] = y; r.m[10] = z;
    return r;
}

static Mat4 mat4_perspective(float fovy_rad, float aspect, float n, float f) {
    float t = 1.0f / tanf(fovy_rad * 0.5f);
    Mat4 r = {{0}};
    r.m[0]  = t / aspect;
    r.m[5]  = t;
    r.m[10] = (f + n) / (n - f);
    r.m[11] = -1.0f;
    r.m[14] = 2.0f * f * n / (n - f);
    return r;
}

static Mat4 mat4_lookat(float ex, float ey, float ez,
                        float cx, float cy, float cz) {
    float ux = 0, uy = 1, uz = 0;
    float fx = cx-ex, fy = cy-ey, fz = cz-ez;
    float fn = sqrtf(fx*fx + fy*fy + fz*fz);
    fx /= fn; fy /= fn; fz /= fn;
    float rx = fy*uz - fz*uy, ry = fz*ux - fx*uz, rz = fx*uy - fy*ux;
    float rn = sqrtf(rx*rx + ry*ry + rz*rz);
    rx /= rn; ry /= rn; rz /= rn;
    float vx = ry*fz - rz*fy, vy = rz*fx - rx*fz, vz = rx*fy - ry*fx;
    Mat4 m = {{0}};
    m.m[0]  = rx;  m.m[1]  = vx;  m.m[2]  = -fx;  m.m[3]  = 0;
    m.m[4]  = ry;  m.m[5]  = vy;  m.m[6]  = -fy;  m.m[7]  = 0;
    m.m[8]  = rz;  m.m[9]  = vz;  m.m[10] = -fz;  m.m[11] = 0;
    m.m[12] = -(rx*ex + ry*ey + rz*ez);
    m.m[13] = -(vx*ex + vy*ey + vz*ez);
    m.m[14] =  (fx*ex + fy*ey + fz*ez);
    m.m[15] = 1.0f;
    return m;
}

/* ── Gyro cylinder geometry (unit radius, unit height) ───────────────────── */
static void build_cylinder(float *verts, unsigned *idx) {
    for (int i = 0; i <= ROB_GYRO_SEGMENTS; i++) {
        float a = (float)i * (2.0f * 3.14159265f / ROB_GYRO_SEGMENTS);
        float x = cosf(a), z = sinf(a);
        int v = i * 36;

        /* side bottom, side top */
        verts[v +  0] = x; verts[v +  1] = -0.5f; verts[v +  2] = z;
        verts[v +  3] = x; verts[v +  4] =  0.0f; verts[v +  5] = z;
        verts[v +  6] = x; verts[v +  7] =  0.5f; verts[v +  8] = z;
        verts[v +  9] = x; verts[v + 10] =  0.0f; verts[v + 11] = z;

        /* top rim, top center */
        verts[v + 12] = x; verts[v + 13] =  0.5f; verts[v + 14] = z;
        verts[v + 15] = 0; verts[v + 16] =  1.0f; verts[v + 17] = 0;
        verts[v + 18] = 0; verts[v + 19] =  0.5f; verts[v + 20] = 0;
        verts[v + 21] = 0; verts[v + 22] =  1.0f; verts[v + 23] = 0;

        /* bottom rim, bottom center */
        verts[v + 24] = x; verts[v + 25] = -0.5f; verts[v + 26] = z;
        verts[v + 27] = 0; verts[v + 28] = -1.0f; verts[v + 29] = 0;
        verts[v + 30] = 0; verts[v + 31] = -0.5f; verts[v + 32] = 0;
        verts[v + 33] = 0; verts[v + 34] = -1.0f; verts[v + 35] = 0;
    }

    for (int i = 0; i < ROB_GYRO_SEGMENTS; i++) {
        unsigned side0 = (unsigned)i * 6;
        unsigned side1 = side0 + 6;
        unsigned top0 = side0 + 2, top1 = side1 + 2;
        unsigned bot0 = side0 + 4, bot1 = side1 + 4;
        unsigned top_center = side0 + 3;
        unsigned bot_center = side0 + 5;
        int k = i * 12;

        idx[k +  0] = side0;     idx[k +  1] = side1;     idx[k +  2] = side1 + 1;
        idx[k +  3] = side0;     idx[k +  4] = side1 + 1; idx[k +  5] = side0 + 1;
        idx[k +  6] = top_center; idx[k +  7] = top0;      idx[k +  8] = top1;
        idx[k +  9] = bot_center; idx[k + 10] = bot1;      idx[k + 11] = bot0;
    }
}

/* ── GL mesh ─────────────────────────────────────────────────────────────── */
typedef struct {
    GLuint vao, vbo, ebo;
    int    n_idx;
    float  color[3];
    float  base_tx, base_ty, base_tz;  /* default translation from glTF node */
    int    arm_idx;   /* 0=left, 1=right, -1=fixed */
    bool   is_arm;    /* participates in vertical movement */
} RobMesh;

typedef struct {
    float x, z;
} RobColumnPos;

/* Positions copied from rob-code/Robert-Godot/RobotScene.tscn and
 * RobotVisual.cs.  Column order is ROB_COL_SPINNER, B, A, TRAY1, TRAY2. */
static const RobColumnPos GYRO_COL_POS[5] = {
    {-0.065f, -0.11258f},
    { 0.065f, -0.11258f},
    { 0.130f,  0.00000f},
    { 0.065f,  0.11258f},
    {-0.065f,  0.11258f},
};

static const float BOX_VERTS[144] = {
    -0.5f,-0.5f,-0.5f, 0,-1,0,  0.5f,-0.5f,-0.5f, 0,-1,0,
     0.5f,-0.5f, 0.5f, 0,-1,0, -0.5f,-0.5f, 0.5f, 0,-1,0,
    -0.5f, 0.5f,-0.5f, 0, 1,0,  0.5f, 0.5f,-0.5f, 0, 1,0,
     0.5f, 0.5f, 0.5f, 0, 1,0, -0.5f, 0.5f, 0.5f, 0, 1,0,
    -0.5f,-0.5f, 0.5f, 0,0, 1,  0.5f,-0.5f, 0.5f, 0,0, 1,
     0.5f, 0.5f, 0.5f, 0,0, 1, -0.5f, 0.5f, 0.5f, 0,0, 1,
     0.5f,-0.5f,-0.5f, 0,0,-1, -0.5f,-0.5f,-0.5f, 0,0,-1,
    -0.5f, 0.5f,-0.5f, 0,0,-1,  0.5f, 0.5f,-0.5f, 0,0,-1,
     0.5f,-0.5f, 0.5f, 1,0,0,   0.5f,-0.5f,-0.5f, 1,0,0,
     0.5f, 0.5f,-0.5f, 1,0,0,   0.5f, 0.5f, 0.5f, 1,0,0,
    -0.5f,-0.5f,-0.5f,-1,0,0,  -0.5f,-0.5f, 0.5f,-1,0,0,
    -0.5f, 0.5f, 0.5f,-1,0,0,  -0.5f, 0.5f,-0.5f,-1,0,0,
};

static const unsigned BOX_IDX[ROB_BOX_IDX] = {
     0,1,2, 0,2,3,   4,5,6, 4,6,7,   8,9,10, 8,10,11,
    12,13,14,12,14,15, 16,17,18,16,18,19, 20,21,22,20,22,23,
};

static int arm_step_to_column_for_view(int arm_step) {
    switch (arm_step) {
        case  0: return ROB_COL_A_BTN;
        case  1: return ROB_COL_B_BTN;
        case  2: return ROB_COL_SPINNER;
        case 10: return ROB_COL_TRAY2;
        case 11: return ROB_COL_TRAY1;
        default: return ROB_COL_NONE;
    }
}

static float arm_step_to_angle_for_view(int arm_step) {
    int col = arm_step_to_column_for_view(arm_step);
    if (col != ROB_COL_NONE)
        return -((float)col - 2.0f) * (3.14159265f / 3.0f);
    return (float)arm_step * (2.0f * 3.14159265f / ROB_ARM_STEPS);
}

/* ── Window ──────────────────────────────────────────────────────────────── */
struct RobWindow {
    SDL_Window   *win;
    SDL_GLContext ctx;
    Uint32        win_id;
    GLuint        prog;
    GLint         u_mvp, u_model, u_color, u_light;
    RobMesh       meshes[ROB_MAX_MESHES];
    int           n_meshes;
    /* Shared gyro cylinder mesh */
    GLuint        gyro_vao, gyro_vbo, gyro_ebo;
    GLuint        box_vao, box_vbo, box_ebo;
    /* Camera orbit state (spherical coords around ROB's centre) */
    float         cam_yaw;    /* horizontal angle around Y (radians) */
    float         cam_pitch;  /* vertical angle above horizontal (radians) */
    float         cam_dist;   /* distance from look-at target */
    bool          dragging;
};

static void draw_cylinder(RobWindow *w, Mat4 pv, float x, float y, float z,
                          float radius, float height, const float color[3]) {
    Mat4 model = mat4_mul(mat4_translate(x, y, z),
                          mat4_scale(radius, height, radius));
    Mat4 mvp = mat4_mul(pv, model);
    glUniformMatrix4fv(w->u_mvp,   1, GL_FALSE, mvp.m);
    glUniformMatrix4fv(w->u_model, 1, GL_FALSE, model.m);
    glUniform3fv(w->u_color, 1, color);
    glDrawElements(GL_TRIANGLES, ROB_GYRO_IDX, GL_UNSIGNED_INT, 0);
}

static void draw_disc_x(RobWindow *w, Mat4 pv, float x, float y, float z,
                        float radius, float thickness, const float color[3]) {
    Mat4 model = mat4_mul(mat4_translate(x, y, z),
                          mat4_mul(mat4_rotate_z(1.57079633f),
                                   mat4_scale(radius, thickness, radius)));
    Mat4 mvp = mat4_mul(pv, model);
    glUniformMatrix4fv(w->u_mvp,   1, GL_FALSE, mvp.m);
    glUniformMatrix4fv(w->u_model, 1, GL_FALSE, model.m);
    glUniform3fv(w->u_color, 1, color);
    glDrawElements(GL_TRIANGLES, ROB_GYRO_IDX, GL_UNSIGNED_INT, 0);
}

static void draw_box(RobWindow *w, Mat4 pv, float x, float y, float z,
                     float sx, float sy, float sz, float angle_y,
                     const float color[3]) {
    Mat4 model = mat4_mul(mat4_translate(x, y, z),
                          mat4_mul(mat4_rotate_y(angle_y), mat4_scale(sx, sy, sz)));
    Mat4 mvp = mat4_mul(pv, model);
    glUniformMatrix4fv(w->u_mvp,   1, GL_FALSE, mvp.m);
    glUniformMatrix4fv(w->u_model, 1, GL_FALSE, model.m);
    glUniform3fv(w->u_color, 1, color);
    glDrawElements(GL_TRIANGLES, ROB_BOX_IDX, GL_UNSIGNED_INT, 0);
}

/* ── Shaders ─────────────────────────────────────────────────────────────── */
static const char *VS_SRC =
    "#version 330 core\n"
    "layout(location=0) in vec3 aPos;\n"
    "layout(location=1) in vec3 aNorm;\n"
    "uniform mat4 uMVP;\n"
    "uniform mat4 uModel;\n"
    "out vec3 vNorm;\n"
    "void main() {\n"
    "    gl_Position = uMVP * vec4(aPos, 1.0);\n"
    "    vNorm = normalize(mat3(uModel) * aNorm);\n"
    "}\n";

static const char *FS_SRC =
    "#version 330 core\n"
    "in vec3 vNorm;\n"
    "out vec4 FragColor;\n"
    "uniform vec3 uColor;\n"
    "uniform vec3 uLight;\n"
    "void main() {\n"
    "    float d = max(dot(normalize(vNorm), normalize(uLight)), 0.0);\n"
    "    vec3 c = uColor * (0.25 + 0.75 * d);\n"
    "    FragColor = vec4(c, 1.0);\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[512]; glGetShaderInfoLog(s, 512, NULL, buf);
        fprintf(stderr, "rob: shader error: %s\n", buf);
    }
    return s;
}

static GLuint build_program(void) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER,   VS_SRC);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, FS_SRC);
    GLuint p  = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs); glDeleteShader(fs);
    return p;
}

/* ── Model loading ───────────────────────────────────────────────────────── */
static void get_material_color(const struct aiScene *sc, unsigned mat_idx,
                                float *out_r, float *out_g, float *out_b) {
    *out_r = *out_g = *out_b = 0.85f;
    if (mat_idx >= sc->mNumMaterials) return;
    struct aiMaterial *mat = sc->mMaterials[mat_idx];
    struct aiColor4D col;
    /* Try PBR base color first (glTF), then fall back to diffuse */
    if (AI_SUCCESS == aiGetMaterialColor(mat, AI_MATKEY_BASE_COLOR, &col) ||
        AI_SUCCESS == aiGetMaterialColor(mat, AI_MATKEY_COLOR_DIFFUSE, &col)) {
        *out_r = col.r; *out_g = col.g; *out_b = col.b;
    }
}

static void load_node(RobWindow *w, const struct aiScene *sc,
                      const struct aiNode *node) {
    if (w->n_meshes >= ROB_MAX_MESHES) return;

    const char *name = node->mName.data;

    for (unsigned mi = 0; mi < node->mNumMeshes && w->n_meshes < ROB_MAX_MESHES; mi++) {
        struct aiMesh *am = sc->mMeshes[node->mMeshes[mi]];

        /* Build interleaved [x y z nx ny nz] buffer */
        float *vbuf = malloc(am->mNumVertices * 6 * sizeof(float));
        for (unsigned vi = 0; vi < am->mNumVertices; vi++) {
            vbuf[vi*6+0] = am->mVertices[vi].x;
            vbuf[vi*6+1] = am->mVertices[vi].y;
            vbuf[vi*6+2] = am->mVertices[vi].z;
            if (am->mNormals) {
                vbuf[vi*6+3] = am->mNormals[vi].x;
                vbuf[vi*6+4] = am->mNormals[vi].y;
                vbuf[vi*6+5] = am->mNormals[vi].z;
            } else {
                vbuf[vi*6+3] = 0; vbuf[vi*6+4] = 1; vbuf[vi*6+5] = 0;
            }
        }
        unsigned n_idx = am->mNumFaces * 3;
        unsigned *ibuf = malloc(n_idx * sizeof(unsigned));
        for (unsigned fi = 0; fi < am->mNumFaces; fi++) {
            ibuf[fi*3+0] = am->mFaces[fi].mIndices[0];
            ibuf[fi*3+1] = am->mFaces[fi].mIndices[1];
            ibuf[fi*3+2] = am->mFaces[fi].mIndices[2];
        }

        RobMesh *rm = &w->meshes[w->n_meshes++];
        glGenVertexArrays(1, &rm->vao);
        glGenBuffers(1, &rm->vbo);
        glGenBuffers(1, &rm->ebo);
        glBindVertexArray(rm->vao);
        glBindBuffer(GL_ARRAY_BUFFER, rm->vbo);
        glBufferData(GL_ARRAY_BUFFER, am->mNumVertices * 6 * sizeof(float), vbuf, GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, rm->ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, n_idx * sizeof(unsigned), ibuf, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6*sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6*sizeof(float), (void*)(3*sizeof(float)));
        glEnableVertexAttribArray(1);
        glBindVertexArray(0);
        free(vbuf); free(ibuf);

        rm->n_idx = (int)n_idx;
        get_material_color(sc, am->mMaterialIndex,
                           &rm->color[0], &rm->color[1], &rm->color[2]);

        if (strcmp(name, "Base and Head") == 0 ||
            strcmp(name, "Shoulders") == 0) {
            rm->color[0] = 0.68f; rm->color[1] = 0.68f; rm->color[2] = 0.68f;
        } else if (strcmp(name, "Spine") == 0) {
            rm->color[0] = 0.08f; rm->color[1] = 0.08f; rm->color[2] = 0.08f;
        } else if (strcmp(name, "Arms") == 0 ||
                   strcmp(name, "Arms.001") == 0) {
            rm->color[0] = 0.36f; rm->color[1] = 0.36f; rm->color[2] = 0.34f;
        }

        /* Translation from node transform (aiMatrix4x4 is row-major) */
        rm->base_tx = node->mTransformation.a4;
        rm->base_ty = node->mTransformation.b4;
        rm->base_tz = node->mTransformation.c4;

        /* Classify by node name.
         * Only the gripper fingers move (height + rotation + open/close).
         * The body (Spine, Shoulders, Base, Head) is completely fixed. */
        if (strcmp(name, "Arms") == 0) {
            rm->arm_idx = 0; rm->is_arm = true;
        } else if (strcmp(name, "Arms.001") == 0) {
            rm->arm_idx = 1; rm->is_arm = true;
        } else {
            rm->arm_idx = -1;
            rm->is_arm  = false;
        }
    }

    for (unsigned ci = 0; ci < node->mNumChildren; ci++)
        load_node(w, sc, node->mChildren[ci]);
}

/* ── Public API ──────────────────────────────────────────────────────────── */
static int SDLCALL rob_event_watch(void *ud, SDL_Event *e);

RobWindow *rob_window_create(const char *model_dir) {
    char path[512];
    snprintf(path, sizeof path, "%s/ROB.gltf", model_dir);

    /* SDL OpenGL attributes must be set before creating the window */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    RobWindow *w = calloc(1, sizeof *w);
    w->win = SDL_CreateWindow("GEMU (R.O.B. Display)", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                              ROB_WIN_W, ROB_WIN_H, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
    if (!w->win) {
        fprintf(stderr, "rob: SDL_CreateWindow: %s\n", SDL_GetError());
        free(w); return NULL;
    }
    w->win_id = SDL_GetWindowID(w->win);

    w->ctx = SDL_GL_CreateContext(w->win);
    if (!w->ctx) {
        fprintf(stderr, "rob: SDL_GL_CreateContext: %s\n", SDL_GetError());
        SDL_DestroyWindow(w->win); free(w); return NULL;
    }
    SDL_GL_MakeCurrent(w->win, w->ctx);

    glEnable(GL_DEPTH_TEST);
    glClearColor(0.19f, 0.22f, 0.48f, 1.0f);

    w->prog  = build_program();
    w->u_mvp   = glGetUniformLocation(w->prog, "uMVP");
    w->u_model = glGetUniformLocation(w->prog, "uModel");
    w->u_color = glGetUniformLocation(w->prog, "uColor");
    w->u_light = glGetUniformLocation(w->prog, "uLight");

    const struct aiScene *sc = aiImportFile(path,
        aiProcess_Triangulate | aiProcess_GenSmoothNormals);
    if (!sc) {
        fprintf(stderr, "rob: assimp: %s\n", aiGetErrorString());
    } else {
        load_node(w, sc, sc->mRootNode);
        aiReleaseImport(sc);
    }

    /* Gyro cylinder mesh (reused for all gyro parts, different model matrices) */
    float gyro_verts[ROB_GYRO_VERTS * 6];
    unsigned gyro_idx[ROB_GYRO_IDX];
    build_cylinder(gyro_verts, gyro_idx);

    glGenVertexArrays(1, &w->gyro_vao);
    glGenBuffers(1, &w->gyro_vbo);
    glGenBuffers(1, &w->gyro_ebo);
    glBindVertexArray(w->gyro_vao);
    glBindBuffer(GL_ARRAY_BUFFER, w->gyro_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof gyro_verts, gyro_verts, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, w->gyro_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof gyro_idx, gyro_idx, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6*sizeof(float), (void*)(3*sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    glGenVertexArrays(1, &w->box_vao);
    glGenBuffers(1, &w->box_vbo);
    glGenBuffers(1, &w->box_ebo);
    glBindVertexArray(w->box_vao);
    glBindBuffer(GL_ARRAY_BUFFER, w->box_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof BOX_VERTS, BOX_VERTS, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, w->box_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof BOX_IDX, BOX_IDX, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6*sizeof(float), (void*)(3*sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    /* Initial orbit matching the old hardcoded lookat:
     *   eye=(0.14,0.26,0.52) target=(0.04,0.09,0.00)
     *   offset=(0.10,0.17,0.52), dist≈0.556, yaw≈0.190 rad, pitch≈0.311 rad */
    w->cam_yaw   = 0.7854f;
    w->cam_pitch = 0.6155f;
    w->cam_dist  = 0.56f;

    SDL_AddEventWatch(rob_event_watch, w);

    SDL_GL_MakeCurrent(NULL, NULL);
    return w;
}

void rob_window_render(RobWindow *w, const RobState *rob) {
    const RobMotorState *state = &rob->state;
    SDL_GL_MakeCurrent(w->win, w->ctx);

    glViewport(0, 0, ROB_WIN_W, ROB_WIN_H);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    float aspect = (float)ROB_WIN_W / ROB_WIN_H;
    Mat4 proj = mat4_perspective(0.7854f /* 45° */, aspect, 0.01f, 10.0f);
    /* Orbit camera: spherical coords around ROB's centre */
    const float tx = 0.04f, ty = 0.09f, tz = 0.00f;
    float ex = tx + w->cam_dist * cosf(w->cam_pitch) * sinf(w->cam_yaw);
    float ey = ty + w->cam_dist * sinf(w->cam_pitch);
    float ez = tz + w->cam_dist * cosf(w->cam_pitch) * cosf(w->cam_yaw);
    Mat4 view = mat4_lookat(ex, ey, ez, tx, ty, tz);
    Mat4 pv   = mat4_mul(proj, view);

    /* Key light from the upper-front (matches camera side) */
    float lx = 0.3f, ly = 1.2f, lz = 1.0f;

    /* Vertical offset: 5 height steps × 14mm = 70mm total travel (matches reference) */
    float vert_off = (float)state->arm_height * 0.014f;

    glUseProgram(w->prog);
    glUniform3f(w->u_light, lx, ly, lz);

    for (int i = 0; i < w->n_meshes; i++) {
        RobMesh *rm = &w->meshes[i];

        Mat4 model = mat4_translate(rm->base_tx,
                                    rm->base_ty + (rm->is_arm ? vert_off : 0.0f),
                                    rm->base_tz);

        if (rm->arm_idx >= 0) {
            /* Both gripper fingers rotate together as one arm assembly */
            float angle = arm_step_to_angle_for_view(state->arm_step);
            Mat4 rot = mat4_rotate_y(angle);
            model = mat4_mul(model, rot);

            /* Gripper open/close: rotate each finger inward around Y.
             * Actual tip vertex: x=0.149, z=±0.020. To bring z→0:
             * θ = atan(0.020/0.149) ≈ 0.133 rad ≈ 7.6°.
             * The old 0.436 rad used the bounding-box extent (z=±0.075) which
             * is the arm BASE, not the tip — that triple-overshot and crossed. */
            float close_ang = state->hands_open ? -0.070f : 0.133f;
            /* arm_idx 0 is at −Z: closing needs a negative Y rotation (moves −z toward 0).
             * arm_idx 1 is at +Z: closing needs a positive Y rotation (moves +z toward 0). */
            float fdir = (rm->arm_idx == 0) ? -1.0f : 1.0f;
            model = mat4_mul(model, mat4_rotate_y(fdir * close_ang));
        }

        Mat4 mvp = mat4_mul(pv, model);
        glUniformMatrix4fv(w->u_mvp,   1, GL_FALSE, mvp.m);
        glUniformMatrix4fv(w->u_model, 1, GL_FALSE, model.m);
        glUniform3fv(w->u_color, 1, rm->color);

        glBindVertexArray(rm->vao);
        glDrawElements(GL_TRIANGLES, rm->n_idx, GL_UNSIGNED_INT, 0);
    }

    static const float prop_black[3]   = {0.01f, 0.01f, 0.01f};
    static const float prop_grey[3]    = {0.58f, 0.58f, 0.60f};
    static const float prop_red[3]     = {0.86f, 0.05f, 0.03f};
    static const float prop_blue[3]    = {0.05f, 0.08f, 0.92f};
    static const float prop_led[3]     = {1.00f, 0.00f, 0.00f};

    glBindVertexArray(w->box_vao);
    draw_box(w, pv, -0.065f, 0.041605383f, -0.11258f,
             0.034f, 0.028f, 0.034f, -2.0943951f, prop_grey);

    glBindVertexArray(w->gyro_vao);
    /* ROB.gltf does not include the Godot head texture, so add simple eyes
     * on the head front as a visual reference. */
    draw_disc_x(w, pv, 0.050f, 0.220f, -0.025f, 0.016f, 0.002f, prop_black);
    draw_disc_x(w, pv, 0.050f, 0.220f,  0.025f, 0.016f, 0.002f, prop_black);
    draw_cylinder(w, pv, -0.065f, 0.086f, -0.11258f,
                  0.025f, 0.006f, prop_grey);
    draw_cylinder(w, pv, 0.130f, 0.0525f, 0.0f,
                  0.018f, 0.016f, prop_red);
    draw_cylinder(w, pv, 0.065f, 0.0525f, -0.11258f,
                  0.018f, 0.016f, prop_blue);
    draw_cylinder(w, pv, 0.065f, 0.058f, 0.11258f,
                  0.020f, 0.016f, prop_black);
    draw_cylinder(w, pv, -0.065f, 0.058f, 0.11258f,
                  0.020f, 0.016f, prop_black);
    draw_cylinder(w, pv, 0.029232476f, 0.24207518f, 0.0f,
                  0.004f, 0.004f, prop_led);

    /* Draw gyros as simple cylinder assemblies.
     * Held gyro tracks the current arm position; resting gyros sit at their
     * column's visual position from RobotVisual.cs.  The simulator logic treats
     * tray/button pickup as height 0, but the Godot mesh origin sits at y=0.086. */
    static const float gyro_rim[3]        = {0.95f, 0.95f, 0.95f};
    static const float gyro_top[3]        = {0.90f, 0.02f, 0.02f};
    static const float gyro_black[3]      = {0.01f, 0.01f, 0.01f};
    static const float gyro_topple[3]     = {0.55f, 0.55f, 0.55f};
    const float gyro_base_y = 0.086f;
    const float held_gyro_y = 0.03813508f + 0.11586492f - 0.012f;

    glBindVertexArray(w->gyro_vao);

    for (int i = 0; i < ROB_GYRO_COUNT; i++) {
        const RobGyroState *g = &rob->gyros[i];
        float gx, gy, gz;
        const float *top_col = gyro_top;

        if (i == rob->held_gyro) {
            int col_idx = arm_step_to_column_for_view(state->arm_step);
            if (col_idx >= 0 && col_idx < 5) {
                gx = GYRO_COL_POS[col_idx].x;
                gz = GYRO_COL_POS[col_idx].z;
            } else {
                float ang = arm_step_to_angle_for_view(state->arm_step);
                gx = 0.13f * cosf(ang);
                gz = -0.13f * sinf(ang);
            }
            gy = held_gyro_y + (float)state->arm_height * 0.014f;
            top_col = gyro_top;
        } else if (g->toppled) {
            /* Toppled: stay at last column, drop to floor height */
            gx = GYRO_COL_POS[g->column].x;
            gy = gyro_base_y;
            gz = GYRO_COL_POS[g->column].z;
            top_col = gyro_topple;
        } else {
            int rest_h = (g->column == ROB_COL_SPINNER) ? 3 : 0;
            gx = GYRO_COL_POS[g->column].x;
            gy = gyro_base_y + (float)rest_h * 0.014f;
            gz = GYRO_COL_POS[g->column].z;
        }

        const struct {
            float dy, radius, height;
            const float *color;
        } parts[] = {
            {0.000f, 0.022f, 0.005f, gyro_rim},
            {0.004f, 0.018f, 0.003f, top_col},
            {0.018f, 0.0025f, 0.022f, gyro_rim},
            {0.032f, 0.010f, 0.005f, gyro_black},
        };

        for (unsigned p = 0; p < sizeof parts / sizeof parts[0]; p++)
            draw_cylinder(w, pv, gx, gy + parts[p].dy, gz,
                          parts[p].radius, parts[p].height, parts[p].color);
    }
    glBindVertexArray(0);

    SDL_GL_SwapWindow(w->win);
    SDL_GL_MakeCurrent(NULL, NULL);
}

/* SDL event watch — fires for every event as it enters the queue, even when
 * gemu_display_poll() is draining the queue on the same SDL_PollEvent call. */
static int SDLCALL rob_event_watch(void *ud, SDL_Event *e) {
    RobWindow *w = ud;
    switch (e->type) {
    case SDL_MOUSEBUTTONDOWN:
        if (e->button.windowID == w->win_id && e->button.button == SDL_BUTTON_LEFT)
            w->dragging = true;
        break;
    case SDL_MOUSEBUTTONUP:
        if (e->button.button == SDL_BUTTON_LEFT)
            w->dragging = false;
        break;
    case SDL_MOUSEMOTION:
        if (w->dragging) {
            w->cam_yaw   -= (float)e->motion.xrel * 0.01f;
            w->cam_pitch -= (float)e->motion.yrel * 0.01f;
            if (w->cam_pitch >  1.50f) w->cam_pitch =  1.50f;
            if (w->cam_pitch < -1.50f) w->cam_pitch = -1.50f;
        }
        break;
    case SDL_MOUSEWHEEL:
        if (e->wheel.windowID == w->win_id) {
            w->cam_dist -= (float)e->wheel.y * 0.05f;
            if (w->cam_dist < 0.10f) w->cam_dist = 0.10f;
            if (w->cam_dist > 3.00f) w->cam_dist = 3.00f;
        }
        break;
    default: break;
    }
    return 1; /* always pass event through */
}

int rob_window_event(RobWindow *w, const SDL_Event *e) {
    if (e->type == SDL_WINDOWEVENT &&
        e->window.windowID == w->win_id &&
        e->window.event == SDL_WINDOWEVENT_CLOSE)
        return 1;
    return 0;
}

void rob_window_destroy(RobWindow *w) {
    if (!w) return;
    SDL_DelEventWatch(rob_event_watch, w);
    SDL_GL_MakeCurrent(w->win, w->ctx);
    glDeleteVertexArrays(1, &w->gyro_vao);
    glDeleteBuffers(1, &w->gyro_vbo);
    glDeleteBuffers(1, &w->gyro_ebo);
    glDeleteVertexArrays(1, &w->box_vao);
    glDeleteBuffers(1, &w->box_vbo);
    glDeleteBuffers(1, &w->box_ebo);
    SDL_GL_DeleteContext(w->ctx);
    SDL_DestroyWindow(w->win);
    free(w);
}
