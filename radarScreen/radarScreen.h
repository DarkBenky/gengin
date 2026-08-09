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
    
    
    int currentRadarBeamScanWidth; // in degrees show where the radar beam is currently scanning (0 => midline, -45 => left edge, +45 => right edge)
    int prevRadarBeamScanWidth;
    int radarScanBeamWidth; // in degrees show how wide the radar beam is scanning

    int currentRadarBeamScanHeight; // in degrees show where the radar beam is currently scanning (0 => midline, -45 => left edge, +45 => right edge)
    int prevRadarBeamScanHeight; 
    int radarScanBeamHeight; // in degrees show how wide the radar beam is scanning

    int radarScanDistanceMeters;
    float topViewToTopViewRatio;
    int offSetX;
    
    uint32 *framebuffer; // pointer to the framebuffer for the radar screen
    int screenWidth; // width of full screen
    int screenHeight; // height of full screen

    radarTarget *targets1;
    int numTargets1;
    int capacityTargets1;

    radarTarget *targets2;
    int numTargets2;
    int capacityTargets2;

    bool DirectionIndicatorActive;
    bool currentBufferTargets1;
} radarScreenUi;

static void initRadarUi(
    uint32 *framebuffer, int screenWidth, int screenHeight,
    int radarScanWidthDegrees, int radarScanHeightDegrees, int radarScanDistanceMeters,
    int screenXPos, int screenYPos, int UIWidth, int UIHeight,
    radarScreenUi *radarUi, bool DirectionIndicatorActive,
    int radarScanBeamWidth, int radarScanBeamHeight
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

    radarUi->currentRadarBeamScanWidth = 0;
    radarUi->prevRadarBeamScanWidth = 0;
    radarUi->radarScanBeamWidth = radarScanBeamWidth;
    radarUi->currentRadarBeamScanHeight = 0;
    radarUi->prevRadarBeamScanHeight = 0;
    radarUi->radarScanBeamHeight = radarScanBeamHeight;

    static const int initialCapacity = 16;
    radarUi->targets1 = (radarTarget *)malloc(sizeof(radarTarget) * initialCapacity);
    radarUi->numTargets1 = 0;
    radarUi->capacityTargets1 = initialCapacity;

    if (DirectionIndicatorActive) {
        radarUi->targets2 = (radarTarget *)malloc(sizeof(radarTarget) * initialCapacity);
        radarUi->numTargets2 = 0;
        radarUi->capacityTargets2 = initialCapacity;
    }

    radarUi->DirectionIndicatorActive = DirectionIndicatorActive;
    radarUi->currentBufferTargets1 = true;
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
    
    if (radarUi->currentBufferTargets1) {
        if (radarUi->numTargets1 == radarUi->capacityTargets1) {
            radarUi->capacityTargets1 *= 2;
            radarUi->targets1 = (radarTarget *)realloc(radarUi->targets1, sizeof(radarTarget) * radarUi->capacityTargets1);
        }
    } else {
        if (radarUi->numTargets2 == radarUi->capacityTargets2) {
            radarUi->capacityTargets2 *= 2;
            radarUi->targets2 = (radarTarget *)realloc(radarUi->targets2, sizeof(radarTarget) * radarUi->capacityTargets2);
        }
    }

    float screenDistance = distance / radarUi->radarScanDistanceMeters;
    float screenDistanceZ = 1.0f - screenDistance;

    float effectiveWidth = (float)radarUi->UIWidth * radarUi->topViewToTopViewRatio;
    if (radarUi->currentBufferTargets1) {
        radarUi->targets1[radarUi->numTargets1].screenX = radarUi->offSetX + (yawDeg / (float)radarUi->radarScanWidthDegrees + 0.5f) * effectiveWidth;
        radarUi->targets1[radarUi->numTargets1].screenY = -(pitchDeg / (float)radarUi->radarScanHeightDegrees) * (float)radarUi->UIHeight;
        radarUi->targets1[radarUi->numTargets1].distanceScreenZ = screenDistanceZ;
        radarUi->numTargets1++;
    } else {
        radarUi->targets2[radarUi->numTargets2].screenX = radarUi->offSetX + (yawDeg / (float)radarUi->radarScanWidthDegrees + 0.5f) * effectiveWidth;
        radarUi->targets2[radarUi->numTargets2].screenY = -(pitchDeg / (float)radarUi->radarScanHeightDegrees) * (float)radarUi->UIHeight;
        radarUi->targets2[radarUi->numTargets2].distanceScreenZ = screenDistanceZ;
        radarUi->numTargets2++;
    }
}

static void clearRadarTargets(radarScreenUi *radarUi) {
    // keep the previous frame's buffer intact so direction indicators can track it
    if (radarUi->currentBufferTargets1) {
        radarUi->numTargets1 = 0;
    } else {
        radarUi->numTargets2 = 0;
    }
}

static void DrawLine(uint32 *framebuffer, int screenWidth, int screenHeight, int x0, int y0, int x1, int y1, Color color) {
    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    while (true) {
        if (x0 >= 0 && x0 < screenWidth && y0 >= 0 && y0 < screenHeight) {
            framebuffer[y0 * screenWidth + x0] = color;
        }

        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static int ClampInt(int value, int min, int max) {
    return value < min ? min : (value > max ? max : value);
}

static void FillRect(uint32 *framebuffer, int screenWidth, int screenHeight, int left, int right, int top, int bottom, Color color) {
    for (int y = top; y <= bottom; y++) {
        for (int x = left; x <= right; x++) {
            if (x < 0 || x >= screenWidth || y < 0 || y >= screenHeight) {
                continue;
            }
            framebuffer[y * screenWidth + x] = color;
        }
    }
}

static void drawRadarScreen(radarScreenUi *radarUi, struct Alphabet *alphabet, int currentRadarBeamScanWidth, int currentRadarBeamScanHeight, bool radarActive) {
    radarUi->prevRadarBeamScanHeight = radarUi->currentRadarBeamScanHeight;
    radarUi->prevRadarBeamScanWidth = radarUi->currentRadarBeamScanWidth;

    radarUi->currentRadarBeamScanHeight = currentRadarBeamScanHeight;
    radarUi->currentRadarBeamScanWidth = currentRadarBeamScanWidth;

    const Color bgColor = RGBToUint32(16, 71, 32);
    const Color uiColor = RGBToUint32(22, 151, 60);
    const Color ScanColor = RGBToUint32(25, 103, 48);
    const Color ScanPrevColor = RGBToUint32(26, 84, 43);
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

    // draw radar scan beam in both views (azimuth-distance and elevation-azimuth)
    if (radarActive) {
        const int halfScanWidth = radarUi->radarScanBeamWidth / 2;
        const int halfScanHeight = radarUi->radarScanBeamHeight / 2;
        const float effectiveWidth = (float)radarUi->UIWidth * radarUi->topViewToTopViewRatio;

        // azimuth-distance view: vertical sweep band at the beam azimuth spanning the full range
        const int scopeLeft = radarUi->ScreenXPos + radarUi->offSetX;
        const int scopeRight = scopeLeft + (int)effectiveWidth;
        const int prevBeamLeft = ClampInt(radarUi->ScreenXPos + radarUi->offSetX + (int)(((float)(radarUi->prevRadarBeamScanWidth - halfScanWidth) / (float)radarUi->radarScanWidthDegrees + 0.5f) * effectiveWidth), scopeLeft, scopeRight);
        const int prevBeamRight = ClampInt(radarUi->ScreenXPos + radarUi->offSetX + (int)(((float)(radarUi->prevRadarBeamScanWidth + halfScanWidth) / (float)radarUi->radarScanWidthDegrees + 0.5f) * effectiveWidth), scopeLeft, scopeRight);
        const int beamLeft = ClampInt(radarUi->ScreenXPos + radarUi->offSetX + (int)(((float)(radarUi->currentRadarBeamScanWidth - halfScanWidth) / (float)radarUi->radarScanWidthDegrees + 0.5f) * effectiveWidth), scopeLeft, scopeRight);
        const int beamRight = ClampInt(radarUi->ScreenXPos + radarUi->offSetX + (int)(((float)(radarUi->currentRadarBeamScanWidth + halfScanWidth) / (float)radarUi->radarScanWidthDegrees + 0.5f) * effectiveWidth), scopeLeft, scopeRight);

        FillRect(radarUi->framebuffer, radarUi->screenWidth, radarUi->screenHeight, prevBeamLeft, prevBeamRight, radarUi->ScreenYPos, radarUi->ScreenYPos + radarUi->UIHeight - 1, ScanPrevColor);
        FillRect(radarUi->framebuffer, radarUi->screenWidth, radarUi->screenHeight, beamLeft, beamRight, radarUi->ScreenYPos, radarUi->ScreenYPos + radarUi->UIHeight - 1, ScanColor);

        // elevation-azimuth view (left sidebar): beam box at the beam azimuth and elevation
        const int sideLeft = radarUi->ScreenXPos;
        const int sideRight = radarUi->ScreenXPos + radarUi->offSetX;
        const int sideTop = radarUi->ScreenYPos;
        const int sideBottom = radarUi->ScreenYPos + radarUi->UIHeight - 1;
        const int prevBeamSideLeft = ClampInt(radarUi->ScreenXPos + (int)(((float)(radarUi->prevRadarBeamScanWidth - halfScanWidth) / (float)radarUi->radarScanWidthDegrees + 0.5f) * radarUi->offSetX), sideLeft, sideRight);
        const int prevBeamSideRight = ClampInt(radarUi->ScreenXPos + (int)(((float)(radarUi->prevRadarBeamScanWidth + halfScanWidth) / (float)radarUi->radarScanWidthDegrees + 0.5f) * radarUi->offSetX), sideLeft, sideRight);
        const int prevBeamSideTop = ClampInt(radarUi->ScreenYPos + radarUi->UIHeight / 2 - (int)((float)(radarUi->prevRadarBeamScanHeight + halfScanHeight) / (float)radarUi->radarScanHeightDegrees * radarUi->UIHeight), sideTop, sideBottom);
        const int prevBeamSideBottom = ClampInt(radarUi->ScreenYPos + radarUi->UIHeight / 2 - (int)((float)(radarUi->prevRadarBeamScanHeight - halfScanHeight) / (float)radarUi->radarScanHeightDegrees * radarUi->UIHeight), sideTop, sideBottom);
        const int beamSideLeft = ClampInt(radarUi->ScreenXPos + (int)(((float)(radarUi->currentRadarBeamScanWidth - halfScanWidth) / (float)radarUi->radarScanWidthDegrees + 0.5f) * radarUi->offSetX), sideLeft, sideRight);
        const int beamSideRight = ClampInt(radarUi->ScreenXPos + (int)(((float)(radarUi->currentRadarBeamScanWidth + halfScanWidth) / (float)radarUi->radarScanWidthDegrees + 0.5f) * radarUi->offSetX), sideLeft, sideRight);
        const int beamSideTop = ClampInt(radarUi->ScreenYPos + radarUi->UIHeight / 2 - (int)((float)(radarUi->currentRadarBeamScanHeight + halfScanHeight) / (float)radarUi->radarScanHeightDegrees * radarUi->UIHeight), sideTop, sideBottom);
        const int beamSideBottom = ClampInt(radarUi->ScreenYPos + radarUi->UIHeight / 2 - (int)((float)(radarUi->currentRadarBeamScanHeight - halfScanHeight) / (float)radarUi->radarScanHeightDegrees * radarUi->UIHeight), sideTop, sideBottom);

        FillRect(radarUi->framebuffer, radarUi->screenWidth, radarUi->screenHeight, prevBeamSideLeft, prevBeamSideRight, prevBeamSideTop, prevBeamSideBottom, ScanPrevColor);
        FillRect(radarUi->framebuffer, radarUi->screenWidth, radarUi->screenHeight, beamSideLeft, beamSideRight, beamSideTop, beamSideBottom, ScanColor);
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
    // if direction indicator is active draw direction indicator
    if (radarActive) {
        const int targetWidth = 8;
        const int targetHeight = 4;
        const int numTargets = radarUi->currentBufferTargets1 ? radarUi->numTargets1 : radarUi->numTargets2;
        const int prevNumTargets = radarUi->currentBufferTargets1 ? radarUi->numTargets2 : radarUi->numTargets1;
        radarTarget *targets = radarUi->currentBufferTargets1 ? radarUi->targets1 : radarUi->targets2;
        radarTarget *prevTargets = radarUi->currentBufferTargets1 ? radarUi->targets2 : radarUi->targets1;

        for (int i = 0; i < numTargets; i++) {
            const int screenXPos = radarUi->ScreenXPos + (int)targets[i].screenX;
            const int screenYPos = radarUi->ScreenYPos + (int)(targets[i].distanceScreenZ * (radarUi->UIHeight - targetHeight));

            if (screenXPos < 0 || screenXPos >= radarUi->screenWidth || screenYPos < 0 || screenYPos >= radarUi->screenHeight) {
                continue;
            }

            for (int y = 0; y < targetHeight; y++) {
                for (int x = 0; x < targetWidth; x++) {
                    const int px = radarUi->ScreenXPos + (int)targets[i].screenX + x;
                    const int py = radarUi->ScreenYPos + (int)(targets[i].distanceScreenZ * (radarUi->UIHeight - targetHeight)) + y;

                    if (px < 0 || px >= radarUi->screenWidth || py < 0 || py >= radarUi->screenHeight) {
                        continue;
                    }

                    radarUi->framebuffer[py * radarUi->screenWidth + px] = uiColor;
                }
            }

            if (radarUi->DirectionIndicatorActive && i < prevNumTargets) {
                // heading arrow: extend from the target in the direction it moved since the last frame
                const int curCX = screenXPos + targetWidth / 2;
                const int curCY = screenYPos + targetHeight / 2;
                const int prevCX = radarUi->ScreenXPos + (int)prevTargets[i].screenX + targetWidth / 2;
                const int prevCY = radarUi->ScreenYPos + (int)(prevTargets[i].distanceScreenZ * (radarUi->UIHeight - targetHeight)) + targetHeight / 2;

                const float dirX = (float)(curCX - prevCX);
                const float dirY = (float)(curCY - prevCY);
                const float speed = sqrtf(dirX * dirX + dirY * dirY);
                if (speed > 0.5f) {
                    const float ux = dirX / speed;
                    const float uy = dirY / speed;
                    const float arrowLen = fminf(speed * 2.0f, 28.0f);
                    const int tipX = curCX + (int)(ux * arrowLen);
                    const int tipY = curCY + (int)(uy * arrowLen);

                    DrawLine(radarUi->framebuffer, radarUi->screenWidth, radarUi->screenHeight, curCX, curCY, tipX, tipY, uiColor);

                    // arrowhead barbs
                    const float px = -uy;
                    const float py = ux;
                    const int headSize = 5;
                    const int bx1 = tipX - (int)(ux * headSize) + (int)(px * headSize / 2);
                    const int by1 = tipY - (int)(uy * headSize) + (int)(py * headSize / 2);
                    const int bx2 = tipX - (int)(ux * headSize) - (int)(px * headSize / 2);
                    const int by2 = tipY - (int)(uy * headSize) - (int)(py * headSize / 2);
                    DrawLine(radarUi->framebuffer, radarUi->screenWidth, radarUi->screenHeight, tipX, tipY, bx1, by1, uiColor);
                    DrawLine(radarUi->framebuffer, radarUi->screenWidth, radarUi->screenHeight, tipX, tipY, bx2, by2, uiColor);
                }
            }
        }


        // draw top to bottom (elevation + azimuth on the left margin)
        const int sidebarTargetWidth = targetWidth;
        for (int i = 0; i < numTargets; i++) {
            float yawFrac = (targets[i].screenX - radarUi->offSetX) / effectiveWidth;
            if (yawFrac < 0.0f) yawFrac = 0.0f;
            if (yawFrac > 1.0f) yawFrac = 1.0f;
            const int sidebarX = (int)(yawFrac * (radarUi->offSetX - sidebarTargetWidth));
            const int screenXPos = radarUi->ScreenXPos + sidebarX;
            const int screenYPos = radarUi->ScreenYPos + radarUi->UIHeight / 2 + (int)targets[i].screenY;

            if (screenXPos < 0 || screenXPos >= radarUi->screenWidth || screenYPos < 0 || screenYPos >= radarUi->screenHeight) {
                continue;
            }

            for (int y = 0; y < targetHeight; y++) {
                for (int x = 0; x < sidebarTargetWidth; x++) {
                    const int px = radarUi->ScreenXPos + sidebarX + x;
                    const int py = radarUi->ScreenYPos + radarUi->UIHeight / 2 + (int)targets[i].screenY + y;

                    if (px < 0 || px >= radarUi->screenWidth || py < 0 || py >= radarUi->screenHeight) {
                        continue;
                    }

                    radarUi->framebuffer[py * radarUi->screenWidth + px] = uiColor;
                }
            }
        }
    }
    

    // if direction indicator active swap buffer indicator
    if (radarUi->DirectionIndicatorActive) {
        radarUi->currentBufferTargets1 = !radarUi->currentBufferTargets1;
    }
}