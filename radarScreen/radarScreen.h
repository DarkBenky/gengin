#include "../object/format.h"
#include "../math/vector3.h"
#include <stdlib.h>

typedef struct {
    float3 position;
    float screenX; // in scale 0.0 to 1.0
    float screenY; // in scale 0.0 to 1.0
    float distanceScreenZ; // in scale 0.0 to 1.0
    bool isVisible;
} radarTarget;

typedef struct {
    int ScreenXPos;
    int ScreenYPos;

    int UIWidth;
    int UIHeight;

    int radarScanWidthDegrees; // in degrees so for example in range +45 -45 where 0 is at radar forward
    int radarScanHeightDegrees; // in degrees so for example in range +45 -45 where 0 is at radar forward
    int radarScanDistanceMeters;
    
    uint32 *framebuffer; // pointer to the framebuffer for the radar screen
    int screenWidth; // width of full screen
    int screenHeight; // height of full screen

    radarTarget *targets;
    int numTargets;
    int capacityTargets;
} radarScreenUi;

static void initRadarUi(
    uint32 *framebuffer, int screenWidth, int screenHeight,
    int radarScanWidthDegrees, int radarScanHeightDegrees, int radarScanDistanceMeters,
    int screenXPos, int screenYPos, int UIWidth, int UIHeight,
    radarScreenUi *radarUi
) {
    radarUi->framebuffer = framebuffer;
    radarUi->screenWidth = screenWidth;
    radarUi->screenHeight = screenHeight;
    radarUi->radarScanWidthDegrees = radarScanWidthDegrees;
    radarUi->radarScanHeightDegrees = radarScanHeightDegrees;
    radarUi->radarScanDistanceMeters = radarScanDistanceMeters;
    radarUi->ScreenXPos = screenXPos;
    radarUi->ScreenYPos = screenYPos;
    radarUi->UIWidth = UIWidth;
    radarUi->UIHeight = UIHeight;

    static const int initialCapacity = 16;
    radarUi->targets = (radarTarget *)malloc(sizeof(radarTarget) * initialCapacity);
    radarUi->numTargets = 0;
    radarUi->capacityTargets = initialCapacity;
}

void addRadarTarget(radarScreenUi *radarUi, float3 targetPosition, float3 radarPostion, float3 radarForward, float3 radarLeft, float3 radarUp) {
    float3 toTarget = Float3_Sub(targetPosition, radarPostion);
    float distance = Float3_Length(toTarget);
    
    // check if the target is within the radar's scan distance
    if (distance > radarUi->radarScanDistanceMeters) {
        return;
    }

    float3 toTargetNormalized = Float3_Normalize(toTarget);
    float forwardDot = Float3_Dot(toTargetNormalized, radarForward);



}
