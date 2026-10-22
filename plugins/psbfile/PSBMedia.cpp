#include "PSBMedia.h"
#include "UtilStreams.h"
#include "PSBFile.h"
#include "Platform.h"

#include <cstring>

PSB::PSBMedia* psbVar = nullptr;

namespace PSB
{

// The index of the '/' after the domain, or -1. (ttstr::IndexOf(tjs_char)
// reads a character past its argument.)
static tjs_int DomainEnd(const ttstr& name)
{
    for (tjs_int i = 0; i < name.GetLen(); i++)
        if (name[i] == TJS_N('/'))
            return i;
    return -1;
}

void PSBMedia::NormalizeDomainName(ttstr& name)
{
    // Keys are lower-cased file names (KeyFor), so the whole domain is too.
    name = name.AsLowerCase();
}

void PSBMedia::NormalizePathName(ttstr& name)
{
    // nothing to do
}

ttstr PSBMedia::KeyFor(const ttstr& path)
{
    // The name after the last folder or archive separator.
    tjs_int start = 0;
    for (tjs_int i = 0; i < path.GetLen(); i++)
    {
        const tjs_char ch = path[i];
        if (ch == TJS_N('/') || ch == TJS_N('\\') || ch == TJS_N('>'))
            start = i + 1;
    }
    return path.SubString(start, path.GetLen() - start).AsLowerCase();
}

PSBMediaInfo::PSBMediaItemInfo* PSBMedia::FindLocked(const ttstr& name, ttstr& path)
{
    tjs_int slash = DomainEnd(name);
    if (slash == -1)
        return nullptr;
    const ttstr key = name.SubString(0, slash).AsLowerCase();
    auto entry = _resources.find(key);
    if (entry == _resources.end())
    {
        entry = _probed.find(key);
        if (entry == _probed.end())
            return nullptr;
    }
    path = entry->second.Path;
    const ttstr chunk = name.SubString(slash + 1, name.GetLen());
    auto item = entry->second._resources.find(chunk);
    if (item != entry->second._resources.end())
        return &item->second;
    // Resource names keep the PSB's case; accept any case from scripts.
    const ttstr lower = chunk.AsLowerCase();
    for (auto& candidate : entry->second._resources)
        if (candidate.first.AsLowerCase() == lower)
            return &candidate.second;
    return nullptr;
}

std::shared_ptr<tTVPMemoryStream> PSBMedia::CachedLocked(const ttstr& path)
{
    for (auto it = _cache.begin(); it != _cache.end(); ++it)
    {
        if (it->first == path)
        {
            _cache.splice(_cache.begin(), _cache, it);
            return _cache.front().second;
        }
    }
    return nullptr;
}

std::shared_ptr<tTVPMemoryStream> PSBMedia::CacheLocked(const ttstr& path, std::shared_ptr<tTVPMemoryStream> whole)
{
    // Another thread may have read the same file meanwhile; keep one copy.
    if (auto cached = CachedLocked(path))
        return cached;
    const tjs_uint64 size = whole->GetSize();
    if (size > CacheByteLimit)
        return whole;
    _cache.emplace_front(path, whole);
    _cacheBytes += size;
    while (_cache.size() > CacheEntryLimit || _cacheBytes > CacheByteLimit)
    {
        _cacheBytes -= _cache.back().second->GetSize();
        _cache.pop_back();
    }
    return whole;
}

std::shared_ptr<tTVPMemoryStream> PSBMedia::Load(const ttstr& path)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (auto cached = CachedLocked(path))
            return cached;
    }
    // Read without the lock; decompressing MDF keeps the offsets valid.
    std::shared_ptr<tTVPMemoryStream> whole;
    try
    {
        whole.reset(PSBReadDecompressed(TVPCreateStream(path)));
    }
    catch (...)
    {
        return nullptr;
    }
    if (!whole)
        return nullptr;
    std::lock_guard<std::mutex> lock(_mutex);
    return CacheLocked(path, std::move(whole));
}

bool PSBMedia::Probe(const ttstr& domain)
{
    // Only the resource list: no PSBFile tree, no TJS objects, and nothing
    // in _resources, whose octets belong to the main thread.
    std::shared_ptr<tTVPMemoryStream> whole = Load(domain);
    if (!whole)
        return false;
    PSBMediaInfo info;
    if (!tTJSNI_PsbFile::scanResources(static_cast<const tjs_uint8*>(whole->GetInternalBuffer()), whole->GetSize(),
                                       info))
        return false;
    info.Path = domain;
    std::lock_guard<std::mutex> lock(_mutex);
    // A PSBFile that registered the name meanwhile wins; so does an earlier
    // probe.
    const ttstr key = domain.AsLowerCase();
    if (_resources.find(key) == _resources.end())
        _probed.emplace(key, std::move(info));
    return true;
}

bool PSBMedia::CheckExistentStorage(const ttstr& name)
{
    tjs_int slash = DomainEnd(name);
    if (slash == -1)
        return false;
    for (int attempt = 0;; attempt++)
    {
        {
            ttstr file;
            std::lock_guard<std::mutex> lock(_mutex);
            if (FindLocked(name, file))
                return true;
            if (!file.IsEmpty())
                return false; // the PSB is known; the resource is not in it
        }
        if (attempt || !Probe(name.SubString(0, slash)))
            return false;
    }
}

tTJSBinaryStream* PSBMedia::Open(const ttstr& name, tjs_uint32 flags)
{
    tjs_int slash = DomainEnd(name);
    if (slash == -1)
        return nullptr;
    for (int attempt = 0;; attempt++)
    {
        ttstr file;
        tjs_uint32 offset, length;
        std::shared_ptr<tTVPMemoryStream> whole;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            PSBMediaInfo::PSBMediaItemInfo* item = FindLocked(name, file);
            if (!item && !file.IsEmpty())
                return nullptr; // the PSB is known; the resource is not in it
            if (item)
            {
                if (item->Data.Type() == tvtOctet)
                {
                    tTJSVariantOctet* octet = item->Data.AsOctetNoAddRef();
                    const tjs_uint size = octet ? octet->GetLength() : 0;
                    tTVPMemoryStream* stream = new tTVPMemoryStream(nullptr, size);
                    if (size)
                        std::memcpy(stream->GetInternalBuffer(), octet->GetData(), size);
                    return stream;
                }
                offset = item->Offsets;
                length = item->Lengths;
                whole = CachedLocked(file);
            }
        }
        if (file.IsEmpty())
        {
            // No PSBFile registered this PSB: learn its resources once.
            if (attempt || !Probe(name.SubString(0, slash)))
                return nullptr;
            continue;
        }

        // The owning PSBFile is gone: copy the chunk out of the PSB read
        // again, or of a recent read of it.
        if (!whole)
            whole = Load(file);
        if (!whole || (tjs_uint64)offset + length > whole->GetSize())
            return nullptr;
        tTVPMemoryStream* stream = new tTVPMemoryStream(nullptr, length);
        if (length)
            std::memcpy(stream->GetInternalBuffer(),
                        static_cast<const tjs_uint8*>(whole->GetInternalBuffer()) + offset, length);
        return stream;
    }
}

void PSBMedia::GetListAt(const ttstr& name, iTVPStorageLister* lister)
{
    // nothing to do
    TVPConsoleLog("TODO PSB:GetListAt");
}

void PSBMedia::GetLocallyAccessibleName(ttstr& name)
{
    // nothing to do
    TVPConsoleLog("TODO PSB:GetLocallyAccessibleName");
}

void PSBMedia::AddPSBFile(const ttstr& name, PSBMediaInfo data)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _probed.erase(name);
    // The newest load of a name wins.
    _resources[name] = std::move(data);
}

void PSBMedia::RemovePSBFile(const ttstr& name, const void* owner)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto entry = _resources.find(name);
    if (entry != _resources.end() && entry->second.Owner == owner)
        _resources.erase(entry);
}

void PSBMedia::DetachPSBFile(const ttstr& name, const void* owner)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto entry = _resources.find(name);
    if (entry == _resources.end() || entry->second.Owner != owner)
        return;
    entry->second.Owner = nullptr;
    for (auto& item : entry->second._resources)
        item.second.Data.Clear();
}

} // namespace PSB
