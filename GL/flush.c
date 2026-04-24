
#include "../containers/aligned_vector.h"
#include "private.h"

PolyList COMMAND_LIST;

/**
 *  FAST_MODE will use invW for all Z coordinates sent to the
 *  GPU.
 *
 *  This will break orthographic mode so default is FALSE
 **/

#define FAST_MODE GL_FALSE

GLboolean AUTOSORT_ENABLED = GL_FALSE;

PolyList* _glCommandList() {
    return &COMMAND_LIST;
}

void APIENTRY glFlush() {
    SceneListSubmit((Vertex*) aligned_vector_front(&COMMAND_LIST.vector), aligned_vector_size(&COMMAND_LIST.vector));
    aligned_vector_clear(&COMMAND_LIST.vector);
}

void APIENTRY glFinish() {

}


void APIENTRY glKosInitConfig(GLdcConfig* config) {
    config->autosort_enabled = GL_FALSE;
    config->fsaa_enabled = GL_FALSE;

    config->initial_op_capacity = 1024 * 3;
    config->initial_pt_capacity = 512 * 3;
    config->initial_tr_capacity = 1024 * 3;
    config->initial_immediate_capacity = 1024 * 3;

    // RGBA4444 is the fastest general format - 8888 will cause a perf issue
    config->internal_palette_format = GL_RGBA4;

    config->texture_twiddle = GL_TRUE;
}

static bool _initialized = false;

void APIENTRY glKosInitEx(GLdcConfig* config) {
    if(_initialized) {
        return;
    }

    _initialized = true;

    TRACE();

    printf("\nWelcome to GLdc! Git revision: %s\n\n", GLDC_VERSION);

    InitGPU(config->autosort_enabled, config->fsaa_enabled);

    AUTOSORT_ENABLED = config->autosort_enabled;

    _glInitSubmissionTarget();
    _glInitMatrices();
    _glInitAttributePointers();
    _glInitContext();
    _glInitLights();
    _glInitImmediateMode(config->initial_immediate_capacity);
    _glInitFramebuffers();

    _glSetInternalPaletteFormat(config->internal_palette_format);

    _glInitTextures();

    if(config->texture_twiddle) {
        glEnable(GL_TEXTURE_TWIDDLE_KOS);
    }

    COMMAND_LIST.list_type = GPU_LIST_OP_POLY;

    aligned_vector_init(&COMMAND_LIST.vector, sizeof(Vertex));
    aligned_vector_reserve(&COMMAND_LIST.vector, config->initial_op_capacity);
}

void APIENTRY glKosShutdown() {
    aligned_vector_clear(&COMMAND_LIST.vector);

    ShutdownGPU();
    _initialized = false;
}

void APIENTRY glKosInit() {
    GLdcConfig config;
    glKosInitConfig(&config);
    glKosInitEx(&config);

    SceneListBegin(COMMAND_LIST.list_type);
}

void APIENTRY glKosSwapBuffers() {
    TRACE();

    if(aligned_vector_header(&COMMAND_LIST.vector)->size > 2) {
        glFlush();
    }

    SceneListFinish();
    SceneListBegin(COMMAND_LIST.list_type);

    aligned_vector_clear(&COMMAND_LIST.vector);

    _glApplyScissor(true);
}
