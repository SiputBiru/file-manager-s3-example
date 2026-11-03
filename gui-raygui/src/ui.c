#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "raygui.h"
#include "raylib.h"
#include "tinyfiledialogs.h"

#include "image_cache.h"

#define WINDOW_WIDTH 1100
#define WINDOW_HEIGHT 700

#define LEFT_X 20
#define PANEL_Y 80
#define LEFT_WIDTH 310
#define RIGHT_X 350
#define RIGHT_WIDTH 730
#define PANEL_HEIGHT 590

#define CARD_WIDTH 220
#define CARD_HEIGHT 160
#define CARD_GAP 14
#define GRID_COLUMNS 3

static void draw_thumbnail(const FileItem *item, Rectangle bounds) {
    Texture2D texture = {0};

    if (cache_get_texture(item->id, &texture) && texture.id != 0) {
        float scale = fminf(bounds.width / (float)texture.width,
                            bounds.height / (float)texture.height);
        float width = texture.width * scale;
        float height = texture.height * scale;

        Rectangle source = {0, 0, (float)texture.width, (float)texture.height};
        Rectangle destination = {
            bounds.x + (bounds.width - width) / 2.0f,
            bounds.y + (bounds.height - height) / 2.0f,
            width,
            height,
        };

        DrawTexturePro(texture, source, destination, (Vector2){0, 0}, 0.0f, WHITE);
    } else {
        DrawRectangleRec(bounds, (Color){230, 232, 236, 255});
        DrawText("loading...", (int)bounds.x + 8, (int)bounds.y + 8, 12, GRAY);
    }
}

static void draw_grid(App *app) {
    Rectangle panel = {RIGHT_X, PANEL_Y, RIGHT_WIDTH, PANEL_HEIGHT};
    DrawRectangleRec(panel, (Color){250, 250, 252, 255});
    DrawRectangleLinesEx(panel, 1, (Color){210, 214, 220, 255});

    int count = (int)app->files.count;
    int rows = (count + GRID_COLUMNS - 1) / GRID_COLUMNS;
    int content_height = rows * (CARD_HEIGHT + CARD_GAP);
    int max_scroll = content_height - (int)panel.height + CARD_GAP;
    if (max_scroll < 0) {
        max_scroll = 0;
    }

    Vector2 mouse = GetMousePosition();
    if (CheckCollisionPointRec(mouse, panel)) {
        app->scroll -= (int)(GetMouseWheelMove() * 40.0f);
    }
    if (app->scroll < 0) {
        app->scroll = 0;
    }
    if (app->scroll > max_scroll) {
        app->scroll = max_scroll;
    }

    BeginScissorMode((int)panel.x, (int)panel.y, (int)panel.width, (int)panel.height);

    for (int index = 0; index < count; index++) {
        FileItem *item = &app->files.items[index];

        cache_request(item->id, item->thumb_url, item->width, item->height);

        int column = index % GRID_COLUMNS;
        int row = index / GRID_COLUMNS;

        Rectangle card = {
            panel.x + 20 + column * (CARD_WIDTH + CARD_GAP),
            panel.y + 18 + row * (CARD_HEIGHT + CARD_GAP) - app->scroll,
            CARD_WIDTH,
            CARD_HEIGHT,
        };

        if (card.y + card.height < panel.y || card.y > panel.y + panel.height) {
            continue;
        }

        bool selected = (index == app->selected);

        DrawRectangleRec(card, selected ? (Color){232, 242, 255, 255} : (Color){255, 255, 255, 255});
        DrawRectangleLinesEx(card, selected ? 2.0f : 1.0f,
                             selected ? (Color){52, 120, 246, 255} : (Color){214, 218, 224, 255});

        Rectangle thumb = {card.x + 10, card.y + 10, card.width - 20, 110};
        draw_thumbnail(item, thumb);

        const char *name = TextSubtext(item->name, 0, 26);
        DrawText(name, (int)card.x + 10, (int)card.y + 126, 14, (Color){30, 34, 40, 255});

        if (item->width > 0 && item->height > 0) {
            DrawText(TextFormat("%d x %d", item->width, item->height),
                     (int)card.x + 10, (int)card.y + 143, 12, (Color){110, 116, 126, 255});
        } else {
            DrawText("not edited", (int)card.x + 10, (int)card.y + 143, 12,
                     (Color){110, 116, 126, 255});
        }

        if (CheckCollisionPointRec(mouse, card) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            app->selected = index;
        }
    }

    EndScissorMode();

    if (count == 0) {
        DrawText("No files yet.", (int)panel.x + 24, (int)panel.y + 24, 16, GRAY);
    }
}

static void draw_left_panel(App *app) {
    Rectangle drop_zone = {LEFT_X, PANEL_Y, LEFT_WIDTH, 190};

    DrawRectangleRec(drop_zone, (Color){250, 250, 252, 255});
    DrawRectangleLinesEx(drop_zone, 1, (Color){200, 205, 214, 255});

    DrawText("Drop an image here", LEFT_X + 20, PANEL_Y + 24, 16, (Color){70, 76, 86, 255});
    DrawText("JPEG, PNG, or WebP", LEFT_X + 20, PANEL_Y + 46, 12, GRAY);

    if (app->drop_path[0]) {
        const char *name = GetFileName(app->drop_path);
        DrawText(TextSubtext(name, 0, 32), LEFT_X + 20, PANEL_Y + 96, 14,
                 (Color){30, 34, 40, 255});
        DrawText(TextFormat("%d KB", (int)(GetFileLength(app->drop_path) / 1024)),
                 LEFT_X + 20, PANEL_Y + 116, 12, GRAY);
    }

    if (GuiButton((Rectangle){LEFT_X, PANEL_Y + 204, 148, 32}, "Browse...")) {
        const char *filters[] = {"*.png", "*.jpg", "*.jpeg", "*.webp"};
        const char *selected = tinyfd_openFileDialog(
            "Choose an image", "", 4, filters, "Images", 0);
        if (selected) {
            snprintf(app->drop_path, sizeof(app->drop_path), "%s", selected);
        }
    }

    const char *upload_text = app->busy ? "Working..." : "Upload";
    if (GuiButton((Rectangle){LEFT_X + 160, PANEL_Y + 204, 150, 32}, upload_text)) {
        if (!app->busy && app->drop_path[0]) {
            app->action = ACTION_UPLOAD;
        }
    }

    DrawText("Resize width", LEFT_X, PANEL_Y + 260, 14, (Color){70, 76, 86, 255});
    static bool resize_edit = false;
    if (GuiValueBox((Rectangle){LEFT_X, PANEL_Y + 280, 138, 30}, NULL, &app->resize_width,
                    100, 4000, resize_edit)) {
        resize_edit = !resize_edit;
    }
    DrawText("px", LEFT_X + 146, PANEL_Y + 288, 12, GRAY);
    if (GuiButton((Rectangle){LEFT_X + 160, PANEL_Y + 280, 150, 30}, "Apply resize")) {
        if (!app->busy && app->selected >= 0 && app->selected < (int)app->files.count) {
            snprintf(app->action_id, sizeof(app->action_id), "%s",
                     app->files.items[app->selected].id);
            app->action = ACTION_EDIT_RESIZE;
        }
    }

    DrawText("Square crop size", LEFT_X, PANEL_Y + 330, 14, (Color){70, 76, 86, 255});
    static bool crop_edit = false;
    if (GuiValueBox((Rectangle){LEFT_X, PANEL_Y + 350, 138, 30}, NULL, &app->crop_size,
                    100, 2000, crop_edit)) {
        crop_edit = !crop_edit;
    }
    DrawText("px", LEFT_X + 146, PANEL_Y + 358, 12, GRAY);
    if (GuiButton((Rectangle){LEFT_X + 160, PANEL_Y + 350, 150, 30}, "Apply crop")) {
        if (!app->busy && app->selected >= 0 && app->selected < (int)app->files.count) {
            snprintf(app->action_id, sizeof(app->action_id), "%s",
                     app->files.items[app->selected].id);
            app->action = ACTION_EDIT_CROP;
        }
    }

    if (GuiButton((Rectangle){LEFT_X, PANEL_Y + 400, LEFT_WIDTH, 32}, "Save selected image")) {
        if (!app->busy && app->selected >= 0 && app->selected < (int)app->files.count) {
            FileItem *item = &app->files.items[app->selected];

            char base_name[256];
            snprintf(base_name, sizeof(base_name), "%s", item->name);
            char *dot = strrchr(base_name, '.');
            if (dot) {
                *dot = '\0';
            }

            const char *extension = strstr(item->display_url, "/derived/") ? ".jpg" : ".jpg";
            if (!strstr(item->display_url, "/derived/")) {
                const char *original_dot = strrchr(item->name, '.');
                if (original_dot) {
                    extension = original_dot;
                }
            }

            char default_name[300];
            snprintf(default_name, sizeof(default_name), "%s-edited%s", base_name, extension);

            const char *path = tinyfd_saveFileDialog("Save image", default_name, 0, NULL, NULL);
            if (path) {
                char save_path[1024];
                snprintf(save_path, sizeof(save_path), "%s", path);
                if (!strrchr(GetFileName(save_path), '.')) {
                    strncat(save_path, extension, sizeof(save_path) - strlen(save_path) - 1);
                }
                snprintf(app->download_path, sizeof(app->download_path), "%s", save_path);
                snprintf(app->download_url, sizeof(app->download_url), "%s", item->display_url);
                app->action = ACTION_DOWNLOAD;
            }
        }
    }

    if (GuiButton((Rectangle){LEFT_X, PANEL_Y + 450, LEFT_WIDTH, 32}, "Delete selected")) {
        if (!app->busy && app->selected >= 0 && app->selected < (int)app->files.count) {
            app->confirm_delete = true;
        }
    }

    if (app->confirm_delete) {
        int active = -1;
        Rectangle box = {LEFT_X + 10, PANEL_Y + 220, 290, 130};
        int result = GuiMessageBox(box, "Delete file", "Delete the selected file?",
                                   "Yes;No", &active);
        if (result >= 0) {
            if (active == 0) {
                snprintf(app->action_id, sizeof(app->action_id), "%s",
                         app->files.items[app->selected].id);
                app->action = ACTION_DELETE;
            }
            app->confirm_delete = false;
        }
    }
}

void ui_draw(App *app) {
    ClearBackground((Color){244, 246, 249, 255});

    DrawText("File Manager S3", 20, 20, 22, (Color){24, 28, 34, 255});

    DrawText("Backend", 20, 54, 12, GRAY);
    static bool backend_edit = false;
    if (GuiTextBox((Rectangle){80, 48, 430, 28}, app->backend, sizeof(app->backend), backend_edit)) {
        backend_edit = !backend_edit;
    }

    DrawText("User", 530, 54, 12, GRAY);
    static bool user_edit = false;
    if (GuiTextBox((Rectangle){570, 48, 190, 28}, app->user, sizeof(app->user), user_edit)) {
        user_edit = !user_edit;
    }

    if (GuiButton((Rectangle){780, 48, 120, 28}, app->busy ? "Waiting..." : "Refresh")) {
        if (!app->busy) {
            app->action = ACTION_REFRESH;
        }
    }

    draw_grid(app);
    draw_left_panel(app);

    DrawRectangle(0, WINDOW_HEIGHT - 30, WINDOW_WIDTH, 30, (Color){232, 235, 240, 255});
    DrawText(app->status[0] ? app->status : "Ready", 20, WINDOW_HEIGHT - 22, 14,
             (Color){60, 66, 76, 255});
}
