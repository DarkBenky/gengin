#include "../object/format.h"

typedef struct {
    int ScreenXPos;
    int ScreenYPos;
    int UIWidth;
    int UIHeight;
    int radarScanWidth;
    int radarScanHeight;
    int radarScanDistance;
    uint32 *framebuffer; // pointer to the framebuffer for the radar screen
} radarScreenUi;

