/*
 * kavita.c
 *
 * Kavita integration implementation for PC.
 */

#include "kavita/kavita.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

/*
 * Trim whitespace from both ends of a string in place.
 */
static char *trim(char *str)
{
    if (!str) {
        return NULL;
    }

    while (*str == ' ' || *str == '\t' || *str == '\r' || *str == '\n') {
        str++;
    }

    char *end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
        *end = '\0';
        end--;
    }

    return str;
}

/*
 * Remove surrounding quotes from a value, if present.
 */
static char *unquote(char *str)
{
    size_t len = strlen(str);
    if (len >= 2 && str[0] == '"' && str[len - 1] == '"') {
        str[len - 1] = '\0';
        return str + 1;
    }
    return str;
}

int kavita_config_load(const char *path, kavita_config_t *cfg)
{
    if (!path || !cfg) {
        return 0;
    }

    memset(cfg, 0, sizeof(*cfg));

    FILE *file = fopen(path, "r");
    if (!file) {
        return 0;
    }

    char line[1024];
    while (fgets(line, sizeof(line), file)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }

        /* Skip comments and section headers. */
        if (*p == '\0' || *p == '#' || *p == ';' || *p == '[') {
            continue;
        }

        char *eq = strchr(p, '=');
        if (!eq) {
            continue;
        }

        *eq = '\0';
        char *key = trim(p);
        char *value = unquote(trim(eq + 1));

        if (strcmp(key, "url") == 0) {
            free(cfg->url);
            cfg->url = strdup(value);
        } else if (strcmp(key, "api_key") == 0) {
            free(cfg->api_key);
            cfg->api_key = strdup(value);
        }
    }

    fclose(file);
    return cfg->url != NULL && cfg->api_key != NULL;
}

void kavita_config_free(kavita_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    free(cfg->url);
    free(cfg->api_key);
    cfg->url = NULL;
    cfg->api_key = NULL;
}

char *kavita_meta_path(const char *epub_path)
{
    if (!epub_path) {
        return NULL;
    }

    size_t len = strlen(epub_path) + strlen(".meta.json") + 1;
    char *path = malloc(len);
    if (!path) {
        return NULL;
    }

    snprintf(path, len, "%s.meta.json", epub_path);
    return path;
}

static int parse_chapter_ids(cJSON *array, kavita_chapter_map_t *chapters)
{
    chapters->count = 0;
    chapters->ids = NULL;

    if (!array || !cJSON_IsArray(array)) {
        return 1;  /* No chapter mapping is acceptable. */
    }

    int count = cJSON_GetArraySize(array);
    if (count <= 0) {
        return 1;
    }

    chapters->ids = malloc((size_t)count * sizeof(int));
    if (!chapters->ids) {
        return 0;
    }

    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(array, i);
        chapters->ids[i] = cJSON_IsNumber(item) ? item->valueint : 0;
    }
    chapters->count = count;
    return 1;
}

int kavita_meta_load(const char *epub_path, kavita_meta_t *meta)
{
    if (!epub_path || !meta) {
        return 0;
    }

    memset(meta, 0, sizeof(*meta));

    char *path = kavita_meta_path(epub_path);
    if (!path) {
        return 0;
    }

    FILE *file = fopen(path, "r");
    free(path);
    if (!file) {
        return 0;
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    char *buffer = malloc((size_t)size + 1);
    if (!buffer) {
        fclose(file);
        return 0;
    }

    fread(buffer, 1, (size_t)size, file);
    buffer[size] = '\0';
    fclose(file);

    cJSON *root = cJSON_Parse(buffer);
    free(buffer);
    if (!root) {
        return 0;
    }

    cJSON *volume_id = cJSON_GetObjectItem(root, "kavita_volume_id");
    cJSON *series_id = cJSON_GetObjectItem(root, "kavita_series_id");
    cJSON *title = cJSON_GetObjectItem(root, "title");
    cJSON *chapter_ids = cJSON_GetObjectItem(root, "chapter_ids");

    meta->kavita_volume_id = cJSON_IsNumber(volume_id) ? volume_id->valueint : 0;
    meta->kavita_series_id = cJSON_IsNumber(series_id) ? series_id->valueint : 0;

    if (cJSON_IsString(title)) {
        meta->title = strdup(title->valuestring);
    }

    if (!parse_chapter_ids(chapter_ids, &meta->chapters)) {
        kavita_meta_free(meta);
        cJSON_Delete(root);
        return 0;
    }

    meta->downloaded_at = time(NULL);
    (void)meta->downloaded_at;

    cJSON_Delete(root);
    return 1;
}

int kavita_meta_save(const char *epub_path, const kavita_meta_t *meta)
{
    if (!epub_path || !meta) {
        return 0;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return 0;
    }

    cJSON_AddNumberToObject(root, "kavita_volume_id", meta->kavita_volume_id);
    cJSON_AddNumberToObject(root, "kavita_series_id", meta->kavita_series_id);

    if (meta->title) {
        cJSON_AddStringToObject(root, "title", meta->title);
    } else {
        cJSON_AddNullToObject(root, "title");
    }

    cJSON *chapter_ids = cJSON_AddArrayToObject(root, "chapter_ids");
    for (int i = 0; i < meta->chapters.count; i++) {
        cJSON_AddItemToArray(chapter_ids, cJSON_CreateNumber(meta->chapters.ids[i]));
    }

    char *path = kavita_meta_path(epub_path);
    if (!path) {
        cJSON_Delete(root);
        return 0;
    }

    FILE *file = fopen(path, "w");
    free(path);
    if (!file) {
        cJSON_Delete(root);
        return 0;
    }

    char *json = cJSON_Print(root);
    if (json) {
        fputs(json, file);
        free(json);
    }

    fclose(file);
    cJSON_Delete(root);
    return json != NULL;
}

void kavita_meta_free(kavita_meta_t *meta)
{
    if (!meta) {
        return;
    }
    free(meta->title);
    free(meta->chapters.ids);
    memset(meta, 0, sizeof(*meta));
}

struct kavita_client {
    kavita_config_t config;
};

kavita_client_t *kavita_client_new(const kavita_config_t *cfg)
{
    if (!cfg || !cfg->url || !cfg->api_key) {
        return NULL;
    }

    kavita_client_t *client = calloc(1, sizeof(*client));
    if (!client) {
        return NULL;
    }

    client->config.url = strdup(cfg->url);
    client->config.api_key = strdup(cfg->api_key);
    if (!client->config.url || !client->config.api_key) {
        kavita_client_free(client);
        return NULL;
    }

    return client;
}

void kavita_client_free(kavita_client_t *client)
{
    if (!client) {
        return;
    }
    kavita_config_free(&client->config);
    free(client);
}

const kavita_config_t *kavita_client_config(const kavita_client_t *client)
{
    return client ? &client->config : NULL;
}

int kavita_client_ping(kavita_client_t *client)
{
    (void)client;
    /* TODO: implement actual ping once HTTP backend is ready. */
    return 0;
}
