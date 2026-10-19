/*
 * kavita.c
 *
 * Kavita integration implementation for PC.
 */

#include "kavita/kavita.h"

#include <curl/curl.h>
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
    CURL *curl;
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

    client->curl = curl_easy_init();
    if (!client->curl) {
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
    if (client->curl) {
        curl_easy_cleanup(client->curl);
    }
    free(client);
}

const kavita_config_t *kavita_client_config(const kavita_client_t *client)
{
    return client ? &client->config : NULL;
}

/*
 * Build a full URL from the client's base URL and a relative path.
 * Caller must free the returned string.
 */
static char *build_url(const kavita_client_t *client, const char *path)
{
    if (!client || !client->config.url || !path) {
        return NULL;
    }

    const char *base = client->config.url;
    size_t base_len = strlen(base);
    int needs_slash = (base_len > 0 && base[base_len - 1] != '/' && path[0] != '/');

    size_t len = base_len + strlen(path) + (needs_slash ? 1 : 0) + 1;
    char *url = malloc(len);
    if (!url) {
        return NULL;
    }

    if (needs_slash) {
        snprintf(url, len, "%s/%s", base, path);
    } else {
        snprintf(url, len, "%s%s", base, path);
    }

    return url;
}

/*
 * Callback for curl to append response data to a buffer.
 */
static size_t write_memory_cb(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t total = size * nmemb;
    char **buffer = (char **)userp;

    size_t existing = *buffer ? strlen(*buffer) : 0;
    char *new_buffer = realloc(*buffer, existing + total + 1);
    if (!new_buffer) {
        return 0;
    }

    *buffer = new_buffer;
    memcpy(*buffer + existing, contents, total);
    (*buffer)[existing + total] = '\0';

    return total;
}

/*
 * Perform an HTTP GET and return the body.
 *
 * Returns 1 on HTTP 200, 0 otherwise. The body is returned in *body, which
 * must be freed by the caller (may be set even on failure).
 */
static int http_get(kavita_client_t *client, const char *path, char **body, long *status_code)
{
    if (!client || !client->curl || !path || !body || !status_code) {
        return 0;
    }

    *body = NULL;
    *status_code = 0;

    char *url = build_url(client, path);
    if (!url) {
        return 0;
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Accept: application/atom+xml,application/xml;q=0.9,*/*;q=0.8");

    curl_easy_setopt(client->curl, CURLOPT_URL, url);
    curl_easy_setopt(client->curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(client->curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(client->curl, CURLOPT_WRITEFUNCTION, write_memory_cb);
    curl_easy_setopt(client->curl, CURLOPT_WRITEDATA, body);
    curl_easy_setopt(client->curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(client->curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(client->curl, CURLOPT_USERAGENT, "Inky/0.1");

    CURLcode res = curl_easy_perform(client->curl);

    curl_slist_free_all(headers);
    free(url);

    if (res != CURLE_OK) {
        free(*body);
        *body = NULL;
        return 0;
    }

    curl_easy_getinfo(client->curl, CURLINFO_RESPONSE_CODE, status_code);

    if (*status_code != 200) {
        free(*body);
        *body = NULL;
        return 0;
    }

    return 1;
}

int kavita_client_ping(kavita_client_t *client)
{
    if (!client || !client->curl) {
        return 0;
    }

    /*
     * The OPDS catalogue endpoint is the lightest way to confirm that the
     * server is reachable and the API key is accepted.
     */
    char path[512];
    snprintf(path, sizeof(path), "/api/Opds/%s", client->config.api_key);

    char *body = NULL;
    long status_code = 0;
    int ok = http_get(client, path, &body, &status_code);
    free(body);

    return ok;
}

/*
 * Decode the most common XML entities in place into the same buffer.
 * Returns the new length of the string.
 */
static size_t xml_decode_entities(char *str)
{
    if (!str) {
        return 0;
    }

    size_t len = strlen(str);
    size_t out = 0;

    for (size_t i = 0; i < len; i++) {
        if (str[i] == '&') {
            if (strncmp(&str[i], "&lt;", 4) == 0) {
                str[out++] = '<';
                i += 3;
            } else if (strncmp(&str[i], "&gt;", 4) == 0) {
                str[out++] = '>';
                i += 3;
            } else if (strncmp(&str[i], "&amp;", 5) == 0) {
                str[out++] = '&';
                i += 4;
            } else if (strncmp(&str[i], "&quot;", 6) == 0) {
                str[out++] = '"';
                i += 5;
            } else if (strncmp(&str[i], "&apos;", 6) == 0) {
                str[out++] = '\'';
                i += 5;
            } else {
                str[out++] = str[i];
            }
        } else {
            str[out++] = str[i];
        }
    }

    str[out] = '\0';
    return out;
}

/*
 * Extract the value of an attribute from an XML start tag.
 *
 * `tag` must point at the '<' character. The returned string is heap-allocated
 * and decoded. Returns NULL if the attribute is not found.
 */
static char *xml_attr_value(const char *tag, const char *attr_name)
{
    if (!tag || tag[0] != '<' || !attr_name) {
        return NULL;
    }

    size_t attr_len = strlen(attr_name);
    const char *p = tag + 1;

    while (*p && *p != '>') {
        /* Skip leading whitespace. */
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            p++;
        }

        if (strncmp(p, attr_name, attr_len) == 0 &&
            (p[attr_len] == '=' || p[attr_len] == ' ' || p[attr_len] == '\t')) {
            p += attr_len;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            if (*p != '=') {
                break;
            }
            p++;
            while (*p == ' ' || *p == '\t') {
                p++;
            }

            char quote = *p;
            if (quote != '"' && quote != '\'') {
                break;
            }
            p++;

            const char *start = p;
            while (*p && *p != quote) {
                p++;
            }

            size_t len = (size_t)(p - start);
            char *value = malloc(len + 1);
            if (!value) {
                return NULL;
            }
            memcpy(value, start, len);
            value[len] = '\0';
            xml_decode_entities(value);
            return value;
        }

        /* Skip to next attribute. */
        while (*p && *p != ' ' && *p != '\t' && *p != '>') {
            p++;
        }
    }

    return NULL;
}

/*
 * Extract text content between `<tag>` and `</tag>`.
 *
 * `start` is advanced to just after the closing tag. The returned string is
 * heap-allocated and decoded. Returns NULL if not found.
 */
static char *xml_element_text(const char **start, const char *tag_name)
{
    if (!start || !*start || !tag_name) {
        return NULL;
    }

    size_t tag_len = strlen(tag_name);
    char open[64];
    char close[64];

    if (tag_len + 3 >= sizeof(open)) {
        return NULL;
    }

    snprintf(open, sizeof(open), "<%s", tag_name);
    snprintf(close, sizeof(close), "</%s>", tag_name);

    const char *p = strstr(*start, open);
    if (!p) {
        return NULL;
    }

    /* Skip the opening tag. */
    const char *tag_end = strchr(p, '>');
    if (!tag_end) {
        return NULL;
    }
    p = tag_end + 1;

    const char *end = strstr(p, close);
    if (!end) {
        return NULL;
    }

    size_t len = (size_t)(end - p);
    char *text = malloc(len + 1);
    if (!text) {
        return NULL;
    }
    memcpy(text, p, len);
    text[len] = '\0';
    xml_decode_entities(text);

    *start = end + strlen(close);
    return text;
}

/*
 * Find the next `<entry>` block in the XML feed.
 *
 * `cursor` is advanced to just after the opening `<entry>` tag. Returns a
 * pointer to the closing `</entry>` tag, or NULL if no more entries.
 */
static const char *xml_next_entry(const char **cursor)
{
    if (!cursor || !*cursor) {
        return NULL;
    }

    const char *start = strstr(*cursor, "<entry");
    if (!start) {
        return NULL;
    }

    const char *tag_end = strchr(start, '>');
    if (!tag_end) {
        return NULL;
    }

    const char *close = strstr(tag_end, "</entry>");
    if (!close) {
        return NULL;
    }

    *cursor = tag_end + 1;
    return close;
}

/*
 * Find the next `<link>` tag within the range [start, end).
 *
 * `cursor` is advanced to just after the found `<link` start. Returns a
 * pointer to the closing '>' of the link tag, or NULL if no more links.
 */
static const char *xml_next_link(const char **cursor, const char *end)
{
    if (!cursor || !*cursor || !end || *cursor >= end) {
        return NULL;
    }

    const char *start = strstr(*cursor, "<link");
    if (!start || start >= end) {
        return NULL;
    }

    const char *tag_end = strchr(start, '>');
    if (!tag_end || tag_end >= end) {
        return NULL;
    }

    *cursor = start;
    return tag_end;
}

/*
 * Find the last decimal integer segment in a path, looking for a marker that
 * precedes it. Returns 0 if not found.
 */
static int extract_id_after_marker(const char *path, const char *marker)
{
    if (!path || !marker) {
        return 0;
    }

    const char *p = strstr(path, marker);
    if (!p) {
        return 0;
    }
    p += strlen(marker);

    while (*p && (*p < '0' || *p > '9')) {
        p++;
    }

    if (!*p) {
        return 0;
    }

    return (int)strtol(p, NULL, 10);
}

/*
 * Extract the last integer segment from a path. Returns 0 if none.
 */
static int extract_last_id(const char *path)
{
    if (!path) {
        return 0;
    }

    int id = 0;
    const char *p = path;
    while (*p) {
        if (*p >= '0' && *p <= '9') {
            id = (int)strtol(p, NULL, 10);
            while (*p >= '0' && *p <= '9') {
                p++;
            }
        } else {
            p++;
        }
    }

    return id;
}

/*
 * Strip Kavita's leading status indicator (a circle or similar glyph) and any
 * surrounding whitespace from an entry title.
 *
 * Kavita prefixes unread entries with a symbol such as U+2B58 (⭘). The symbol
 * is a UTF-8 multi-byte codepoint followed by a space. We strip any leading
 * non-ASCII symbol that is immediately followed by whitespace, which preserves
 * real titles that start with accented letters (e.g. "Émile") because those
 * are not followed by a space.
 */
static void strip_leading_indicator(char *title)
{
    if (!title) {
        return;
    }

    const char *p = title;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }

    if ((unsigned char)*p >= 0x80) {
        const char *next = p;

        /* Advance past one UTF-8 code point. */
        if (((unsigned char)*next & 0xe0) == 0xc0) {
            next += 2;
        } else if (((unsigned char)*next & 0xf0) == 0xe0) {
            next += 3;
        } else if (((unsigned char)*next & 0xf8) == 0xf0) {
            next += 4;
        } else {
            next++;
        }

        /* Only strip if the symbol is followed by whitespace. */
        if (*next == ' ' || *next == '\t') {
            p = next;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
        }
    }

    if (p != title) {
        memmove(title, p, strlen(p) + 1);
    }
}

/*
 * Parse a single OPDS entry into title, navigation link, and acquisition link.
 */
typedef struct {
    char *title;
    char *nav_link;   /* rel="subsection" or "alternate" */
    char *acq_link;   /* rel="http://opds-spec.org/acquisition" */
} opds_entry_t;

static void opds_entry_free(opds_entry_t *entry)
{
    if (!entry) {
        return;
    }
    free(entry->title);
    free(entry->nav_link);
    free(entry->acq_link);
    memset(entry, 0, sizeof(*entry));
}

static int parse_opds_entry(const char *start, const char *end, opds_entry_t *entry)
{
    if (!start || !end || !entry) {
        return 0;
    }

    memset(entry, 0, sizeof(*entry));

    const char *title_cursor = start;
    entry->title = xml_element_text(&title_cursor, "title");
    if (!entry->title) {
        entry->title = strdup("");
        if (!entry->title) {
            return 0;
        }
    }
    strip_leading_indicator(entry->title);

    const char *link_cursor = start;
    const char *link_end;
    while ((link_end = xml_next_link(&link_cursor, end)) != NULL) {
        char *rel = xml_attr_value(link_cursor, "rel");
        char *href = xml_attr_value(link_cursor, "href");

        if (rel && href) {
            if (strstr(rel, "acquisition") != NULL) {
                free(entry->acq_link);
                entry->acq_link = href;
                href = NULL;
            } else if (strcmp(rel, "subsection") == 0 || strcmp(rel, "alternate") == 0) {
                if (!entry->nav_link) {
                    entry->nav_link = href;
                    href = NULL;
                }
            }
        }

        free(rel);
        free(href);
        link_cursor = link_end + 1;
    }

    return 1;
}

/*
 * Parse a feed into an array of OPDS entries.
 *
 * Returns the number of entries, or -1 on error. The caller must free each
 * entry with opds_entry_free() and the array itself with free().
 */
static int parse_opds_feed(const char *xml, opds_entry_t **entries)
{
    if (!xml || !entries) {
        return -1;
    }

    *entries = NULL;

    int capacity = 16;
    int count = 0;
    opds_entry_t *list = calloc(capacity, sizeof(opds_entry_t));
    if (!list) {
        return -1;
    }

    const char *cursor = xml;
    const char *entry_end;

    while ((entry_end = xml_next_entry(&cursor)) != NULL) {
        if (count >= capacity) {
            int new_capacity = capacity * 2;
            opds_entry_t *new_list = realloc(list, new_capacity * sizeof(opds_entry_t));
            if (!new_list) {
                for (int i = 0; i < count; i++) {
                    opds_entry_free(&list[i]);
                }
                free(list);
                return -1;
            }
            list = new_list;
            capacity = new_capacity;
        }

        if (!parse_opds_entry(cursor, entry_end, &list[count])) {
            for (int i = 0; i < count; i++) {
                opds_entry_free(&list[i]);
            }
            free(list);
            return -1;
        }

        count++;
        cursor = entry_end + strlen("</entry>");
    }

    *entries = list;
    return count;
}

/*
 * Return 1 if the OPDS feed contains a <link rel="next">, indicating that
 * more pages are available.
 */
static int opds_has_next_link(const char *xml)
{
    if (!xml) {
        return 0;
    }

    const char *cursor = xml;
    const char *xml_end = xml + strlen(xml);
    const char *end;
    while ((end = xml_next_link(&cursor, xml_end)) != NULL) {
        char *rel = xml_attr_value(cursor, "rel");
        if (rel && strcmp(rel, "next") == 0) {
            free(rel);
            return 1;
        }
        free(rel);
        cursor = end + 1;
    }

    return 0;
}

int kavita_client_fetch_libraries(kavita_client_t *client,
                                  kavita_library_t **libraries)
{
    if (!client || !libraries) {
        return -1;
    }

    *libraries = NULL;

    char path[512];
    snprintf(path, sizeof(path), "/api/Opds/%s/libraries", client->config.api_key);

    char *body = NULL;
    long status_code = 0;
    if (!http_get(client, path, &body, &status_code)) {
        return -1;
    }

    opds_entry_t *entries = NULL;
    int count = parse_opds_feed(body, &entries);
    free(body);

    if (count < 0) {
        return -1;
    }

    kavita_library_t *libs = calloc(count, sizeof(kavita_library_t));
    if (!libs) {
        for (int i = 0; i < count; i++) {
            opds_entry_free(&entries[i]);
        }
        free(entries);
        return -1;
    }

    for (int i = 0; i < count; i++) {
        libs[i].name = entries[i].title;
        entries[i].title = NULL;
        libs[i].library_id = extract_last_id(entries[i].nav_link);
        opds_entry_free(&entries[i]);
    }
    free(entries);

    *libraries = libs;
    return count;
}

int kavita_client_fetch_series(kavita_client_t *client, int library_id,
                               kavita_series_t **series)
{
    if (!client || library_id <= 0 || !series) {
        return -1;
    }

    *series = NULL;

    int capacity = 32;
    int total = 0;
    kavita_series_t *s = calloc(capacity, sizeof(kavita_series_t));
    if (!s) {
        return -1;
    }

    int page = 0;
    while (1) {
        char path[512];
        snprintf(path, sizeof(path), "/api/Opds/%s/libraries/%d?pageNumber=%d",
                 client->config.api_key, library_id, page);

        char *body = NULL;
        long status_code = 0;
        if (!http_get(client, path, &body, &status_code)) {
            kavita_series_free(s, total);
            return -1;
        }

        opds_entry_t *entries = NULL;
        int count = parse_opds_feed(body, &entries);

        if (count < 0) {
            free(body);
            kavita_series_free(s, total);
            return -1;
        }

        if (count == 0) {
            for (int i = 0; i < count; i++) {
                opds_entry_free(&entries[i]);
            }
            free(entries);
            free(body);
            break;
        }

        if (total + count > capacity) {
            int new_capacity = capacity;
            while (new_capacity < total + count) {
                new_capacity *= 2;
            }
            kavita_series_t *new_s = realloc(s, new_capacity * sizeof(kavita_series_t));
            if (!new_s) {
                for (int i = 0; i < count; i++) {
                    opds_entry_free(&entries[i]);
                }
                free(entries);
                kavita_series_free(s, total);
                return -1;
            }
            s = new_s;
            capacity = new_capacity;
        }

        for (int i = 0; i < count; i++) {
            s[total + i].name = entries[i].title;
            entries[i].title = NULL;
            s[total + i].series_id = extract_last_id(entries[i].nav_link);
            opds_entry_free(&entries[i]);
        }
        free(entries);

        total += count;

        int has_next = opds_has_next_link(body);
        free(body);

        if (!has_next) {
            break;
        }
        page++;
    }

    *series = s;
    return total;
}

int kavita_client_fetch_volumes(kavita_client_t *client, int series_id,
                                kavita_volume_t **volumes)
{
    if (!client || series_id <= 0 || !volumes) {
        return -1;
    }

    *volumes = NULL;

    int capacity = 16;
    int total = 0;
    kavita_volume_t *v = calloc(capacity, sizeof(kavita_volume_t));
    if (!v) {
        return -1;
    }

    int page = 0;
    while (1) {
        char path[512];
        snprintf(path, sizeof(path), "/api/Opds/%s/series/%d?pageNumber=%d",
                 client->config.api_key, series_id, page);

        char *body = NULL;
        long status_code = 0;
        if (!http_get(client, path, &body, &status_code)) {
            kavita_volumes_free(v, total);
            return -1;
        }

        opds_entry_t *entries = NULL;
        int count = parse_opds_feed(body, &entries);

        if (count < 0) {
            free(body);
            kavita_volumes_free(v, total);
            return -1;
        }

        if (count == 0) {
            for (int i = 0; i < count; i++) {
                opds_entry_free(&entries[i]);
            }
            free(entries);
            free(body);
            break;
        }

        if (total + count > capacity) {
            int new_capacity = capacity;
            while (new_capacity < total + count) {
                new_capacity *= 2;
            }
            kavita_volume_t *new_v = realloc(v, new_capacity * sizeof(kavita_volume_t));
            if (!new_v) {
                for (int i = 0; i < count; i++) {
                    opds_entry_free(&entries[i]);
                }
                free(entries);
                free(body);
                kavita_volumes_free(v, total);
                return -1;
            }
            v = new_v;
            capacity = new_capacity;
        }

        for (int i = 0; i < count; i++) {
            v[total + i].title = entries[i].title;
            entries[i].title = NULL;

            if (entries[i].acq_link) {
                v[total + i].download_url = strdup(entries[i].acq_link);
                v[total + i].volume_id = extract_id_after_marker(entries[i].acq_link, "/volume/");
                v[total + i].chapter_id = extract_id_after_marker(entries[i].acq_link, "/chapter/");
            }

            opds_entry_free(&entries[i]);
        }
        free(entries);

        total += count;

        int has_next = opds_has_next_link(body);
        free(body);

        if (!has_next) {
            break;
        }
        page++;
    }

    *volumes = v;
    return total;
}

void kavita_libraries_free(kavita_library_t *libraries, int count)
{
    if (!libraries) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(libraries[i].name);
    }
    free(libraries);
}

void kavita_series_free(kavita_series_t *series, int count)
{
    if (!series) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(series[i].name);
    }
    free(series);
}

void kavita_volumes_free(kavita_volume_t *volumes, int count)
{
    if (!volumes) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(volumes[i].title);
        free(volumes[i].download_url);
    }
    free(volumes);
}

/*
 * Callback for curl to stream download data to a file.
 */
typedef struct {
    FILE *file;
    size_t received;
    size_t total;
    kavita_download_progress_t progress;
    void *user_data;
} download_ctx_t;

static size_t write_file_cb(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t total = size * nmemb;
    download_ctx_t *ctx = (download_ctx_t *)userp;

    if (fwrite(contents, 1, total, ctx->file) != total) {
        return 0;
    }

    ctx->received += total;
    if (ctx->progress) {
        ctx->progress(ctx->received, ctx->total, ctx->user_data);
    }

    return total;
}

static int progress_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow)
{
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;

    download_ctx_t *ctx = (download_ctx_t *)clientp;
    if (dltotal > 0) {
        ctx->total = (size_t)dltotal;
    }
    return 0;
}

int kavita_client_download_volume(kavita_client_t *client,
                                  const kavita_volume_t *volume,
                                  const char *out_path,
                                  kavita_download_progress_t progress,
                                  void *user_data)
{
    if (!client || !client->curl || !volume || !volume->download_url || !out_path) {
        return 0;
    }

    char *url = build_url(client, volume->download_url);
    if (!url) {
        return 0;
    }

    FILE *file = fopen(out_path, "wb");
    if (!file) {
        free(url);
        return 0;
    }

    download_ctx_t ctx = {
        .file = file,
        .received = 0,
        .total = 0,
        .progress = progress,
        .user_data = user_data,
    };

    curl_easy_setopt(client->curl, CURLOPT_URL, url);
    curl_easy_setopt(client->curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(client->curl, CURLOPT_HTTPHEADER, NULL);
    curl_easy_setopt(client->curl, CURLOPT_WRITEFUNCTION, write_file_cb);
    curl_easy_setopt(client->curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(client->curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(client->curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(client->curl, CURLOPT_USERAGENT, "Inky/0.1");
    curl_easy_setopt(client->curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(client->curl, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(client->curl, CURLOPT_NOPROGRESS, 0L);

    CURLcode res = curl_easy_perform(client->curl);

    fclose(file);
    free(url);

    if (res != CURLE_OK) {
        remove(out_path);
        return 0;
    }

    long status_code = 0;
    curl_easy_getinfo(client->curl, CURLINFO_RESPONSE_CODE, &status_code);

    if (status_code != 200) {
        remove(out_path);
        return 0;
    }

    return 1;
}
