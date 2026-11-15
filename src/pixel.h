#ifndef PIXEL_H
#define PIXEL_H
#include "sensors.h"
#define TILE 64
#define COLORS 15
extern const unsigned int palette[COLORS];
void pixel_render(unsigned char pixels[TILE * TILE], Reading r, int demo);
int pixel_save(const char *path, const unsigned char *pixels, int scale);
#endif
