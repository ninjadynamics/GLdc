#include "private.h"

void APIENTRY glVertexPackColor3fKOS(GLVertexKOS* vertex, float r, float g, float b) {
    vertex->bgra[3] = 255;
    vertex->bgra[2] = (r * 255.0f);
    vertex->bgra[1] = (g * 255.0f);
    vertex->bgra[0] = (b * 255.0f);
}

void APIENTRY glVertexPackColor4fKOS(GLVertexKOS* vertex, float r, float g, float b, float a) {
    vertex->bgra[3] = (a * 255.0f);
    vertex->bgra[2] = (r * 255.0f);
    vertex->bgra[1] = (g * 255.0f);
    vertex->bgra[0] = (b * 255.0f);
}

uint32_t _glPackNormal(const GLfloat* nxyz) {
    uint8_t bx = (uint8_t)((nxyz[0] + 1) * 127.5);
    uint8_t by = (uint8_t)((nxyz[1] + 1) * 127.5);
    uint8_t bz = (uint8_t)((nxyz[2] + 1) * 127.5);
    return (bx << 16) | (by << 8) | bz;
}

void _glUnpackNormal(uint32_t packed, float* nxyz) {
    nxyz[0] = ((packed >> 16) & 0xFF) / 127.5 - 1;
    nxyz[1] = ((packed >> 8) & 0xFF) / 127.5 - 1;
    nxyz[2] = (packed & 0xFF) / 127.5 - 1;
}

half_float_t _glPackHalfFloat(float f) {

}

float _glUnpackHalfFloat(half_float_t h) {

}
