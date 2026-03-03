#ifndef RCS_H
#define RCS_H

#include "../math/vector3.h"
#include "../object/format.h"

typedef struct {
    float3 position;
    float3 forward;
    float3 up;
    float forwardRCS;
    float sideRCS; // Left and Right
    float backRCS;
    float topRCS; // Top and Bottom
} SurfaceInfo;

// Returns the radar cross section (RCS) in square meters for a ray coming from rayDir direction, given the surface info of the point being hit. This is a simplified model that treats the surface as a flat plate with different RCS values for front/back, sides, and top/bottom, and applies a cosine weighting based on the angle of incidence.
float calculateRadarCrossSection(float3 rayDir, SurfaceInfo surface);
// Returns the radar cross section (RCS) in square meters after applying Doppler shift effects based on the relative velocity between the ray and the surface. This can be used to simulate how the RCS changes for fast-moving objects due to relativistic effects.
float calculateRadarCrossSectionPostDoppler(float3 rayDir, SurfaceInfo surface, float relativeVelocity);

#endif // RCS_H