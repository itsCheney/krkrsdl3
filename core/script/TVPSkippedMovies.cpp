#include "tjsCommHead.h"

#include "TVPSkippedMovies.h"
#include "TVPStorage.h"

#include <mutex>
#include <set>

namespace {
std::mutex skippedMoviesMutex;
std::set<ttstr> skippedMovies;

/// Base name, lowercased, with any `?` parameter suffix and surrounding
/// whitespace removed. Tolerates CRLF-separated host input.
ttstr normalizedMovieKey(const ttstr& value)
{
    ttstr name(value);
    const tjs_char* parameters = TJS_strchr(name.c_str(), TJS_N('?'));
    if (parameters != nullptr)
        name = ttstr(name, (int)(parameters - name.c_str()));

    name = TVPExtractStorageName(name);

    const tjs_char* start = name.c_str();
    const tjs_char* end = start + name.GetLen();
    auto blank = [](tjs_char character) {
        return character == TJS_N(' ') || character == TJS_N('\t') || character == TJS_N('\r');
    };
    while (start < end && blank(*start))
        ++start;
    while (end > start && blank(*(end - 1)))
        --end;
    if (start == end)
        return ttstr();

    ttstr trimmed(start, (int)(end - start));
    trimmed.ToLowerCase();
    return trimmed;
}
} // namespace

void TVPSetSkippedMovies(const tjs_char* names)
{
    std::set<ttstr> parsed;
    if (names != nullptr)
    {
        ttstr remaining(names);
        while (remaining.GetLen() > 0)
        {
            const tjs_char* separator = TJS_strchr(remaining.c_str(), TJS_N('\n'));
            ttstr entry;
            if (separator != nullptr)
            {
                const int length = (int)(separator - remaining.c_str());
                entry = ttstr(remaining, length);
                remaining = ttstr(remaining.c_str() + length + 1);
            }
            else
            {
                entry = remaining;
                remaining.Clear();
            }

            const ttstr key = normalizedMovieKey(entry);
            if (key.GetLen() > 0)
                parsed.insert(key);
        }
    }

    std::lock_guard<std::mutex> lock(skippedMoviesMutex);
    skippedMovies = std::move(parsed);
}

bool TVPIsSkippedMovie(const ttstr& storage)
{
    const ttstr key = normalizedMovieKey(storage);
    if (key.GetLen() == 0)
        return false;

    std::lock_guard<std::mutex> lock(skippedMoviesMutex);
    return skippedMovies.find(key) != skippedMovies.end();
}
