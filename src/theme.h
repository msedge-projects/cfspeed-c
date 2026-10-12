/* Palette.
 *
 * The greys are deliberately neutral. An earlier Tokyo Night-derived palette
 * carried a blue cast tuned to sit on a #1a1b26 background; on a terminal whose
 * background is pure black that cast showed up as blue rules and blue-tinged
 * bars floating on the background.
 */
#ifndef CFS_THEME_H
#define CFS_THEME_H

#include <stdint.h>

#define RGB(r, g, b) (0xFF000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

#define T_SURFACE   RGB(26, 27, 38)
#define T_BORDER    RGB(64, 64, 64)
#define T_TEXT      RGB(169, 177, 214)
#define T_TEXT_HI   RGB(192, 202, 245)
#define T_TEXT_DIM  RGB(147, 153, 181)

#define T_DOWN      RGB(158, 206, 106)
#define T_UP        RGB(224, 175, 104)
#define T_LAT       RGB(125, 207, 255)

#define T_GRADE_AP  RGB(158, 206, 106)
#define T_GRADE_A   RGB(158, 206, 106)
#define T_GRADE_B   RGB(187, 224, 140)
#define T_GRADE_C   RGB(224, 175, 104)
#define T_GRADE_D   RGB(255, 158, 100)
#define T_GRADE_F   RGB(247, 118, 142)

/* Darken along the colour's own hue. Blending toward a blue-grey surface was
 * doing this job and tinted every bar on a black terminal. */
static inline uint32_t theme_shade(uint32_t c, double factor) {
    double f = factor < 0.0 ? 0.0 : (factor > 1.0 ? 1.0 : factor);
    uint32_t r = (uint32_t)(((c >> 16) & 0xFF) * f + 0.5);
    uint32_t g = (uint32_t)(((c >> 8) & 0xFF) * f + 0.5);
    uint32_t b = (uint32_t)((c & 0xFF) * f + 0.5);
    return RGB(r, g, b);
}

static inline uint32_t theme_grade(const char *g) {
    if (g == NULL) return T_TEXT_DIM;
    switch (g[0]) {
        case 'A': return (g[1] == '+') ? T_GRADE_AP : T_GRADE_A;
        case 'B': return T_GRADE_B;
        case 'C': return T_GRADE_C;
        case 'D': return T_GRADE_D;
        case 'F': return T_GRADE_F;
        default:  return T_TEXT_DIM;
    }
}

#endif /* CFS_THEME_H */