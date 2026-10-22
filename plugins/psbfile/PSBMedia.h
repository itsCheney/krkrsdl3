#pragma once
#include "TVPStorage.h"

#include <list>
#include <map>
#include <memory>
#include <mutex>

class tTVPMemoryStream;

namespace PSB
{
struct PSBMediaInfo
{
    struct PSBMediaItemInfo
    {
        // Offset into the decompressed PSB.
        tjs_uint32 Offsets;
        tjs_uint32 Lengths;
        // The resource octet that the PSBFile tree also holds (or, for an
        // extra chunk, a copy only psb:// holds), while its owner is alive.
        // Void after the owner goes away; Open then reads the file again.
        tTJSVariant Data;
    };
    std::map<ttstr, PSBMediaItemInfo> _resources;
    // The path the PSB was loaded from, for reading it again.
    ttstr Path;
    // The PSBFile instance whose octets Data shares, or null.
    const void* Owner = nullptr;
};

class PSBMedia : public iTVPStorageMedia
{
public:
    PSBMedia() : refCount(1) {}

    virtual void AddRef() override { refCount++; };

    virtual void Release() override
    {
        if (refCount == 1)
        {
            delete this;
        }
        else
        {
            refCount--;
        }
    };

    // returns media name like "file", "http" etc.
    virtual void GetName(ttstr& name) override { name = TJS_N("psb"); }

    //	virtual ttstr IsCaseSensitive() = 0;
    // returns whether this media is case sensitive or not

    // normalize domain name according with the media's rule
    virtual void NormalizeDomainName(ttstr& name) override;

    // normalize path name according with the media's rule
    virtual void NormalizePathName(ttstr& name) override;

    // check file existence
    virtual bool CheckExistentStorage(const ttstr& name) override;

    // open a storage and return a tTJSBinaryStream instance.
    // name does not contain in-archive storage name but
    // is normalized.
    virtual tTJSBinaryStream* Open(const ttstr& name, tjs_uint32 flags) override;

    // list files at given place
    virtual void GetListAt(const ttstr& name, iTVPStorageLister* lister) override;

    // basically the same as above,
    // check wether given name is easily accessible from local OS filesystem.
    // if true, returns local OS native name. otherwise returns an empty string.
    virtual void GetLocallyAccessibleName(ttstr& name) override;

    // Media key for a PSB load path: its file name, lower-cased. psb://
    // domains cannot contain '/', so this is the only part they can name.
    static ttstr KeyFor(const ttstr& path);

    // AddPSBFile, RemovePSBFile and DetachPSBFile release octets, so only
    // the main thread calls them.
    void AddPSBFile(const ttstr& name, PSBMediaInfo data);
    // Removes an entry this owner registered.
    void RemovePSBFile(const ttstr& name, const void* owner);
    // Drops the shared octets of an entry this owner registered; the entry
    // stays, served from the file.
    void DetachPSBFile(const ttstr& name, const void* owner);

protected:
    /**
     * デストラクタ
     */
    virtual ~PSBMedia() {}

private:
    tjs_uint refCount; //< リファレンスカウント
    // Open can run on image loading threads.
    std::mutex _mutex;
    std::map<ttstr, PSBMediaInfo> _resources;
    // Resource lists of PSBs that no PSBFile registered, read by
    // CheckExistentStorage or Open (Owner null, Data void). A registered
    // entry of the same name takes precedence and replaces it.
    std::map<ttstr, PSBMediaInfo> _probed;
    // Recently read decompressed PSBs by load path, most recent first, so
    // opening several resources of a released PSB reads it once.
    std::list<std::pair<ttstr, std::shared_ptr<tTVPMemoryStream>>> _cache;
    tjs_uint64 _cacheBytes = 0;
    static constexpr tjs_uint64 CacheByteLimit = 32u * 1024 * 1024;
    static constexpr size_t CacheEntryLimit = 4;

    // Caller holds _mutex. Sets path to the entry's load path when the PSB
    // is known, even if the resource is not.
    PSBMediaInfo::PSBMediaItemInfo* FindLocked(const ttstr& name, ttstr& path);
    // Caller holds _mutex.
    std::shared_ptr<tTVPMemoryStream> CachedLocked(const ttstr& path);
    std::shared_ptr<tTVPMemoryStream> CacheLocked(const ttstr& path, std::shared_ptr<tTVPMemoryStream> whole);
    // Caller does not hold _mutex. The decompressed PSB at path, from the
    // cache or read now; null on any failure.
    std::shared_ptr<tTVPMemoryStream> Load(const ttstr& path);
    // Caller does not hold _mutex. Reads the resource list of the PSB a
    // domain names into _probed; false if it cannot be read.
    bool Probe(const ttstr& domain);
};
} // namespace PSB

extern PSB::PSBMedia* psbVar;
