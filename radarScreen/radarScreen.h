#include "../object/format.h"
#include "../math/vector3.h"
#include "../math/angle.h"
#include "../util/saveImage.h"
#include "../render/cpu/font.h"
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
    float topViewToTopViewRatio;
    int offSetX;
    
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
    radarUi->topViewToTopViewRatio = 0.85f;
    radarUi->offSetX = UIWidth * (1.0f - radarUi->topViewToTopViewRatio);

    static const int initialCapacity = 16;
    radarUi->targets = (radarTarget *)malloc(sizeof(radarTarget) * initialCapacity);
    radarUi->numTargets = 0;
    radarUi->capacityTargets = initialCapacity;
}

typedef struct {
    float pitch;
    float yaw;
    float roll;
} Rotation;

static void calculateTargetsAngle(float3 radarPostion, float3 radarForward, float3 radarLeft, float3 radarUp, float3 targetPosition, Rotation *rotation) {
    float3 toTarget = Float3_Sub(targetPosition, radarPostion);
    float3 toTargetDir = Float3_Normalize(toTarget);

    float forwardDot = Float3_Dot(toTargetDir, radarForward);
    float leftDot   = Float3_Dot(toTargetDir, radarLeft);
    float upDot     = Float3_Dot(toTargetDir, radarUp);

    // yaw: horizontal angle from forward (positive = toward radarLeft)
    rotation->yaw = atan2f(leftDot, forwardDot);

    // pitch: vertical angle above the horizontal (forward-left) plane
    float horizontalDist = sqrtf(forwardDot * forwardDot + leftDot * leftDot);
    rotation->pitch = atan2f(upDot, horizontalDist);

    rotation->roll = 0.0f;
}

static void addRadarTarget(radarScreenUi *radarUi, float3 targetPosition, float3 radarPostion, float3 radarForward, float3 radarLeft, float3 radarUp) {
    float3 toTarget = Float3_Sub(targetPosition, radarPostion);
    float distance = Float3_Length(toTarget);
    
    // check if the target is within the radar's scan distance
    if (distance > radarUi->radarScanDistanceMeters) {
        return;
    }

    Rotation rotation;
    calculateTargetsAngle(radarPostion, radarForward, radarLeft, radarUp, targetPosition, &rotation);

    float rollDeg = radianToAngle(rotation.roll);
    float pitchDeg = radianToAngle(rotation.pitch);
    float yawDeg = radianToAngle(rotation.yaw);

    if (radarUi->radarScanWidthDegrees / 2.0f < fabsf(yawDeg) || radarUi->radarScanHeightDegrees / 2.0f < fabsf(pitchDeg)) {
        return;
    }
    
    if (radarUi->numTargets == radarUi->capacityTargets) {
        radarUi->capacityTargets *= 2;
        radarUi->targets = (radarTarget *)realloc(radarUi->targets, sizeof(radarTarget) * radarUi->capacityTargets);
    }

    float screenDistance = distance / radarUi->radarScanDistanceMeters;
    float screenDistanceZ = 1.0f - screenDistance;

    float effectiveWidth = (float)radarUi->UIWidth * radarUi->topViewToTopViewRatio;
    radarUi->targets[radarUi->numTargets].screenX = radarUi->offSetX + (yawDeg / (float)radarUi->radarScanWidthDegrees + 0.5f) * effectiveWidth;
    radarUi->targets[radarUi->numTargets].screenY = -(pitchDeg / (float)radarUi->radarScanHeightDegrees) * (float)radarUi->UIHeight;
    radarUi->targets[radarUi->numTargets].distanceScreenZ = screenDistanceZ;
    radarUi->numTargets++;
}

static void clearRadarTargets(radarScreenUi *radarUi) {
    radarUi->numTargets = 0;
}

static void drawRadarScreen(radarScreenUi *radarUi, struct Alphabet *alphabet) {
    const Color bgColor = RGBToUint32(16, 71, 32);
    const Color uiColor = RGBToUint32(22, 151, 60);
    const Color uiLinesColor = RGBToUint32(10, 46, 21);

    // draw background
    for (int y = 0; y < radarUi->UIHeight; y++) {
        for (int x = 0; x < radarUi->UIWidth; x++) {
            const int screenXPos = x + radarUi->ScreenXPos;
            const int screenYPos = y + radarUi->ScreenYPos;

            if (screenXPos < 0 || screenXPos >= radarUi->screenWidth || screenYPos < 0 || screenYPos >= radarUi->screenHeight) {
                continue;
            }

            radarUi->framebuffer[screenYPos * radarUi->screenWidth + screenXPos] = bgColor;
        }
    }

    const float effectiveWidth = (float)radarUi->UIWidth * radarUi->topViewToTopViewRatio;
    const int bScopeRight = radarUi->ScreenXPos + radarUi->offSetX + (int)effectiveWidth;

    // draw distance lines
    const int distanceLines = 8;
    for (int i = 0; i < distanceLines; i++) {
        const int lineY = radarUi->ScreenYPos + (int)((float)i / (float)(distanceLines - 1) * radarUi->UIHeight);
        for (int x = 0; x < (int)effectiveWidth; x++) {
            const int px = x + radarUi->ScreenXPos + radarUi->offSetX;
            if (px < 0 || px >= radarUi->screenWidth || lineY < 0 || lineY >= radarUi->screenHeight) {
                continue;
            }
            radarUi->framebuffer[lineY * radarUi->screenWidth + px] = uiLinesColor;
        }

        if (alphabet && i < distanceLines - 1) {
            char text[8];
            const int val = (int)((float)(distanceLines - 1 - i) / (float)(distanceLines - 1) * radarUi->radarScanDistanceMeters);
            if (val >= 1000) {
                sprintf(text, "%dK", (int)((float)(distanceLines - 1 - i) / (float)(distanceLines - 1) * radarUi->radarScanDistanceMeters / 1000));
            } else {
                sprintf(text, "%dm", (int)((float)(distanceLines - 1 - i) / (float)(distanceLines - 1) * radarUi->radarScanDistanceMeters));
            }
            RenderText(
                radarUi->framebuffer,
                radarUi->screenWidth,
                radarUi->screenHeight,
                alphabet,
                text,
                bScopeRight - 30,
                lineY + 10,
                0.9f,
                HexToUint32(0x8bb07b));
        }
    }

    // draw azimuth lines
    const int azimuthStep = 15;
    const int numAzimuthLines = radarUi->radarScanWidthDegrees / azimuthStep + 1;
    if (numAzimuthLines > 1) {
        const int halfScan = radarUi->radarScanWidthDegrees / 2;
        for (int i = 0; i < numAzimuthLines; i++) {
            const int yawDeg = -halfScan + i * azimuthStep;
            const float frac = (float)i / (float)(numAzimuthLines - 1);
            const int lineX = radarUi->ScreenXPos + radarUi->offSetX + (int)(frac * effectiveWidth);
            for (int y = 0; y < radarUi->UIHeight; y++) {
                const int py = y + radarUi->ScreenYPos;
                if (lineX < 0 || lineX >= radarUi->screenWidth || py < 0 || py >= radarUi->screenHeight) {
                    continue;
                }
                radarUi->framebuffer[py * radarUi->screenWidth + lineX] = uiLinesColor;
            }

            if (alphabet) {
                char text[8];
                sprintf(text, "%d", yawDeg);
                int textWidth = (int)((float)(text[0] == '-' ? 3 : 2) * 8.0f * 0.7f);
                RenderText(
                    radarUi->framebuffer,
                    radarUi->screenWidth,
                    radarUi->screenHeight,
                    alphabet,
                    text,
                    lineX - textWidth / 2 - 6,
                    radarUi->ScreenYPos + 8,
                    0.9f,
                    HexToUint32(0x8bb07b));
            }
        }
    }

    // draw elevation lines
    const int elevationStep = 15;
    const int numElevationLines = radarUi->radarScanHeightDegrees / elevationStep + 1;
    if (numElevationLines > 1) {
        const int halfScanHeight = radarUi->radarScanHeightDegrees / 2;
        const int elevationLeft = radarUi->ScreenXPos;
        const int elevationRight = radarUi->ScreenXPos + radarUi->offSetX;
        for (int i = 0; i < numElevationLines; i++) {
            const int pitchDeg = halfScanHeight - i * elevationStep;
            const int lineY = radarUi->ScreenYPos + radarUi->UIHeight / 2 + (int)(-(float)pitchDeg / (float)radarUi->radarScanHeightDegrees * radarUi->UIHeight);
            for (int x = elevationLeft; x < elevationRight; x++) {
                if (x < 0 || x >= radarUi->screenWidth || lineY < 0 || lineY >= radarUi->screenHeight) {
                    continue;
                }
                radarUi->framebuffer[lineY * radarUi->screenWidth + x] = uiLinesColor;
            }

            if (alphabet) {
                char text[8];
                sprintf(text, "%d", pitchDeg);
                RenderText(
                    radarUi->framebuffer,
                    radarUi->screenWidth,
                    radarUi->screenHeight,
                    alphabet,
                    text,
                    elevationLeft + 4,
                    lineY + 2,
                    0.9f,
                    HexToUint32(0x8bb07b));
            }
        }
    }

    // draw left to right (B-scope: azimuth vs range)
    const int targetWidth = 8;
    const int targetHeight = 4;
    for (int i = 0; i < radarUi->numTargets; i++) {
        const int screenXPos = radarUi->ScreenXPos + (int)radarUi->targets[i].screenX;
        const int screenYPos = radarUi->ScreenYPos + (int)(radarUi->targets[i].distanceScreenZ * (radarUi->UIHeight - targetHeight));

        if (screenXPos < 0 || screenXPos >= radarUi->screenWidth || screenYPos < 0 || screenYPos >= radarUi->screenHeight) {
            continue;
        }

        for (int y = 0; y < targetHeight; y++) {
            for (int x = 0; x < targetWidth; x++) {
                const int px = radarUi->ScreenXPos + (int)radarUi->targets[i].screenX + x;
                const int py = radarUi->ScreenYPos + (int)(radarUi->targets[i].distanceScreenZ * (radarUi->UIHeight - targetHeight)) + y;

                if (px < 0 || px >= radarUi->screenWidth || py < 0 || py >= radarUi->screenHeight) {
                    continue;
                }

                radarUi->framebuffer[py * radarUi->screenWidth + px] = uiColor;
            }
        }
    }


    // draw top to bottom (elevation + azimuth on the left margin)
    const int sidebarTargetWidth = targetWidth;
    for (int i = 0; i < radarUi->numTargets; i++) {
        float yawFrac = (radarUi->targets[i].screenX - radarUi->offSetX) / effectiveWidth;
        if (yawFrac < 0.0f) yawFrac = 0.0f;
        if (yawFrac > 1.0f) yawFrac = 1.0f;
        const int sidebarX = (int)(yawFrac * (radarUi->offSetX - sidebarTargetWidth));
        const int screenXPos = radarUi->ScreenXPos + sidebarX;
        const int screenYPos = radarUi->ScreenYPos + radarUi->UIHeight / 2 + (int)radarUi->targets[i].screenY;

        if (screenXPos < 0 || screenXPos >= radarUi->screenWidth || screenYPos < 0 || screenYPos >= radarUi->screenHeight) {
            continue;
        }

        for (int y = 0; y < targetHeight; y++) {
            for (int x = 0; x < sidebarTargetWidth; x++) {
                const int px = radarUi->ScreenXPos + sidebarX + x;
                const int py = radarUi->ScreenYPos + radarUi->UIHeight / 2 + (int)radarUi->targets[i].screenY + y;

                if (px < 0 || px >= radarUi->screenWidth || py < 0 || py >= radarUi->screenHeight) {
                    continue;
                }

                radarUi->framebuffer[py * radarUi->screenWidth + px] = uiColor;
            }
        }
    }
}