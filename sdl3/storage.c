/*
 * storage.c: persistent key -> string store. See storage.h.
 */
#include <stdlib.h>
#include <string.h>

#include "storage.h"

#ifdef __EMSCRIPTEN__

/* ---------------------------------------------------------------------
 * Web: localStorage, keys namespaced under "sgtpuzzles:".
 * ------------------------------------------------------------------- */
#include <emscripten.h>

EM_JS(void, js_storage_set, (const char *k, const char *v), {
    try { localStorage.setItem("sgtpuzzles:" + UTF8ToString(k), UTF8ToString(v)); }
    catch (e) {}
});

EM_JS(char *, js_storage_get, (const char *k), {
    try {
        var v = localStorage.getItem("sgtpuzzles:" + UTF8ToString(k));
        return v === null ? 0 : stringToNewUTF8(v);
    } catch (e) { return 0; }
});

EM_JS(void, js_storage_remove, (const char *k), {
    try { localStorage.removeItem("sgtpuzzles:" + UTF8ToString(k)); } catch (e) {}
});

void storage_init(void) {}
void storage_set(const char *key, const char *value) { js_storage_set(key, value); }
char *storage_get(const char *key) { return js_storage_get(key); }
void storage_remove(const char *key) { js_storage_remove(key); }

#else

/* ---------------------------------------------------------------------
 * Native: one file per key in a platform data directory.
 * ------------------------------------------------------------------- */
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define PATHSEP '\\'
#else
#include <unistd.h>
#define MKDIR(p) mkdir((p), 0755)
#define PATHSEP '/'
#endif

static char *data_dir = NULL;

static char *join(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    char *r = malloc(la + lb + 2);
    memcpy(r, a, la);
    r[la] = PATHSEP;
    memcpy(r + la + 1, b, lb);
    r[la + 1 + lb] = '\0';
    return r;
}

/* Create dir and any missing parents; ignore errors (e.g. already exists). */
static void mkdir_p(const char *path)
{
    char *tmp = malloc(strlen(path) + 1), *p;
    strcpy(tmp, path);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = '\0';
            MKDIR(tmp);
            *p = c;
        }
    }
    MKDIR(tmp);
    free(tmp);
}

void storage_init(void)
{
    const char *env;

    if (data_dir)
        return;

    if ((env = getenv("SGT_PUZZLES_DIR")) != NULL && *env) {
        data_dir = malloc(strlen(env) + 1);
        strcpy(data_dir, env);
    }
#ifdef _WIN32
    else if ((env = getenv("APPDATA")) != NULL && *env) {
        data_dir = join(env, "Puzzles");
    }
#elif defined(__APPLE__)
    else if ((env = getenv("HOME")) != NULL && *env) {
        char *support = join(env, "Library/Application Support");
        data_dir = join(support, "Puzzles");
        free(support);
    }
#else
    else if ((env = getenv("XDG_DATA_HOME")) != NULL && *env) {
        data_dir = join(env, "puzzles");
    } else if ((env = getenv("HOME")) != NULL && *env) {
        char *share = join(env, ".local/share");
        data_dir = join(share, "puzzles");
        free(share);
    }
#endif

    if (data_dir)
        mkdir_p(data_dir);
}

void storage_set(const char *key, const char *value)
{
    char *path;
    FILE *f;

    if (!data_dir)
        return;
    path = join(data_dir, key);
    if ((f = fopen(path, "wb")) != NULL) {
        fwrite(value, 1, strlen(value), f);
        fclose(f);
    }
    free(path);
}

char *storage_get(const char *key)
{
    char *path, *buf = NULL;
    FILE *f;
    long n;

    if (!data_dir)
        return NULL;
    path = join(data_dir, key);
    f = fopen(path, "rb");
    free(path);
    if (!f)
        return NULL;

    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0) {
        rewind(f);
        buf = malloc(n + 1);
        if (buf) {
            size_t got = fread(buf, 1, n, f);
            buf[got] = '\0';
        }
    }
    fclose(f);
    return buf;
}

void storage_remove(const char *key)
{
    char *path;
    if (!data_dir)
        return;
    path = join(data_dir, key);
    remove(path);
    free(path);
}

#endif
