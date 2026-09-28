// noteSS offline cache implementation. See cache.h for the API.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cache.h"

#include <curl/curl.h>
#include <dirent.h>
#include <glib.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define CACHE_SUBDIR "/noteSS"
#define CACHE_SUFFIX ".txt"
#define CACHE_TEXT_MAX (64 * 1024)

static gint cmp_filenames(gconstpointer a, gconstpointer b);

// Resolve cache dir: $XDG_CACHE_HOME/noteSS, fallback to ~/.cache/noteSS.
// Caller frees the result with g_free().
static char *cache_dir_path(void) {
    const char *xdg = g_getenv("XDG_CACHE_HOME");
    char base[PATH_MAX];

    if (xdg != NULL && xdg[0] != '\0') {
        snprintf(base, sizeof(base), "%s", xdg);
    } else {
        const char *home = g_getenv("HOME");
        if (home == NULL || home[0] == '\0')
            home = g_get_home_dir();
        if (home == NULL)
            return NULL;
        snprintf(base, sizeof(base), "%s/.cache", home);
    }

    return g_strconcat(base, CACHE_SUBDIR, NULL);
}

// Create the cache dir (mode 0700) if missing. Returns 0 on success.
static int cache_ensure_dir(char *dir) {
    g_mkdir_with_parents(dir, 0700);
    chmod(dir, 0700); // enforce even if the dir already existed
    return 0;
}

int cache_save_note(const char *text) {
    if (text == NULL || text[0] == '\0')
        return -1;

    char *dir = cache_dir_path();
    if (dir == NULL)
        return -1;
    cache_ensure_dir(dir);

    // Unique name: note-<epoch>-<pid>-<counter>.txt
    static unsigned int counter = 0;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/note-%ld-%d-%u%s", dir, (long)time(NULL),
             (int)getpid(), counter++, CACHE_SUFFIX);
    g_free(dir);

    FILE *f = fopen(path, "w");
    if (f == NULL)
        return -1;
    size_t len = strlen(text);
    if (len > CACHE_TEXT_MAX)
        len = CACHE_TEXT_MAX;
    size_t written = fwrite(text, 1, len, f);
    fclose(f);
    if (written != len) {
        unlink(path);
        return -1;
    }
    chmod(path, 0600);
    return 0;
}

// Collect full paths of cached notes, sorted oldest-first (by name).
// Returns a NULL-terminated array; caller frees it with g_strfreev().
// Returns NULL when empty or on error.
static char **cache_list_files(void) {
    char *dir = cache_dir_path();
    if (dir == NULL)
        return NULL;

    DIR *d = opendir(dir);
    if (d == NULL) {
        g_free(dir);
        return NULL;
    }

    GPtrArray *arr = g_ptr_array_new();
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        size_t slen = strlen(CACHE_SUFFIX);
        if (nlen <= slen)
            continue;
        if (strcmp(ent->d_name + nlen - slen, CACHE_SUFFIX) != 0)
            continue;
        g_ptr_array_add(arr, g_build_filename(dir, ent->d_name, NULL));
    }
    closedir(d);
    g_free(dir);

    if (arr->len == 0) {
        g_ptr_array_free(arr, TRUE);
        return NULL;
    }
    g_ptr_array_sort(arr, cmp_filenames);
    g_ptr_array_add(arr, NULL);
    return (char **)g_ptr_array_free(arr, FALSE);
}

static gint cmp_filenames(gconstpointer a, gconstpointer b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

size_t cache_pending_count(void) {
    char **files = cache_list_files();
    if (files == NULL)
        return 0;
    size_t n = 0;
    while (files[n] != NULL)
        n++;
    g_strfreev(files);
    return n;
}

// Read a whole cached note (up to CACHE_TEXT_MAX). Caller frees with free().
static char *cache_read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return NULL;
    char *buf = malloc(CACHE_TEXT_MAX + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, CACHE_TEXT_MAX, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

// Escape text for embedding inside a JSON string. Caller frees the result.
static char *cache_json_escape(const char *s) {
    size_t n = strlen(s);
    size_t cap = n * 6 + 1; // worst case: every byte becomes \u00XX
    char *out = malloc(cap);
    if (out == NULL)
        return NULL;

    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':
            out[j++] = '\\';
            out[j++] = '"';
            break;
        case '\\':
            out[j++] = '\\';
            out[j++] = '\\';
            break;
        case '\n':
            out[j++] = '\\';
            out[j++] = 'n';
            break;
        case '\r':
            out[j++] = '\\';
            out[j++] = 'r';
            break;
        case '\t':
            out[j++] = '\\';
            out[j++] = 't';
            break;
        default:
            if (c < 0x20) {
                j += (size_t)snprintf(out + j, cap - j, "\\u%04x", c);
            } else {
                out[j++] = (char)c;
            }
            break;
        }
    }
    out[j] = '\0';
    return out;
}

// Discard server response body (we only care about the HTTP status).
static size_t cache_discard_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    (void)ptr;
    (void)userdata;
    return size * nmemb;
}

// POST one note. Returns TRUE on HTTP 200/201.
static gboolean cache_post_note(const char *endpoint, const char *token, const char *text) {
    char *esc = cache_json_escape(text);
    if (esc == NULL)
        return FALSE;
    char *body = g_strdup_printf("{\"content\": \"%s\"}", esc);
    free(esc);
    if (body == NULL)
        return FALSE;

    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        g_free(body);
        return FALSE;
    }

    struct curl_slist *headers = NULL;
    char *auth = g_strdup_printf("Authorization: Bearer %s", token);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth);

    curl_easy_setopt(curl, CURLOPT_URL, endpoint);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cache_discard_cb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "noteSS/1.1.0");

    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    if (rc == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    g_free(auth);
    g_free(body);

    return rc == CURLE_OK && (code == 200 || code == 201);
}

typedef struct {
    char *endpoint;
    char *token;
} FlushJob;

// Upload every cached note in order; delete each file on success.
// Failures are left in the cache for the next launch.
static void *flush_thread_func(void *arg) {
    FlushJob *job = arg;

    char **files = cache_list_files();
    if (files != NULL) {
        for (size_t i = 0; files[i] != NULL; i++) {
            char *text = cache_read_file(files[i]);
            if (text == NULL)
                continue; // unreadable file: keep it, try the next one
            if (cache_post_note(job->endpoint, job->token, text))
                unlink(files[i]); // uploaded: drop from the cache
            free(text);
        }
        g_strfreev(files);
    }

    g_free(job->endpoint);
    g_free(job->token);
    free(job);
    return NULL;
}

void cache_flush_async(const char *endpoint, const char *token) {
    if (endpoint == NULL || token == NULL)
        return;
    if (cache_pending_count() == 0)
        return;

    FlushJob *job = malloc(sizeof(*job));
    if (job == NULL)
        return;
    job->endpoint = g_strdup(endpoint);
    job->token = g_strdup(token);

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&tid, &attr, flush_thread_func, job) != 0) {
        g_free(job->endpoint);
        g_free(job->token);
        free(job);
    }
    pthread_attr_destroy(&attr);
}
