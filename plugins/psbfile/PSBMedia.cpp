#include "PSBMedia.h"
#include "UtilStreams.h"
#include "PSBFile.h"
#include "Platform.h"

#include <cstring>

PSB::PSBMedia* psbVar = nullptr;

namespace PSB
{

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
    tjs_int slash = name.IndexOf(TJS_N('/'));
    if (slash == -1)
        return nullptr;
    auto entry = _resources.find(name.SubString(0, slash).AsLowerCase());
    if (entry == _resources.end())
        return nullptr;
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

bool PSBMedia::CheckExistentStorage(const ttstr& name)
{
    tjs_int slash = name.IndexOf(TJS_N('/'));
    if (slash == -1)
        return false;
    ttstr file;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (FindLocked(name, file))
            return true;
        if (!file.IsEmpty())
            return false; // the PSB is known; the resource is not in it
    }
    // Load the PSB once to learn its resources. Invalidate drops the parsed
    // copy; the entry stays registered and is served from the file.
    {
        tTJSNI_PsbFile tmp;
        tmp.load(name.SubString(0, slash));
        tmp.Invalidate();
    }
    std::lock_guard<std::mutex> lock(_mutex);
    return FindLocked(name, file) != nullptr;
}

tTJSBinaryStream* PSBMedia::Open(const ttstr& name, tjs_uint32 flags)
{
    ttstr file;
    tjs_uint32 offset, length;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        PSBMediaInfo::PSBMediaItemInfo* item = FindLocked(name, file);
        if (!item)
            return nullptr;
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
    }

    // The owning PSBFile is gone: read the PSB again (decompressing MDF, so
    // the offsets match) and copy the chunk out.
    tTVPMemoryStream* whole = nullptr;
    try
    {
        whole = PSBReadDecompressed(TVPCreateStream(file));
    }
    catch (...)
    {
        return nullptr;
    }
    if (!whole)
        return nullptr;
    if ((tjs_uint64)offset + length > whole->GetSize())
    {
        delete whole;
        return nullptr;
    }
    tTVPMemoryStream* stream = new tTVPMemoryStream(nullptr, length);
    if (length)
        std::memcpy(stream->GetInternalBuffer(), static_cast<const tjs_uint8*>(whole->GetInternalBuffer()) + offset,
                    length);
    delete whole;
    return stream;
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
    // The newest load of a name wins.
    _resources[name] = std::move(data);
}

void PSBMedia::RemovePSBFile(const ttstr& name)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _resources.erase(name);
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
