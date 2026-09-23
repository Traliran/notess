// noteSS v1.0 - ultra-light quick note capture for Memos API v1.
// Single-file GTK4 + libcurl + pthread client. KISS by design.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See the LICENSE file for details.
//
// Flow:
//   1. Load config ($XDG_CONFIG_HOME/noteSS/config.conf or ~/.config/noteSS/config.conf).
//   2. If config is missing/incomplete, show a setup window to enter URL + token.
//   3. Main window: text field + Send button. Ctrl+Enter sends, Escape closes.
//   4. POST {memos_url}/api/v1/memos runs in a background pthread (UI never blocks).
//   5. Success (200/201) closes the app, failure shows an error dialog.

#include <ctype.h>
#include <curl/curl.h>
#include <gtk/gtk.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define APP_ID "org.notess.app"
#define CONFIG_REL_PATH "/noteSS/config.conf"
#define URL_MAX 1024
#define TOKEN_MAX 2048
#define TEXT_MAX (64 * 1024)

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

typedef struct {
    char memos_url[URL_MAX];
    char access_token[TOKEN_MAX];
} AppConfig;

// Resolve config path: $XDG_CONFIG_HOME/noteSS/config.conf,
// fallback to ~/.config/noteSS/config.conf. Caller frees the result.
static char *config_file_path(void) {
    const char *xdg = g_getenv("XDG_CONFIG_HOME");
    char base[PATH_MAX];

    if (xdg != NULL && xdg[0] != '\0') {
        snprintf(base, sizeof(base), "%s", xdg);
    } else {
        const char *home = g_getenv("HOME");
        if (home == NULL || home[0] == '\0')
            home = g_get_home_dir();
        if (home == NULL)
            return NULL;
        snprintf(base, sizeof(base), "%s/.config", home);
    }

    size_t need = strlen(base) + strlen(CONFIG_REL_PATH) + 1;
    char *out = malloc(need);
    if (out == NULL)
        return NULL;
    snprintf(out, need, "%s%s", base, CONFIG_REL_PATH);
    return out;
}

// Trim leading/trailing whitespace in place. Returns pointer to first char.
static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s))
        s++;
    if (*s == '\0')
        return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end))
        *end-- = '\0';
    return s;
}

// Remove one trailing '/' from URL so endpoint join is predictable.
static void strip_trailing_slash(char *s) {
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == '/') {
        s[n - 1] = '\0';
        n--;
    }
}

// Load "key=value" file. Ignores blank lines and lines starting with '#'.
static gboolean config_load(const char *path, AppConfig *cfg) {
    cfg->memos_url[0] = '\0';
    cfg->access_token[0] = '\0';

    FILE *f = fopen(path, "r");
    if (f == NULL)
        return FALSE;

    char line[4096];
    while (fgets(line, sizeof(line), f) != NULL) {
        char *t = trim(line);
        if (t[0] == '\0' || t[0] == '#' || t[0] == ';')
            continue;
        char *eq = strchr(t, '=');
        if (eq == NULL)
            continue;
        *eq = '\0';
        char *key = trim(t);
        char *val = trim(eq + 1);

        if (strcmp(key, "memos_url") == 0) {
            snprintf(cfg->memos_url, sizeof(cfg->memos_url), "%s", val);
        } else if (strcmp(key, "access_token") == 0) {
            snprintf(cfg->access_token, sizeof(cfg->access_token), "%s", val);
        }
    }
    fclose(f);

    strip_trailing_slash(cfg->memos_url);
    return cfg->memos_url[0] != '\0' && cfg->access_token[0] != '\0';
}

// Write config with mode 0600. Creates parent dirs with mode 0700.
static gboolean config_save(const char *path, const AppConfig *cfg) {
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    chmod(dir, 0700); // enforce even if dir already existed
    g_free(dir);

    FILE *f = fopen(path, "w");
    if (f == NULL)
        return FALSE;

    fprintf(f,
            "# noteSS config (chmod 600, keep it secret)\n"
            "memos_url=%s\n"
            "access_token=%s\n",
            cfg->memos_url, cfg->access_token);
    fclose(f);
    chmod(path, 0600);
    return TRUE;
}

// ---------------------------------------------------------------------------
// JSON (manual, dependency-free)
// ---------------------------------------------------------------------------

// Escape text for embedding inside a JSON string.
// Handles: " \ control chars (\n \r \t \b \f) and \u00XX for the rest.
// Caller frees the result.
static char *json_escape(const char *s) {
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
        case '\b':
            out[j++] = '\\';
            out[j++] = 'b';
            break;
        case '\f':
            out[j++] = '\\';
            out[j++] = 'f';
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

// Build {"content": "..."} body. Caller frees the result.
static char *json_build_memo(const char *text) {
    char *esc = json_escape(text);
    if (esc == NULL)
        return NULL;
    size_t need = strlen(esc) + 32;
    char *body = malloc(need);
    if (body == NULL) {
        free(esc);
        return NULL;
    }
    snprintf(body, need, "{\"content\": \"%s\"}", esc);
    free(esc);
    return body;
}

// ---------------------------------------------------------------------------
// Network (runs in a background thread)
// ---------------------------------------------------------------------------

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} RespBuf;

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    RespBuf *buf = userdata;
    size_t n = size * nmemb;
    if (buf->len + n + 1 > buf->cap) {
        size_t ncap = (buf->cap == 0 ? 1024 : buf->cap * 2) + n;
        char *nd = realloc(buf->data, ncap);
        if (nd == NULL)
            return 0; // abort transfer, curl reports error
        buf->data = nd;
        buf->cap = ncap;
    }
    memcpy(buf->data + buf->len, ptr, n);
    buf->len += n;
    buf->data[buf->len] = '\0';
    return n;
}

// Everything the worker thread needs (owned copies).
typedef struct {
    char *endpoint;
    char *token;
    char *json_body;
    GtkWindow *window; // reffed, for the completion callback
    GtkButton *send_button; // reffed
} SendJob;

// Result passed back to the main thread via g_idle_add (single ownership).
typedef struct {
    gboolean ok;
    long http_code;
    char *message; // error text or server response
    GtkWindow *window; // reffed
    GtkButton *send_button; // reffed
} SendResult;

// Forward declaration: idle callback runs on the GTK main thread.
static gboolean on_send_done(gpointer data);

static void *send_thread_func(void *arg) {
    SendJob *job = arg;
    SendResult *res = calloc(1, sizeof(*res));
    if (res == NULL) {
        g_object_unref(job->window);
        g_object_unref(job->send_button);
        free(job->endpoint);
        free(job->token);
        free(job->json_body);
        free(job);
        return NULL;
    }
    res->window = job->window; // transfer refs
    res->send_button = job->send_button;
    res->ok = FALSE;
    res->message = NULL;

    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        res->message = g_strdup("Failed to init libcurl.");
        g_idle_add(on_send_done, res);
        free(job->endpoint);
        free(job->token);
        free(job->json_body);
        free(job);
        return NULL;
    }

    struct curl_slist *headers = NULL;
    char auth[TOKEN_MAX + 32];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", job->token);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth);

    RespBuf resp = {0};
    char errbuf[CURL_ERROR_SIZE] = {0};

    curl_easy_setopt(curl, CURLOPT_URL, job->endpoint);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, job->json_body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "noteSS/1.0");

    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    if (rc == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    res->http_code = code;

    if (rc != CURLE_OK) {
        if (errbuf[0] != '\0')
            res->message = g_strdup(errbuf);
        else
            res->message = g_strdup(curl_easy_strerror(rc));
    } else if (code == 200 || code == 201) {
        res->ok = TRUE;
    } else {
        if (resp.data != NULL && resp.len > 0)
            res->message = g_strdup_printf("Server returned HTTP %ld: %s", code, resp.data);
        else
            res->message = g_strdup_printf("Server returned HTTP %ld.", code);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(resp.data);

    free(job->endpoint);
    free(job->token);
    free(job->json_body);
    free(job);

    g_idle_add(on_send_done, res); // back to the main thread
    return NULL;
}

// Main-thread completion: close on success, show dialog on error.
static gboolean on_send_done(gpointer data) {
    SendResult *res = data;

    gtk_widget_set_sensitive(GTK_WIDGET(res->send_button), TRUE);
    gtk_button_set_label(res->send_button, "Send");

    if (res->ok) {
        GApplication *gapp = g_application_get_default();
        if (gapp != NULL)
            g_application_quit(gapp);
        else
            gtk_window_close(res->window);
    } else {
        GtkWidget *dlg = gtk_message_dialog_new(
            res->window, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
            "Failed to send note (HTTP %ld).\n%s", res->http_code,
            res->message != NULL ? res->message : "Unknown error.");
        g_signal_connect_swapped(dlg, "response", G_CALLBACK(gtk_window_destroy), dlg);
        gtk_window_present(GTK_WINDOW(dlg));
    }

    g_object_unref(res->window);
    g_object_unref(res->send_button);
    g_free(res->message);
    free(res);
    return G_SOURCE_REMOVE;
}

// ---------------------------------------------------------------------------
// UI state
// ---------------------------------------------------------------------------

typedef struct {
    GtkApplication *app;
    GtkWindow *window;
    GtkTextView *text_view;
    GtkButton *send_button;
    AppConfig cfg;
} MainUI;

// Start the background POST. Reads text, validates, disables the button.
static void submit_note(MainUI *ui) {
    GtkTextBuffer *buf = gtk_text_view_get_buffer(ui->text_view);
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buf, &start, &end);
    char *raw = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
    if (raw == NULL)
        return;

    char *text = trim(raw);
    if (text[0] == '\0') {
        gtk_widget_grab_focus(GTK_WIDGET(ui->text_view));
        g_free(raw);
        return;
    }
    if (strlen(text) > TEXT_MAX)
        text[TEXT_MAX] = '\0';

    char endpoint[URL_MAX + 32];
    snprintf(endpoint, sizeof(endpoint), "%s/api/v1/memos", ui->cfg.memos_url);

    char *json_body = json_build_memo(text);
    g_free(raw);
    if (json_body == NULL) {
        GtkWidget *dlg = gtk_message_dialog_new(ui->window, GTK_DIALOG_MODAL,
                                                GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                                                "Out of memory while building the request.");
        g_signal_connect_swapped(dlg, "response", G_CALLBACK(gtk_window_destroy), dlg);
        gtk_window_present(GTK_WINDOW(dlg));
        return;
    }

    SendJob *job = malloc(sizeof(*job));
    if (job == NULL) {
        free(json_body);
        return;
    }
    job->endpoint = g_strdup(endpoint);
    job->token = g_strdup(ui->cfg.access_token);
    job->json_body = json_body; // malloc'd, freed by the worker
    job->window = g_object_ref(ui->window);
    job->send_button = g_object_ref(ui->send_button);

    gtk_widget_set_sensitive(GTK_WIDGET(ui->send_button), FALSE);
    gtk_button_set_label(ui->send_button, "Sending...");

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&tid, &attr, send_thread_func, job) != 0) {
        // Thread spawn failed: roll back UI state and report inline.
        pthread_attr_destroy(&attr);
        g_object_unref(job->window);
        g_object_unref(job->send_button);
        g_free(job->endpoint);
        g_free(job->token);
        free(job->json_body);
        free(job);
        gtk_widget_set_sensitive(GTK_WIDGET(ui->send_button), TRUE);
        gtk_button_set_label(ui->send_button, "Send");
        GtkWidget *dlg = gtk_message_dialog_new(ui->window, GTK_DIALOG_MODAL,
                                                GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                                                "Failed to start background thread.");
        g_signal_connect_swapped(dlg, "response", G_CALLBACK(gtk_window_destroy), dlg);
        gtk_window_present(GTK_WINDOW(dlg));
        return;
    }
    pthread_attr_destroy(&attr);
}

// Key handling: Ctrl+Enter sends, Escape closes.
static gboolean on_text_key_pressed(GtkEventControllerKey *ctl, guint keyval,
                                    guint keycode, GdkModifierType state, gpointer data) {
    (void)ctl;
    (void)keycode;
    MainUI *ui = data;

    if (keyval == GDK_KEY_Escape) {
        gtk_window_close(ui->window);
        return TRUE;
    }
    if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) &&
        (state & GDK_CONTROL_MASK)) {
        submit_note(ui);
        return TRUE;
    }
    return FALSE;
}

// Build the main capture window.
static void build_main_window(GtkApplication *app, const AppConfig *cfg) {
    MainUI *ui = g_new0(MainUI, 1);
    ui->app = app;
    ui->cfg = *cfg;

    ui->window = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(ui->window, "noteSS");
    gtk_window_set_default_size(ui->window, 420, 260);
    gtk_window_set_resizable(ui->window, FALSE);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_window_set_child(ui->window, box);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_hexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(box), scroll);

    ui->text_view = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_wrap_mode(ui->text_view, GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_accepts_tab(ui->text_view, FALSE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(ui->text_view));

    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_text_key_pressed), ui);
    gtk_widget_add_controller(GTK_WIDGET(ui->text_view), key);

    // Bottom row: hint on the left, Send button on the right.
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(box), row);

    GtkWidget *hint = gtk_label_new("Ctrl+Enter to send, Esc to close");
    gtk_widget_set_opacity(hint, 0.6);
    gtk_widget_set_halign(hint, GTK_ALIGN_START);
    gtk_widget_set_hexpand(hint, TRUE);
    gtk_box_append(GTK_BOX(row), hint);

    ui->send_button = GTK_BUTTON(gtk_button_new_with_label("Send"));
    gtk_widget_set_halign(GTK_WIDGET(ui->send_button), GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(ui->send_button));
    g_signal_connect_swapped(ui->send_button, "clicked", G_CALLBACK(submit_note), ui);

    // Free UI struct when the window is destroyed.
    g_signal_connect_data(ui->window, "destroy", G_CALLBACK(g_free), ui, NULL, G_CONNECT_SWAPPED);

    gtk_window_present(ui->window);
    gtk_widget_grab_focus(GTK_WIDGET(ui->text_view));
}

// ---------------------------------------------------------------------------
// Setup window (shown when config is missing or incomplete)
// ---------------------------------------------------------------------------

typedef struct {
    GtkApplication *app;
    GtkWindow *window;
    GtkEntry *url_entry;
    GtkEntry *token_entry;
    AppConfig existing; // prefill when partially present
} SetupUI;

static void on_setup_save(GtkButton *btn, gpointer data) {
    (void)btn;
    SetupUI *su = data;

    const char *url = gtk_editable_get_text(GTK_EDITABLE(su->url_entry));
    const char *token = gtk_editable_get_text(GTK_EDITABLE(su->token_entry));

    AppConfig cfg;
    snprintf(cfg.memos_url, sizeof(cfg.memos_url), "%s", url ? url : "");
    snprintf(cfg.access_token, sizeof(cfg.access_token), "%s", token ? token : "");
    char *t = trim(cfg.memos_url);
    memmove(cfg.memos_url, t, strlen(t) + 1);
    t = trim(cfg.access_token);
    memmove(cfg.access_token, t, strlen(t) + 1);
    strip_trailing_slash(cfg.memos_url);

    if (cfg.memos_url[0] == '\0' || cfg.access_token[0] == '\0') {
        GtkWidget *dlg = gtk_message_dialog_new(su->window, GTK_DIALOG_MODAL,
                                                GTK_MESSAGE_WARNING, GTK_BUTTONS_CLOSE,
                                                "Please fill in both Memos URL and Access Token.");
        g_signal_connect_swapped(dlg, "response", G_CALLBACK(gtk_window_destroy), dlg);
        gtk_window_present(GTK_WINDOW(dlg));
        return;
    }

    char *path = config_file_path();
    if (path == NULL || !config_save(path, &cfg)) {
        GtkWidget *dlg = gtk_message_dialog_new(
            su->window, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
            "Could not write config file: %s", path != NULL ? path : "(unknown path)");
        g_signal_connect_swapped(dlg, "response", G_CALLBACK(gtk_window_destroy), dlg);
        gtk_window_present(GTK_WINDOW(dlg));
        free(path);
        return;
    }
    free(path);

    GtkApplication *app = su->app;
    gtk_window_destroy(su->window); // also frees SetupUI via "destroy" handler
    build_main_window(app, &cfg);
}

static void build_setup_window(GtkApplication *app, const AppConfig *existing) {
    SetupUI *su = g_new0(SetupUI, 1);
    su->app = app;
    if (existing != NULL)
        su->existing = *existing;

    su->window = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(su->window, "noteSS - Setup");
    gtk_window_set_default_size(su->window, 420, 220);
    gtk_window_set_resizable(su->window, FALSE);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_window_set_child(su->window, box);

    GtkWidget *info = gtk_label_new("Enter your Memos server URL and access token.\n"
                                    "They will be saved with mode 600.");
    gtk_widget_set_halign(info, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(box), info);

    GtkWidget *url_label = gtk_label_new("Memos URL (e.g. https://memos.example.com)");
    gtk_widget_set_halign(url_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(box), url_label);

    su->url_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(su->url_entry, "https://memos.example.com");
    if (existing != NULL && existing->memos_url[0] != '\0')
        gtk_editable_set_text(GTK_EDITABLE(su->url_entry), existing->memos_url);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(su->url_entry));

    GtkWidget *token_label = gtk_label_new("Access Token");
    gtk_widget_set_halign(token_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(box), token_label);

    su->token_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_visibility(su->token_entry, FALSE); // hide token input
    gtk_entry_set_placeholder_text(su->token_entry, "memos_pat_...");
    if (existing != NULL && existing->access_token[0] != '\0')
        gtk_editable_set_text(GTK_EDITABLE(su->token_entry), existing->access_token);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(su->token_entry));

    GtkWidget *save_btn = gtk_button_new_with_label("Save and Continue");
    gtk_box_append(GTK_BOX(box), save_btn);
    g_signal_connect(save_btn, "clicked", G_CALLBACK(on_setup_save), su);

    g_signal_connect_data(su->window, "destroy", G_CALLBACK(g_free), su, NULL, G_CONNECT_SWAPPED);

    // Enter in either field triggers save.
    g_signal_connect_swapped(su->url_entry, "activate", G_CALLBACK(on_setup_save), su);
    g_signal_connect_swapped(su->token_entry, "activate", G_CALLBACK(on_setup_save), su);

    gtk_window_present(su->window);
    gtk_widget_grab_focus(GTK_WIDGET(su->url_entry));
}

// ---------------------------------------------------------------------------
// App entry
// ---------------------------------------------------------------------------

static void on_activate(GtkApplication *app, gpointer data) {
    (void)data;

    char *path = config_file_path();
    AppConfig cfg;
    memset(&cfg, 0, sizeof(cfg));

    gboolean ok = (path != NULL) && config_load(path, &cfg);
    free(path);

    if (ok)
        build_main_window(app, &cfg);
    else
        build_setup_window(app, &cfg); // prefill partial values if any
}

int main(int argc, char **argv) {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    GtkApplication *app = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    curl_global_cleanup();
    return status;
}
