// The Style grade of nr_style_grade.glsl for the DLL's Natural and Cinematic entries, in closed form ([Network] Style
// Grade Acceleration, on by default; the *_grade_accel pipelines and the Interleaved programs built with
// NR_STYLE_GRADE_CLOSED). The same steps up to the tone bands, then the saturation step without its round trips through
// HSL.
//
// Why it is the same grade: for those two entries the black / white points, both tints, the tone-band gains, gamma
// and the saturation power are neutral, and the saturation factor (1 + saturation) is at most 1. Converting to HSL,
// scaling S by k <= 1 and converting back keeps the hue and the lightness L = (max + min) / 2 and moves every channel
// along c' = L + k (c - L): each channel is L plus S * min(L, 1 - L) times a function of the hue alone, the scaled S
// never reaches the clamp, and a grey stays L. The second round trip (saturation power 0: S' = sat(S)) and gamma 0 are
// identities. The compiler already folds the table form's identity log2 / exp2 pairs; its two HSL round trips it
// cannot fold, and they were ~690 instructions a pixel in the post block (~0.2 ms at 4K), this form ~100. Its results
// differ from the table form's only by that form's float rounding (rcp-based divides, two conversions): on game
// captures about 1 pixel in 100 000 by one step of the output format.
//
// Any other table entry (nr_style_grade_closed_valid() false) takes the table form.
#include "nr_style_grade.glsl"

bool nr_style_grade_closed_valid(NrGrade g) {
    return g.black == 0.0 && g.white == 1.0 && g.tint1 == 0.0 && g.tint2 == 0.0 && g.gamma == 0.0 && g.sat_power == 0.0 &&
           g.band0 == 0.0 && g.band1 == 0.0 && g.band2 == 0.0 && g.band3 == 0.0 && g.band4 == 0.0 && g.saturation <= 0.0;
}

// nr_style_grade(style, base, rgb, intensity) in closed form (same arguments and result).
vec3 nr_style_grade_closed(uint style, vec3 base, vec3 rgb, float intensity) {
    const NrGrade g = nr_grade_of(style);
    if (!nr_style_grade_closed_valid(g)) return nr_style_grade(style, base, rgb, intensity);
    vec3 c = clamp(rgb, vec3(0.0), vec3(1.0));
    c *= exp2(g.exposure);
    c = vec3(nr_grade_contrast(c.r, g.contrast), nr_grade_contrast(c.g, g.contrast), nr_grade_contrast(c.b, g.contrast));
    c = clamp(c, vec3(0.0), vec3(1.0));   // the tone bands' saturate (their gain is exp2(-0) = 1)
    const float L = (max(max(c.r, c.g), c.b) + min(min(c.r, c.g), c.b)) * 0.5;
    c = clamp(fma(vec3(g.saturation + 1.0), c - L, vec3(L)), vec3(0.0), vec3(1.0));
    return clamp(fma(vec3(nr_grade_sat(intensity)), c - base, base), vec3(0.0), vec3(1.0));
}
