#include "pixel.h"
#include <stdio.h>
#include <string.h>

enum { BLACK, FRAME, EDGE, HI, BG, BLUE, ICE, BLUE_DIM, GREEN,
       LIME, GREEN_DIM, WHITE, MUTED, AMBER, RED };
const unsigned int palette[COLORS] = {
    0x070b12, 0x202c3c, 0x35475a, 0x6b8092, 0x0b1420,
    0x0071c5, 0x71d6ff, 0x143449, 0x76b900, 0xc0f36b,
    0x263918, 0xeaf6ff, 0x6c8194, 0xffbd4a, 0xff625e
};
static unsigned char *canvas;
static void box(int x, int y, int w, int h, int color)
{
    for (int j = y; j < y + h && j < TILE; ++j)
        for (int i = x; i < x + w && i < TILE; ++i)
            if (i >= 0 && j >= 0) canvas[j * TILE + i] = (unsigned char)color;
}

/* Original 3x5 bitmap alphabet; every mark sits on the pixel grid. */
static const char *glyph(char c)
{
    switch (c) {
    case '0': return "111101101101111";
    case '1': return "010110010010111";
    case '2': return "111001111100111";
    case '3': return "111001111001111";
    case '4': return "101101111001001";
    case '5': return "111100111001111";
    case '6': return "111100111101111";
    case '7': return "111001010010010";
    case '8': return "111101111101111";
    case '9': return "111101111001111";
    case 'C': return "111100100100111";
    case 'P': return "110101110100100";
    case 'U': return "101101101101111";
    case 'G': return "111100101101111";
    case 'I': return "111010010010111";
    case 'N': return "101111111111101";
    case 'T': return "111010010010010";
    case 'E': return "111100110100111";
    case 'L': return "100100100100111";
    case 'V': return "101101101101010";
    case 'D': return "110101101101110";
    case 'A': return "010101111101101";
    case '-': return "000000111000000";
    default: return "000000000000000";
    }
}
static void text(int x, int y, const char *s, int color, int scale)
{
    for (; *s; ++s, x += 4 * scale) {
        const char *g = glyph(*s);
        for (int j = 0; j < 5; ++j)
            for (int i = 0; i < 3; ++i)
                if (g[j * 3 + i] == '1')
                    box(x + i * scale, y + j * scale, scale, scale, color);
    }
}
static void panel(int y, int value, int gpu)
{
    int base = gpu ? GREEN : BLUE;
    int bright = gpu ? LIME : ICE;
    int dim = gpu ? GREEN_DIM : BLUE_DIM;
    int ink = value >= 90 ? RED : value >= 80 ? AMBER : bright;
    box(4, y, 56, 26, BLACK);
    box(5, y + 1, 54, 24, BG);
    box(5, y + 1, 2, 24, base);
    text(10, y + 3, gpu ? "GPU" : "CPU", bright, 1);
    /* CPU microchip / GPU board, drawn as tiny pixel sprites. */
    box(10, y + 12, 11, 9, dim);
    box(12, y + 14, 7, 5, base);
    box(14, y + 15, 3, 3, bright);
    if (gpu) {
        box(9, y + 11, 1, 11, base);
        box(21, y + 13, 2, 6, base);
        box(12, y + 22, 2, 1, bright);
        box(16, y + 22, 2, 1, bright);
    } else {
        for (int i = 0; i < 3; ++i) {
            box(11 + i * 4, y + 10, 1, 2, bright);
            box(11 + i * 4, y + 21, 1, 2, base);
        }
    }
    char number[16];
    if (value < 0) snprintf(number, sizeof number, "--");
    else snprintf(number, sizeof number, "%02d", value);
    int n = (int)strlen(number);
    int x = 49 - (n * 8 - 2);
    text(x, y + 4, number, value < 0 ? MUTED : ink, 2);
    /* Degree mark and C remain legible even at native 64x64. */
    box(52, y + 3, 3, 3, ink); box(53, y + 4, 1, 1, BG);
    text(53, y + 8, "C", value < 0 ? MUTED : ink, 1);
    int bars = value < 0 ? 0 : (value > 100 ? 10 : (value + 9) / 10);
    for (int i = 0; i < 10; ++i)
        box(26 + i * 3, y + 18, 2, 4, i < bars ? ink : dim);
    box(54, y + 1, 3, 1, base);
}
void pixel_render(unsigned char pixels[TILE * TILE], Reading r, int demo)
{
    canvas = pixels;
    memset(pixels, BLACK, TILE * TILE);
    box(1, 1, 62, 62, FRAME);
    box(2, 2, 60, 1, HI); box(2, 3, 1, 58, EDGE);
    box(3, 61, 59, 1, BLACK); box(61, 3, 1, 59, BLACK);
    text(5, 5, "INTEL", ICE, 1);
    text(31, 5, "NVIDIA", LIME, 1);
    box(57, 5, 2, 2, demo ? AMBER : MUTED);
    panel(12, r.cpu, 0);
    panel(37, r.gpu, 1);
    /* The lower panel terminates within the outer frame. */
    box(4, 62, 56, 1, BLACK);
}
int pixel_save(const char *path, const unsigned char *pixels, int scale)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fprintf(f, "P6\n%d %d\n255\n", TILE * scale, TILE * scale);
    for (int y = 0; y < TILE * scale; ++y)
        for (int x = 0; x < TILE * scale; ++x) {
            unsigned int rgb = palette[pixels[(y / scale) * TILE + x / scale]];
            unsigned char bytes[] = {rgb >> 16, rgb >> 8, rgb};
            if (fwrite(bytes, 1, 3, f) != 3) { fclose(f); return 0; }
        }
    return fclose(f) == 0;
}
