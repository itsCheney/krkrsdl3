#pragma once

#include "tjsCommHead.h"

// Movies the host has been configured to skip, by file name.
//
// Some localization patches insert a credits video (commonly signature.wmv)
// after the publisher logos. Playback is skipped by reporting the normal
// end-of-playback status immediately, so waiting scripts continue as if the
// movie had finished; refusing to open it would raise a TJS exception instead.
//
// The list is supplied by the host as a newline-separated set of file names and
// is empty unless configured. Matching is on the base name only, case- and
// width-insensitively, because the same patch video appears under different
// directories across games.

/// Replaces the list. `names` is newline-separated; NULL or empty clears it.
/// Entries may include directories and a `?` parameter suffix; both are
/// stripped. Safe to call between sessions and before startup.
void TVPSetSkippedMovies(const tjs_char* names);

/// True when `storage` names a movie on the list. `storage` may be a full
/// storage path with an optional `?` parameter suffix.
bool TVPIsSkippedMovie(const ttstr& storage);
