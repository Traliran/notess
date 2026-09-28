// noteSS offline cache.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// KISS local fallback for unsent notes:
//   - cache_save_note() stores one note as a plain-text file under
//     $XDG_CACHE_HOME/noteSS (or ~/.cache/noteSS), mode 0600.
//   - cache_flush_async() uploads every cached note in filename order in a
//     detached background thread and deletes each file on HTTP 200/201.
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

#endif // NOTESS_CACHE_H
