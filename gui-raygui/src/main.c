#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "raylib.h"

#include "api.h"
#include "image_cache.h"
#include "ui.h"

typedef struct {
    AppAction type;
    char id[64];
    int value;
    char path[1024];
    char url[4096];
    char save_path[1024];
} Job;

typedef struct {
    App app;
    Job job;
    FileList result_files;
    char result_status[512];
    int result_code;
    bool result_has_files;
    bool job_done;
    char invalidate_id[64];
    pthread_mutex_t lock;
    pthread_t thread;
    bool thread_started;
} Runtime;

static void *job_main(void *arg) {
    Runtime *runtime = (Runtime *)arg;
    char error[512] = {0};
    char status[512] = {0};
    char success_status[512] = {0};
    FileList files = {0};
    int code = -1;
    bool list_loaded = false;

    switch (runtime->job.type) {
        case ACTION_REFRESH:
            code = api_list_files(runtime->app.backend, runtime->app.user, &files,
                                  error, sizeof(error));
            if (code == 0) {
                list_loaded = true;
            }
            break;

        case ACTION_UPLOAD:
            code = api_upload_file(runtime->app.backend, runtime->app.user,
                                   runtime->job.path, NULL, 0, error, sizeof(error));
            if (code == 0) {
                code = api_list_files(runtime->app.backend, runtime->app.user, &files,
                                      error, sizeof(error));
                if (code == 0) {
                    list_loaded = true;
                }
            }
            break;

        case ACTION_EDIT_RESIZE:
            code = api_edit_file(runtime->app.backend, runtime->app.user, runtime->job.id,
                                 "resize", runtime->job.value, error, sizeof(error));
            if (code == 0) {
                snprintf(runtime->invalidate_id, sizeof(runtime->invalidate_id), "%s",
                         runtime->job.id);
                code = api_list_files(runtime->app.backend, runtime->app.user, &files,
                                      error, sizeof(error));
                if (code == 0) {
                    list_loaded = true;
                }
            }
            break;

        case ACTION_EDIT_CROP:
            code = api_edit_file(runtime->app.backend, runtime->app.user, runtime->job.id,
                                 "square", runtime->job.value, error, sizeof(error));
            if (code == 0) {
                snprintf(runtime->invalidate_id, sizeof(runtime->invalidate_id), "%s",
                         runtime->job.id);
                code = api_list_files(runtime->app.backend, runtime->app.user, &files,
                                      error, sizeof(error));
                if (code == 0) {
                    list_loaded = true;
                }
            }
            break;

        case ACTION_DOWNLOAD: {
            int download_code = api_download_to_file(runtime->job.url, runtime->job.save_path,
                                                     error, sizeof(error));
            int list_code = api_list_files(runtime->app.backend, runtime->app.user, &files,
                                           error, sizeof(error));
            if (list_code == 0) {
                list_loaded = true;
            }
            if (download_code == 0) {
                snprintf(success_status, sizeof(success_status), "Saved to %s",
                         runtime->job.save_path);
                code = list_code;
            } else {
                code = download_code;
            }
            break;
        }

        case ACTION_DELETE:
            code = api_delete_file(runtime->app.backend, runtime->app.user, runtime->job.id,
                                   error, sizeof(error));
            if (code == 0) {
                snprintf(runtime->invalidate_id, sizeof(runtime->invalidate_id), "%s",
                         runtime->job.id);
                code = api_list_files(runtime->app.backend, runtime->app.user, &files,
                                      error, sizeof(error));
                if (code == 0) {
                    list_loaded = true;
                }
            }
            break;

        default:
            snprintf(error, sizeof(error), "Unknown action");
            break;
    }

    if (code == 0) {
        snprintf(status, sizeof(status), "%s", success_status[0] ? success_status : "Ready");
    } else {
        snprintf(status, sizeof(status), "%s", error[0] ? error : "Request failed");
    }

    pthread_mutex_lock(&runtime->lock);
    runtime->result_files = files;
    snprintf(runtime->result_status, sizeof(runtime->result_status), "%s", status);
    runtime->result_code = code;
    runtime->result_has_files = list_loaded;
    runtime->job_done = true;
    pthread_mutex_unlock(&runtime->lock);

    return NULL;
}

static void start_job(Runtime *runtime, AppAction type, const char *id, int value,
                      const char *path, const char *url, const char *save_path) {
    if (runtime->app.busy) {
        return;
    }

    runtime->app.busy = true;
    runtime->app.action = ACTION_NONE;

    runtime->job.type = type;
    runtime->job.id[0] = '\0';
    runtime->job.value = value;
    runtime->job.path[0] = '\0';
    runtime->job.url[0] = '\0';
    runtime->job.save_path[0] = '\0';

    if (id) {
        snprintf(runtime->job.id, sizeof(runtime->job.id), "%s", id);
    }
    if (path) {
        snprintf(runtime->job.path, sizeof(runtime->job.path), "%s", path);
    }
    if (url) {
        snprintf(runtime->job.url, sizeof(runtime->job.url), "%s", url);
    }
    if (save_path) {
        snprintf(runtime->job.save_path, sizeof(runtime->job.save_path), "%s", save_path);
    }

    pthread_mutex_lock(&runtime->lock);
    runtime->job_done = false;
    pthread_mutex_unlock(&runtime->lock);

    if (pthread_create(&runtime->thread, NULL, job_main, runtime) == 0) {
        runtime->thread_started = true;
    } else {
        runtime->app.busy = false;
        snprintf(runtime->app.status, sizeof(runtime->app.status), "Failed to start worker");
    }
}

static void poll_job(Runtime *runtime) {
    if (!runtime->thread_started) {
        return;
    }

    pthread_mutex_lock(&runtime->lock);
    bool done = runtime->job_done;
    pthread_mutex_unlock(&runtime->lock);

    if (!done) {
        return;
    }

    pthread_join(runtime->thread, NULL);
    runtime->thread_started = false;

    if (runtime->result_has_files) {
        file_list_free(&runtime->app.files);
        runtime->app.files = runtime->result_files;
        runtime->result_files = (FileList){0};
    } else {
        file_list_free(&runtime->result_files);
    }

    snprintf(runtime->app.status, sizeof(runtime->app.status), "%s", runtime->result_status);
    runtime->app.busy = false;

    if (runtime->invalidate_id[0]) {
        cache_invalidate(runtime->invalidate_id);
        runtime->invalidate_id[0] = '\0';
    }

    if (runtime->result_has_files && runtime->app.selected >= (int)runtime->app.files.count) {
        runtime->app.selected = (int)runtime->app.files.count - 1;
    }

    runtime->job_done = false;
}

static void handle_action(Runtime *runtime) {
    AppAction action = runtime->app.action;
    if (action == ACTION_NONE) {
        return;
    }

    switch (action) {
        case ACTION_REFRESH:
            start_job(runtime, ACTION_REFRESH, NULL, 0, NULL, NULL, NULL);
            break;
        case ACTION_UPLOAD:
            start_job(runtime, ACTION_UPLOAD, NULL, 0, runtime->app.drop_path, NULL, NULL);
            break;
        case ACTION_EDIT_RESIZE:
            start_job(runtime, ACTION_EDIT_RESIZE, runtime->app.action_id,
                      runtime->app.resize_width, NULL, NULL, NULL);
            break;
        case ACTION_EDIT_CROP:
            start_job(runtime, ACTION_EDIT_CROP, runtime->app.action_id,
                      runtime->app.crop_size, NULL, NULL, NULL);
            break;
        case ACTION_DOWNLOAD:
            start_job(runtime, ACTION_DOWNLOAD, runtime->app.action_id, 0, NULL,
                      runtime->app.download_url, runtime->app.download_path);
            break;
        case ACTION_DELETE:
            start_job(runtime, ACTION_DELETE, runtime->app.action_id, 0, NULL, NULL, NULL);
            break;
        default:
            break;
    }

    runtime->app.action = ACTION_NONE;
}

static int run_selftest(const char *base, const char *user, const char *path) {
    char error[512] = {0};
    FileList list = {0};

    if (!path) {
        fprintf(stderr, "usage: gui-raygui --selftest <image>\n");
        return 2;
    }

    printf("base: %s\n", base);
    printf("user: %s\n", user);
    printf("image: %s\n", path);

    if (api_list_files(base, user, &list, error, sizeof(error)) != 0) {
        fprintf(stderr, "initial list failed: %s\n", error);
        return 1;
    }
    printf("initial files: %zu\n", list.count);
    file_list_free(&list);

    char id[64] = {0};
    if (api_upload_file(base, user, path, id, sizeof(id), error, sizeof(error)) != 0) {
        fprintf(stderr, "upload failed: %s\n", error);
        return 1;
    }
    printf("uploaded: %s\n", id);

    if (api_edit_file(base, user, id, "resize", 320, error, sizeof(error)) != 0) {
        fprintf(stderr, "resize failed: %s\n", error);
        return 1;
    }

    if (api_list_files(base, user, &list, error, sizeof(error)) != 0) {
        fprintf(stderr, "list after resize failed: %s\n", error);
        return 1;
    }

    int width = 0;
    int height = 0;
    char display_url[4096] = {0};
    bool found = false;
    for (size_t index = 0; index < list.count; index++) {
        if (strcmp(list.items[index].id, id) == 0) {
            width = list.items[index].width;
            height = list.items[index].height;
            snprintf(display_url, sizeof(display_url), "%s", list.items[index].display_url);
            found = true;
            break;
        }
    }
    file_list_free(&list);

    if (!found || width != 320) {
        fprintf(stderr, "resize check failed: width=%d height=%d\n", width, height);
        return 1;
    }
    printf("after resize: %d x %d\n", width, height);

    if (display_url[0]) {
        const char *download_path = "/tmp/gui-selftest-download.jpg";
        if (api_download_to_file(display_url, download_path, error, sizeof(error)) != 0) {
            fprintf(stderr, "download failed: %s\n", error);
            return 1;
        }

        struct stat info;
        if (stat(download_path, &info) != 0 || info.st_size <= 0) {
            fprintf(stderr, "download check failed\n");
            return 1;
        }
        printf("downloaded: %ld bytes\n", (long)info.st_size);
        remove(download_path);
    }

    if (api_edit_file(base, user, id, "square", 200, error, sizeof(error)) != 0) {
        fprintf(stderr, "crop failed: %s\n", error);
        return 1;
    }

    if (api_list_files(base, user, &list, error, sizeof(error)) != 0) {
        fprintf(stderr, "list after crop failed: %s\n", error);
        return 1;
    }

    int crop_width = 0;
    int crop_height = 0;
    for (size_t index = 0; index < list.count; index++) {
        if (strcmp(list.items[index].id, id) == 0) {
            crop_width = list.items[index].width;
            crop_height = list.items[index].height;
            break;
        }
    }
    file_list_free(&list);

    if (crop_width != 200 || crop_height != 200) {
        fprintf(stderr, "crop check failed: %d x %d\n", crop_width, crop_height);
        return 1;
    }
    printf("after crop: %d x %d\n", crop_width, crop_height);

    if (api_delete_file(base, user, id, error, sizeof(error)) != 0) {
        fprintf(stderr, "delete failed: %s\n", error);
        return 1;
    }

    if (api_list_files(base, user, &list, error, sizeof(error)) != 0) {
        fprintf(stderr, "list after delete failed: %s\n", error);
        return 1;
    }

    bool still_there = false;
    for (size_t index = 0; index < list.count; index++) {
        if (strcmp(list.items[index].id, id) == 0) {
            still_there = true;
            break;
        }
    }
    file_list_free(&list);

    if (still_there) {
        fprintf(stderr, "delete check failed: file still listed\n");
        return 1;
    }

    printf("selftest: PASS\n");
    return 0;
}

int main(int argc, char **argv) {
    const char *base = getenv("S3_API");
    const char *user = getenv("S3_USER");
    if (!base) {
        base = "http://localhost:3000";
    }
    if (!user) {
        user = "demo-user";
    }

    if (argc >= 2 && strcmp(argv[1], "--selftest") == 0) {
        api_global_init();
        int result = run_selftest(base, user, argc >= 3 ? argv[2] : NULL);
        api_global_cleanup();
        return result;
    }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_HIGHDPI);
    InitWindow(1100, 700, "File Manager S3 - raygui");
    SetTargetFPS(60);
    TraceLog(LOG_INFO, "screen=%dx%d render=%dx%d dpi=%.1fx%.1f",
             GetScreenWidth(), GetScreenHeight(), GetRenderWidth(), GetRenderHeight(),
             GetWindowScaleDPI().x, GetWindowScaleDPI().y);

    api_global_init();
    cache_init();

    Runtime runtime;
    memset(&runtime, 0, sizeof(runtime));
    pthread_mutex_init(&runtime.lock, NULL);

    snprintf(runtime.app.backend, sizeof(runtime.app.backend), "%s", base);
    snprintf(runtime.app.user, sizeof(runtime.app.user), "%s", user);
    snprintf(runtime.app.status, sizeof(runtime.app.status), "Ready");
    runtime.app.selected = -1;
    runtime.app.resize_width = 800;
    runtime.app.crop_size = 400;
    runtime.app.action = ACTION_REFRESH;

    while (!WindowShouldClose()) {
        if (IsFileDropped()) {
            FilePathList dropped = LoadDroppedFiles();
            if (dropped.count > 0) {
                snprintf(runtime.app.drop_path, sizeof(runtime.app.drop_path), "%s",
                         dropped.paths[0]);
            }
            UnloadDroppedFiles(dropped);
        }

        poll_job(&runtime);
        handle_action(&runtime);
        cache_update_textures();

        BeginDrawing();
        ui_draw(&runtime.app);
        EndDrawing();
    }

    if (runtime.thread_started) {
        pthread_join(runtime.thread, NULL);
        file_list_free(&runtime.result_files);
    }

    file_list_free(&runtime.app.files);
    cache_shutdown();
    api_global_cleanup();
    pthread_mutex_destroy(&runtime.lock);
    CloseWindow();

    return 0;
}
