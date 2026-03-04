#pragma once

#include "../include/GL/gl.h"

GLboolean _glNearZClippingEnabled();
GLboolean _glGPUStateIsDirty();
void _glGPUStateMarkClean();
void _glGPUStateMarkDirty();

float* _glCurrentColor();
float* _glCurrentNormal();
float* _glCurrentTexCoord0();
float* _glCurrentTexCoord1();

GLfloat _glGetHalfPointSize();
GLfloat _glGetHalfLineWidth();
