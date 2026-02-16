#ifndef COLOR_H
#define COLOR_H

#include "../../object/format.h"
#include <math.h>

typedef uint32 Color;

inline Color PackColor(float r, float g, float b);
inline Color PackColorF(float3 color);
inline Color BlendColors(Color c1, Color c2, float t);
inline float3 UnpackColor(Color c);
inline Color ApplyGamma(Color c, float gamma);
inline Color ApplyExposure(Color c, float exposure);
inline Color ApplyToneMapping(Color c);
inline Color LerpColor(Color c1, Color c2, float t);
inline Color ClampColor(Color c);
inline Color AddColors(Color c1, Color c2);
inline Color MultiplyColors(Color c1, Color c2);
inline Color ScaleColor(Color c, float s);
inline Color ModulateColor(Color c, float r, float g, float b);
inline Color ModulateColorF(Color c, float3 mod);
inline Color InvertColor(Color c);
inline Color GrayscaleColor(Color c);
inline Color DesaturateColor(Color c, float amount);
inline Color HueShiftColor(Color c, float shift);
inline Color AdjustSaturation(Color c, float saturation);
inline Color QuantizeColor(Color c, int levels);

#endif // COLOR_H