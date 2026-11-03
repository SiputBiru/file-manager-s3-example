#include "image_cache.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"
#include "stb_image.h"

#define MAX_ENTRIES 128
#define MAX_QUEUE 128
#define URL_SIZE 4096

typedef struct {
    char id[64];
    char url[URL_SIZE];
    unsigned char *bytes;
    size_t bytes_len;
    Texture2D texture;
    bool has_texture;
    bool has_bytes;
    bool failed;
    bool queued;
    int width;
    int height;
    double last_used;
} Entry;

static Entry entries[MAX_ENTRIES];
static int entry_count;

static int queue[MAX_QUEUE];
static int queue_len;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static pthread_t worker;
static bool worker_started;
static bool stopping;

static int find_entry(const char *id) {
    for (int index = 0; index < entry_count; index++) {
        if (strcmp(entries[index].id, id) == 0) {
            return index;
        }
    }
    return -1;
}

static void enqueue(int index) {
    if (queue_len >= MAX_QUEUE || entries[index].queued) {
        return;
    }
    queue[queue_len++] = index;
    entries[index].queued = true;
    pthread_cond_signal(&cond);
}

static void *worker_main(void *arg) {
    (void)arg;

    for (;;) {
        pthread_mutex_lock(&lock);

        while (queue_len == 0 && !stopping) {
            pthread_cond_wait(&cond, &lock);
        }

        if (stopping) {
            pthread_mutex_unlock(&lock);
            break;
        }

        int index = queue[0];
        for (int i = 1; i < queue_len; i++) {
            queue[i - 1] = queue[i];
        }
        queue_len--;

        char id[64];
        char url[URL_SIZE];
        snprintf(id, sizeof(id), "%s", entries[index].id);
        snprintf(url, sizeof(url), "%s", entries[index].url);

        pthread_mutex_unlock(&lock);

        unsigned char *data = NULL;
        size_t data_len = 0;
        char error[256] = {0};
        int result = api_download(url, &data, &data_len, error, sizeof(error));

        pthread_mutex_lock(&lock);

        int current = find_entry(id);
        if (current >= 0) {
            entries[current].queued = false;
            if (result == 0) {
                free(entries[current].bytes);
                entries[current].bytes = data;
                entries[current].bytes_len = data_len;
                entries[current].has_bytes = true;
                entries[current].failed = false;
                TraceLog(LOG_INFO, "THUMB %s downloaded %d bytes", id, (int)data_len);
            } else {
                entries[current].failed = true;
                TraceLog(LOG_WARNING, "THUMB %s download failed: %s", id, error);
                free(data);
            }
        } else {
            free(data);
        }

        pthread_mutex_unlock(&lock);
    }

    return NULL;
}

void cache_init(void) {
    entry_count = 0;
    queue_len = 0;
    stopping = false;

    memset(entries, 0, sizeof(entries));

    if (!worker_started) {
        if (pthread_create(&worker, NULL, worker_main, NULL) == 0) {
            worker_started = true;
        }
    }
}

void cache_shutdown(void) {
    if (worker_started) {
        pthread_mutex_lock(&lock);
        stopping = true;
        pthread_cond_signal(&cond);
        pthread_mutex_unlock(&lock);
        pthread_join(worker, NULL);
        worker_started = false;
    }

    cache_clear();
}

void cache_clear(void) {
    pthread_mutex_lock(&lock);

    for (int index = 0; index < entry_count; index++) {
        if (entries[index].has_texture) {
            UnloadTexture(entries[index].texture);
        }
        free(entries[index].bytes);
    }

    memset(entries, 0, sizeof(entries));
    entry_count = 0;
    queue_len = 0;

    pthread_mutex_unlock(&lock);
}

void cache_invalidate(const char *id) {
    pthread_mutex_lock(&lock);

    int index = find_entry(id);
    if (index >= 0) {
        if (entries[index].has_texture) {
            UnloadTexture(entries[index].texture);
        }
        free(entries[index].bytes);

        entries[index] = entries[entry_count - 1];
        memset(&entries[entry_count - 1], 0, sizeof(Entry));
        entry_count--;

        queue_len = 0;
    }

    pthread_mutex_unlock(&lock);
}

void cache_request(const char *id, const char *url, int width, int height) {
    if (!id || !id[0] || !url || !url[0]) {
        return;
    }

    pthread_mutex_lock(&lock);

    int index = find_entry(id);
    if (index < 0) {
        if (entry_count >= MAX_ENTRIES) {
            pthread_mutex_unlock(&lock);
            return;
        }
        index = entry_count++;
        memset(&entries[index], 0, sizeof(Entry));
        snprintf(entries[index].id, sizeof(entries[index].id), "%s", id);
    }

    snprintf(entries[index].url, sizeof(entries[index].url), "%s", url);
    entries[index].width = width;
    entries[index].height = height;
    entries[index].last_used = GetTime();

    if (!entries[index].has_texture && !entries[index].has_bytes && !entries[index].failed) {
        enqueue(index);
    }

    pthread_mutex_unlock(&lock);
}

void cache_update_textures(void) {
    pthread_mutex_lock(&lock);

    for (int index = 0; index < entry_count; index++) {
        Entry *entry = &entries[index];
        if (!entry->has_bytes || entry->has_texture) {
            continue;
        }

        int width = 0;
        int height = 0;
        int channels = 0;
        unsigned char *pixels = stbi_load_from_memory(
            entry->bytes, (int)entry->bytes_len, &width, &height, &channels, 4);

        TraceLog(LOG_INFO, "THUMB %s decode len=%d %dx%d", entry->id,
                 (int)entry->bytes_len, width, height);

        if (pixels && width > 0 && height > 0) {
            Image image = {0};
            image.data = pixels;
            image.width = width;
            image.height = height;
            image.mipmaps = 1;
            image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;

            Texture2D texture = LoadTextureFromImage(image);
            if (texture.id != 0) {
                entry->texture = texture;
                entry->has_texture = true;
            }

            stbi_image_free(pixels);
        } else {
            entry->failed = true;
        }

        free(entry->bytes);
        entry->bytes = NULL;
        entry->bytes_len = 0;
        entry->has_bytes = false;
    }

    pthread_mutex_unlock(&lock);
}

bool cache_get_texture(const char *id, Texture2D *out) {
    bool found = false;

    pthread_mutex_lock(&lock);

    int index = find_entry(id);
    if (index >= 0 && entries[index].has_texture) {
        *out = entries[index].texture;
        entries[index].last_used = GetTime();
        found = true;
    }

    pthread_mutex_unlock(&lock);
    return found;
}
