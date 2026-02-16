#ifndef COLOR_H
#define COLOR_H

#include "../../object/format.h"
#include <math.h>

typedef uint32 Color;

Color PackColor(float r, float g, float b);
Color PackColorF(float3 color);
Color BlendColors(Color c1, Color c2, float t);
float3 UnpackColor(Color c);
Color ApplyGamma(Color c, float gamma);
Color ApplyExposure(Color c, float exposure);
Color ApplyToneMapping(Color c);
Color LerpColor(Color c1, Color c2, float t);
Color ClampColor(Color c);
Color AddColors(Color c1, Color c2);
Color SubtractColors(Color c1, Color c2);
Color MultiplyColors(Color c1, Color c2);
Color ScaleColor(Color c, float s);
Color ModulateColor(Color c, float r, float g, float b);
Color ModulateColorF(Color c, float3 mod);
Color InvertColor(Color c);
Color GrayscaleColor(Color c);
Color DesaturateColor(Color c, float amount);
Color HueShiftColor(Color c, float shift);
Color AdjustSaturation(Color c, float saturation);
Color QuantizeColor(Color c, int levels);
Color DarkenColor(Color c, float amount);

#endif // COLOR_H