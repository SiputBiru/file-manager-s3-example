#include "api.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "cJSON.h"

#define MAX_UPLOAD_BYTES (15 * 1024 * 1024)
#define URL_SIZE 4096
#define ERROR_SIZE 512

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Buffer;

static void buffer_init(Buffer *buffer) {
    buffer->data = NULL;
    buffer->len = 0;
    buffer->cap = 0;
}

static void buffer_free(Buffer *buffer) {
    free(buffer->data);
    buffer->data = NULL;
    buffer->len = 0;
    buffer->cap = 0;
}

static int buffer_reserve(Buffer *buffer, size_t extra) {
    size_t needed = buffer->len + extra + 1;
    if (needed <= buffer->cap) {
        return 0;
    }

    size_t cap = buffer->cap ? buffer->cap : 4096;
    while (cap < needed) {
        cap *= 2;
    }

    char *data = realloc(buffer->data, cap);
    if (!data) {
        return -1;
    }

    buffer->data = data;
    buffer->cap = cap;
    return 0;
}

static size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    Buffer *buffer = (Buffer *)userdata;

    if (buffer_reserve(buffer, total) != 0) {
        return 0;
    }

    memcpy(buffer->data + buffer->len, ptr, total);
    buffer->len += total;
    buffer->data[buffer->len] = '\0';
    return total;
}

void api_global_init(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

void api_global_cleanup(void) {
    curl_global_cleanup();
}

static const char *effective_user(const char *user) {
    return (user && user[0]) ? user : "demo-user";
}

static void join_url(const char *base, const char *path, char *out, size_t out_len) {
    size_t length = strlen(base);
    while (length > 0 && base[length - 1] == '/') {
        length--;
    }
    snprintf(out, out_len, "%.*s%s", (int)length, base, path);
}

static struct curl_slist *json_headers(const char *user) {
    struct curl_slist *headers = NULL;
    char line[256];

    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "Content-Type: application/json");
    snprintf(line, sizeof(line), "x-user-id: %s", effective_user(user));
    headers = curl_slist_append(headers, line);
    return headers;
}

static int http_perform(const char *method, const char *url,
                        struct curl_slist *headers,
                        const void *body, size_t body_len,
                        Buffer *response, long *status,
                        char *err, size_t err_len) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(err, err_len, "Failed to initialize curl");
        return -1;
    }

    buffer_init(response);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "gui-raygui/1.0");

    if (body && body_len > 0) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body_len);
    }

    CURLcode result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
        snprintf(err, err_len, "%s", curl_easy_strerror(result));
        curl_easy_cleanup(curl);
        buffer_free(response);
        return -1;
    }

    *status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
    curl_easy_cleanup(curl);
    return 0;
}

static int check_response(Buffer *response, long status, char *err, size_t err_len) {
    if (status >= 200 && status < 300) {
        return 0;
    }

    char message[ERROR_SIZE] = {0};
    if (response->data) {
        cJSON *root = cJSON_Parse(response->data);
        if (root) {
            cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
            if (cJSON_IsString(error) && error->valuestring) {
                snprintf(message, sizeof(message), "%s", error->valuestring);
            }
            cJSON_Delete(root);
        }
    }

    if (message[0]) {
        snprintf(err, err_len, "%s", message);
    } else {
        snprintf(err, err_len, "HTTP %ld", status);
    }
    return -1;
}

static void copy_json_string(const cJSON *object, const char *key, char *out, size_t out_len) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsString(item) && item->valuestring) {
        snprintf(out, out_len, "%s", item->valuestring);
    } else {
        out[0] = '\0';
    }
}

static int copy_json_int(const cJSON *object, const char *key) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsNumber(item)) {
        return item->valueint;
    }
    return 0;
}

static long copy_json_long(const cJSON *object, const char *key) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsNumber(item)) {
        return (long)item->valuedouble;
    }
    return 0;
}

static const char *content_type_for(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) {
        return NULL;
    }
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) {
        return "image/jpeg";
    }
    if (strcasecmp(dot, ".png") == 0) {
        return "image/png";
    }
    if (strcasecmp(dot, ".webp") == 0) {
        return "image/webp";
    }
    return NULL;
}

static const char *file_name_of(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int read_file(const char *path, unsigned char **data, size_t *len) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        return -1;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -1;
    }

    long size = ftell(file);
    if (size < 0 || size > MAX_UPLOAD_BYTES) {
        fclose(file);
        return -1;
    }

    rewind(file);

    unsigned char *buffer = malloc((size_t)size + 1);
    if (!buffer) {
        fclose(file);
        return -1;
    }

    size_t read = fread(buffer, 1, (size_t)size, file);
    fclose(file);

    if (read != (size_t)size) {
        free(buffer);
        return -1;
    }

    *data = buffer;
    *len = read;
    return 0;
}

void file_list_free(FileList *list) {
    if (!list) {
        return;
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

int api_list_files(const char *base, const char *user, FileList *out,
                   char *err, size_t err_len) {
    char url[URL_SIZE];
    join_url(base, "/api/files", url, sizeof(url));

    Buffer response;
    long status = 0;
    struct curl_slist *headers = json_headers(user);

    int result = http_perform("GET", url, headers, NULL, 0, &response, &status, err, err_len);
    curl_slist_free_all(headers);

    if (result != 0) {
        return result;
    }

    if (check_response(&response, status, err, err_len) != 0) {
        buffer_free(&response);
        return -1;
    }

    cJSON *root = cJSON_Parse(response.data ? response.data : "[]");
    buffer_free(&response);

    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        snprintf(err, err_len, "Invalid file list response");
        return -1;
    }

    int count = cJSON_GetArraySize(root);
    FileItem *items = count > 0 ? calloc((size_t)count, sizeof(FileItem)) : NULL;
    if (count > 0 && !items) {
        cJSON_Delete(root);
        snprintf(err, err_len, "Out of memory");
        return -1;
    }

    for (int index = 0; index < count; index++) {
        cJSON *entry = cJSON_GetArrayItem(root, index);
        FileItem *item = &items[index];

        copy_json_string(entry, "id", item->id, sizeof(item->id));
        copy_json_string(entry, "name", item->name, sizeof(item->name));
        copy_json_string(entry, "displayUrl", item->display_url, sizeof(item->display_url));
        copy_json_string(entry, "thumbUrl", item->thumb_url, sizeof(item->thumb_url));
        item->width = copy_json_int(entry, "width");
        item->height = copy_json_int(entry, "height");
        item->size = copy_json_long(entry, "size");
    }

    cJSON_Delete(root);

    out->items = items;
    out->count = (size_t)count;
    return 0;
}

int api_upload_file(const char *base, const char *user, const char *path,
                    char *out_id, size_t out_id_len,
                    char *err, size_t err_len) {
    const char *content_type = content_type_for(path);
    if (!content_type) {
        snprintf(err, err_len, "Only JPEG, PNG, and WebP files are supported");
        return -1;
    }

    unsigned char *data = NULL;
    size_t data_len = 0;
    if (read_file(path, &data, &data_len) != 0) {
        snprintf(err, err_len, "Cannot read file or file exceeds 15 MB");
        return -1;
    }

    const char *name = file_name_of(path);
    char url[URL_SIZE];
    Buffer response;
    long status = 0;

    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "name", name);
    cJSON_AddStringToObject(request, "contentType", content_type);
    cJSON_AddNumberToObject(request, "size", (double)data_len);
    char *body = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);

    join_url(base, "/api/files/upload-url", url, sizeof(url));
    struct curl_slist *headers = json_headers(user);
    int result = http_perform("POST", url, headers, body, strlen(body), &response, &status, err, err_len);
    curl_slist_free_all(headers);
    cJSON_free(body);

    if (result != 0) {
        free(data);
        return result;
    }

    if (check_response(&response, status, err, err_len) != 0) {
        buffer_free(&response);
        free(data);
        return -1;
    }

    cJSON *root = cJSON_Parse(response.data ? response.data : "{}");
    buffer_free(&response);
    if (!root) {
        free(data);
        snprintf(err, err_len, "Invalid upload response");
        return -1;
    }

    char file_id[64] = {0};
    char key[512] = {0};
    char upload_url[URL_SIZE] = {0};
    copy_json_string(root, "fileId", file_id, sizeof(file_id));
    copy_json_string(root, "key", key, sizeof(key));
    copy_json_string(root, "uploadUrl", upload_url, sizeof(upload_url));
    cJSON_Delete(root);

    if (!file_id[0] || !key[0] || !upload_url[0]) {
        free(data);
        snprintf(err, err_len, "Incomplete upload response");
        return -1;
    }

    struct curl_slist *put_headers = NULL;
    char content_header[128];
    snprintf(content_header, sizeof(content_header), "Content-Type: %s", content_type);
    put_headers = curl_slist_append(put_headers, content_header);
    put_headers = curl_slist_append(put_headers, "Expect:");

    result = http_perform("PUT", upload_url, put_headers, data, data_len, &response, &status, err, err_len);
    curl_slist_free_all(put_headers);
    free(data);

    if (result != 0) {
        return result;
    }

    if (check_response(&response, status, err, err_len) != 0) {
        buffer_free(&response);
        return -1;
    }
    buffer_free(&response);

    cJSON *confirm = cJSON_CreateObject();
    cJSON_AddStringToObject(confirm, "fileId", file_id);
    cJSON_AddStringToObject(confirm, "key", key);
    cJSON_AddStringToObject(confirm, "name", name);
    cJSON_AddStringToObject(confirm, "contentType", content_type);
    body = cJSON_PrintUnformatted(confirm);
    cJSON_Delete(confirm);

    join_url(base, "/api/files/confirm", url, sizeof(url));
    headers = json_headers(user);
    result = http_perform("POST", url, headers, body, strlen(body), &response, &status, err, err_len);
    curl_slist_free_all(headers);
    cJSON_free(body);

    if (result != 0) {
        return result;
    }

    if (check_response(&response, status, err, err_len) != 0) {
        buffer_free(&response);
        return -1;
    }
    buffer_free(&response);

    if (out_id && out_id_len > 0) {
        snprintf(out_id, out_id_len, "%s", file_id);
    }
    return 0;
}

int api_edit_file(const char *base, const char *user, const char *id,
                  const char *mode, int value,
                  char *err, size_t err_len) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "mode", mode);
    if (strcmp(mode, "square") == 0) {
        cJSON_AddNumberToObject(request, "size", value);
    } else {
        cJSON_AddNumberToObject(request, "width", value);
    }

    char *body = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);

    char path[256];
    snprintf(path, sizeof(path), "/api/files/%s/edit", id);
    char url[URL_SIZE];
    join_url(base, path, url, sizeof(url));

    Buffer response;
    long status = 0;
    struct curl_slist *headers = json_headers(user);
    int result = http_perform("POST", url, headers, body, strlen(body), &response, &status, err, err_len);
    curl_slist_free_all(headers);
    cJSON_free(body);

    if (result != 0) {
        return result;
    }

    result = check_response(&response, status, err, err_len);
    buffer_free(&response);
    return result;
}

int api_delete_file(const char *base, const char *user, const char *id,
                    char *err, size_t err_len) {
    char path[256];
    snprintf(path, sizeof(path), "/api/files/%s", id);
    char url[URL_SIZE];
    join_url(base, path, url, sizeof(url));

    Buffer response;
    long status = 0;
    struct curl_slist *headers = json_headers(user);
    int result = http_perform("DELETE", url, headers, NULL, 0, &response, &status, err, err_len);
    curl_slist_free_all(headers);

    if (result != 0) {
        return result;
    }

    result = check_response(&response, status, err, err_len);
    buffer_free(&response);
    return result;
}

int api_download(const char *url, unsigned char **data, size_t *len,
                 char *err, size_t err_len) {
    Buffer response;
    long status = 0;

    int result = http_perform("GET", url, NULL, NULL, 0, &response, &status, err, err_len);
    if (result != 0) {
        return result;
    }

    if (status != 200) {
        snprintf(err, err_len, "HTTP %ld", status);
        buffer_free(&response);
        return -1;
    }

    *data = (unsigned char *)response.data;
    *len = response.len;
    return 0;
}

int api_download_to_file(const char *url, const char *path,
                         char *err, size_t err_len) {
    unsigned char *data = NULL;
    size_t data_len = 0;

    if (api_download(url, &data, &data_len, err, err_len) != 0) {
        return -1;
    }

    FILE *file = fopen(path, "wb");
    if (!file) {
        free(data);
        snprintf(err, err_len, "Cannot open output file");
        return -1;
    }

    size_t written = fwrite(data, 1, data_len, file);
    fclose(file);
    free(data);

    if (written != data_len) {
        snprintf(err, err_len, "Failed to write output file");
        return -1;
    }

    return 0;
}
