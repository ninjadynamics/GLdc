/* KallistiGL for KallistiOS ##version##

   libgl/gl-error.c
   Copyright (C) 2014 Josh Pearson
   Copyright (C) 2016 Lawrence Sebald
 * Copyright (C) 2017 Luke Benstead

    KOS Open GL State Machine Error Code Implementation.
*/

#include <stdio.h>

#include "private.h"

static GLenum LAST_ERROR = GL_NO_ERROR;

GL_FORCE_INLINE const char* _glErrorEnumAsString(GLenum error) {
    switch(error) {
        case GL_INVALID_ENUM: return "GL_INVALID_ENUM";
        case GL_OUT_OF_MEMORY: return "GL_OUT_OF_MEMORY";
        case GL_INVALID_OPERATION: return "GL_INVALID_OPERATION";
        case GL_INVALID_VALUE: return "GL_INVALID_VALUE";
        default:
            return "GL_UNKNOWN_ERROR";
    }
}

void _glKosThrowError(GLenum error, const char *function) {
    if(LAST_ERROR == GL_NO_ERROR) {
        LAST_ERROR = error;
        /* Print the name directly: the old fixed 64-byte sprintf staging
           buffer was one long __func__ from stack corruption on the error
           path, and double-printed its newline (AUD-001-OPB-22). */
        fprintf(stderr, "GL ERROR: %s when calling %s\n", _glErrorEnumAsString(LAST_ERROR), function);
    }
}

GL_FORCE_INLINE void _glKosResetError() {
    LAST_ERROR = GL_NO_ERROR;
}

/* Quoth the GL Spec:
    When an error occurs, the error flag is set to the appropriate error code
    value. No other errors are recorded until glGetError is called, the error
    code is returned, and the flag is reset to GL_NO_ERROR.

   So, we only record an error here if the error code is currently unset.
   Nothing in the spec requires recording multiple error flags, although it is
   allowed by the spec. We take the easy way out for now. */


GLenum glGetError(void) {
    GLenum rv = LAST_ERROR;
    _glKosResetError();
    return rv;
}

const GLubyte *gluErrorString(GLenum error) {
    switch(error) {
        case GL_NO_ERROR:
            return (GLubyte *)"no error";

        case GL_INVALID_ENUM:
            return (GLubyte *)"invalid enumerant";

        case GL_INVALID_OPERATION:
            return (GLubyte *)"invalid operation";

        case GL_INVALID_VALUE:
            return (GLubyte *)"invalid value";

        case GL_OUT_OF_MEMORY:
            return (GLubyte *)"out of memory";

        default:
            return (GLubyte *)"unknown error";
    }
}
