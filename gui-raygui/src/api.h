#ifndef API_H
#define API_H

#include <stddef.h>

typedef struct {
    char id[64];
    char name[256];
    int width;
    int height;
    long size;
    char display_url[2048];
    char thumb_url[2048];
} FileItem;

typedef struct {
    FileItem *items;
    size_t count;
} FileList;

void api_global_init(void);
void api_global_cleanup(void);

void file_list_free(FileList *list);

int api_upload_file(const char *base, const char *user, const char *path,
                    char *out_id, size_t out_id_len,
                    char *err, size_t err_len);

int api_list_files(const char *base, const char *user, FileList *out,
                   char *err, size_t err_len);

int api_edit_file(const char *base, const char *user, const char *id,
                  const char *mode, int value,
                  char *err, size_t err_len);

int api_delete_file(const char *base, const char *user, const char *id,
                    char *err, size_t err_len);

int api_download(const char *url, unsigned char **data, size_t *len,
                 char *err, size_t err_len);

int api_download_to_file(const char *url, const char *path,
                         char *err, size_t err_len);

#endif
