// noteSS offline cache.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// KISS local fallback for unsent notes:
//   - cache_save_note() stores one note as a plain-text file under
//     $XDG_CACHE_HOME/noteSS (or ~/.cache/noteSS), mode 0600.
//   - cache_flush_async() uploads every cached note in filename order in a
//     detached background thread and deletes each file on HTTP 200/201.
//   - cache_flush_sync() does the same in the calling thread (used after a
//     successful send, so the app never quits before the cache is uploaded).
//     Notes that fail to upload stay in the cache for the next launch.

#ifndef NOTESS_CACHE_H
#define NOTESS_CACHE_H

#include <stddef.h>

// Save one note to the local cache. Returns 0 on success, -1 on error.
int cache_save_note(const char *text);

// Number of notes currently waiting in the cache.
size_t cache_pending_count(void);

// Upload all cached notes, deleting each one after HTTP 200/201.
// Safe to call when the cache is empty (does nothing). Never blocks.
void cache_flush_async(const char *endpoint, const char *token);

// Same as above, but blocking: returns after every cached note was tried.
// Safe to call from any worker thread (calls are serialized internally).
void cache_flush_sync(const char *endpoint, const char *token);

// Same as cache_flush_async, but calls done(data) on the GTK main thread after the
// upload attempt finishes (even when the cache was empty). done() must be
// fast and non-blocking; it may be NULL (behaves like cache_flush_async).
void cache_flush_async_done(const char *endpoint, const char *token,
                            void (*done)(void *data), void *data);

#endif // NOTESS_CACHE_H
