#ifndef IMAGE_CACHE_H
#define IMAGE_CACHE_H

#include <stdbool.h>

#include "raylib.h"

void cache_init(void);
void cache_shutdown(void);
void cache_clear(void);
void cache_invalidate(const char *id);
void cache_request(const char *id, const char *url, int width, int height);
void cache_update_textures(void);
bool cache_get_texture(const char *id, Texture2D *out);

#endif
