/*
 * storage.h: a tiny key -> string persistent store for the SDL3 front end.
 *
 * Backed by localStorage on the web and by one file per key in a
 * platform-appropriate data directory on native platforms. Used to save
 * each puzzle's serialised game state and which puzzle is active, so the
 * app resumes exactly where the user left off.
 */
#ifndef PUZZLES_SDL3_STORAGE_H
#define PUZZLES_SDL3_STORAGE_H

/* Resolve (and create) the native data directory. No-op on the web. */
void storage_init(void);

/* Store a NUL-terminated string under key (overwriting any existing). */
void storage_set(const char *key, const char *value);

/* Return the stored string for key, or NULL if absent. The result is
 * heap-allocated; free it with sfree() (or free()). */
char *storage_get(const char *key);

/* Forget the value stored under key. */
void storage_remove(const char *key);

#endif
