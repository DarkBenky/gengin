#include <string.h>

#include "../../object/format.h"
#include "../../object/object.h"
#include "../../object/scene.h"

enum EditorMode {
    MOVE_MODE, // default mode
    ROTATE_MODE,
    SCALE_MODE
};

typedef struct editorUi {
    ObjectList* scene;
    Object* selectedObject; // if NULL, no object is selected
    enum EditorMode mode;
    bool EditorActive;
    float moveSpeed;
    float rotationSpeed;
    float scaleSpeed;
} editorUi;

Error createEditorUi(ObjectList *objectList, editorUi* ui) {
    Error err;
    if (ui == NULL || objectList == NULL) {
        err.err = E_INVALID_ARG;
        strcpy(err.msg, "Invalid arguments");
        return err;
    }
    ui->scene = objectList;
    ui->selectedObject = NULL;
    ui->mode = MOVE_MODE;
    ui->EditorActive = false;
    ui->moveSpeed = 1.0f;
    ui->rotationSpeed = 1.0f;
    ui->scaleSpeed = 1.0f;
    err.err = nil;
    strcpy(err.msg, "OK");
    return err;
}

void destroyEditorUi(editorUi* ui) {
    free(ui);
}

Error activateEditorUi(editorUi* ui) {
    Error err;
    if (ui == NULL) {
        err.err = E_INVALID_ARG;
        strcpy(err.msg, "Invalid arguments");
        return err;
    }
    ui->EditorActive = true;
    err.err = nil;
    strcpy(err.msg, "OK");
    return err;
}

Error deactivateEditorUi(editorUi* ui) {
    Error err;
    if (ui == NULL) {
        err.err = E_INVALID_ARG;
        strcpy(err.msg, "Invalid arguments");
        return err;
    }
    ui->selectedObject = NULL;
    ui->mode = MOVE_MODE;
    ui->EditorActive = false;
    err.err = nil;
    strcpy(err.msg, "OK");
    return err;
}

void selectObject(editorUi* ui, Camera* cam, int px, int py) {
    
}
