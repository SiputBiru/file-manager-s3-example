#ifndef UI_H
#define UI_H

#include <stdbool.h>

#include "api.h"

typedef enum {
    ACTION_NONE = 0,
    ACTION_REFRESH,
    ACTION_UPLOAD,
    ACTION_EDIT_RESIZE,
    ACTION_EDIT_CROP,
    ACTION_DOWNLOAD,
    ACTION_DELETE
} AppAction;

typedef struct {
    char backend[256];
    char user[128];
    char status[512];
    FileList files;
    int selected;
    int resize_width;
    int crop_size;
    int scroll;
    bool busy;
    bool confirm_delete;
    char drop_path[1024];
    AppAction action;
    char action_id[64];
    char download_url[4096];
    char download_path[1024];
} App;

void ui_draw(App *app);

#endif
