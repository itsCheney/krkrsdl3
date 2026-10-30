#include "tjsCommHead.h"
#include "PSBFile.h"

#include "Platform.h"
#include "TVPStorage.h"
#include "UtilStreams.h"
#include "tjsArray.h"
#include "TVPEvent.h"
#include "psbsession.h"
#include "../../core/render/CPUFrameDiagnostics.h"

#include <zlib.h>

#include <chrono>
#include <cstring>
#include <deque>
#include <memory>
#include <vector>

namespace
{
// Freeing a scenario tree in one go took 125-130 ms on device (one frame).
// Each queued container holds the only native reference, and the queue is in
// creation order (parents first), so each release frees one container.
class PSBDeferredRelease : public tTVPContinuousEventCallbackIntf
{
public:
    bool enabled = true;

    void Add(std::vector<iTJSDispatch2*>& objects)
    {
        if (!enabled)
        {
            for (auto* object : objects) object->Release();
            objects.clear();
            return;
        }
        if (objects.empty()) return;
        _queue.insert(_queue.end(), objects.begin(), objects.end());
        objects.clear();
        if (!_hooked)
        {
            TVPAddContinuousEventHook(this);
            _hooked = true;
        }
    }
    void OnContinuousCallback(tjs_uint64) override
    {
        using clock = std::chrono::steady_clock;
        const auto deadline = clock::now() + std::chrono::microseconds(2000);
        size_t released = 0;
        while (!_queue.empty())
        {
            iTJSDispatch2* object = _queue.front();
            _queue.pop_front();
            object->Release();
            if ((++released & 31) == 0 && clock::now() >= deadline) break;
        }
        if (_queue.empty()) Unhook();
    }
    void Flush()
    {
        while (!_queue.empty())
        {
            iTJSDispatch2* object = _queue.front();
            _queue.pop_front();
            object->Release();
        }
        Unhook();
    }

private:
    std::deque<iTJSDispatch2*> _queue;
    bool _hooked = false;

    void Unhook()
    {
        if (!_hooked) return;
        TVPRemoveContinuousEventHook(this);
        _hooked = false;
    }
};

PSBDeferredRelease& DeferredRelease()
{
    static auto* release = new PSBDeferredRelease();
    return *release;
}
} // namespace

void TVPSetPSBDeferredRelease(bool enabled)
{
    DeferredRelease().enabled = enabled;
    if (!enabled) DeferredRelease().Flush();
}

void TVPFlushPSBDeferredRelease()
{
    DeferredRelease().Flush();
}

//---------------------------------------------------------------------------
// tTJSNI_PsbFile : PsbFile TJS native instance
//---------------------------------------------------------------------------
tTJSNI_PsbFile::tTJSNI_PsbFile()
{
    Owner = NULL;
    _header.version = 0;
    _header.encrypt = 0;
    _header.offsetEncrypt = 0;
    _header.offsetNames = 0;
    _header.offsetStrings = 0;
    _header.offsetStringsData = 0;
    _header.offsetChunkOffsets = 0;
    _header.offsetChunkLengths = 0;
    _header.offsetChunkData = 0;
    _header.offsetEntries = 0;
    _header.checksum = 0;
    _header.offsetExtraChunkOffsets = 0;
    _header.offsetExtraChunkLengths = 0;
    _header.offsetExtraChunkData = 0;
}
tjs_error tTJSNI_PsbFile::Construct(tjs_int numparams, tTJSVariant** param, iTJSDispatch2* tjs_obj)
{
    Owner = tjs_obj;

    return TJS_S_OK;
}
void tTJSNI_PsbFile::Invalidate()
{
    krkrsdl3::cpu_frame::EventScope cpuInvalidate(krkrsdl3::cpu_frame::Kind::PSBInvalidate);
    releaseLoad();
    detachMedia();
    _resources._resources.clear();
    // The tree is released by Destruct, inside the PSBDestroy scope.
    Owner = NULL;
    inherited::Invalidate();
}

void tTJSNI_PsbFile::releaseTree()
{
    // Every container still holds its own reference, so this frees nothing.
    _root.Clear();
    DeferredRelease().Add(_containers);
    _stringValues.clear();
    _stringBuilt.clear();
}

void tTJSNI_PsbFile::releaseLoad()
{
    delete filePtr;
    filePtr = nullptr;
    fileData = nullptr;
    fileSize = 0;
}

void tTJSNI_PsbFile::detachMedia()
{
    if (psbVar != nullptr && !_mediaKey.IsEmpty())
        psbVar->DetachPSBFile(_mediaKey, this);
    _mediaKey.Clear();
}

void tTJSNI_PsbFile::dropMedia()
{
    if (psbVar != nullptr && !_mediaKey.IsEmpty())
        psbVar->RemovePSBFile(_mediaKey, this);
    _mediaKey.Clear();
}

tTVPMemoryStream* PSBReadDecompressed(tTJSBinaryStream* src)
{
    if (!src)
        return nullptr;
    std::unique_ptr<tTJSBinaryStream> file(src);
    const tjs_uint64 readSize = file->GetSize();
    if (readSize < 9 || readSize > 0xFFFFFFFFull)
        return nullptr;

    char sign[4] = {0, 0, 0, 0};
    {
        krkrsdl3::cpu_frame::EventScope cpuRead(krkrsdl3::cpu_frame::Kind::PSBRead);
        cpuRead.SetWorkBytes(4);
        file->Read(sign, 4);
    }
    sign[3] = '\0';
    if (TJS_strcasecmp(sign, "MDF") != 0)
    {
        // Read the whole file once; parsing from memory avoids xp3filter
        // decrypting the same bytes on every seek.
        std::unique_ptr<tTVPMemoryStream> memory(new tTVPMemoryStream(nullptr, (tjs_uint)readSize));
        krkrsdl3::cpu_frame::EventScope cpuRead(krkrsdl3::cpu_frame::Kind::PSBRead);
        cpuRead.SetWorkBytes(readSize);
        file->SetPosition(0);
        file->ReadBuffer(memory->GetInternalBuffer(), (tjs_uint)readSize);
        return memory.release();
    }

    // MDF: "MDF\0", uncompressed size, zlib data. Inflate straight into the
    // stream's buffer, so the peak is the compressed bytes plus one copy.
    uLongf uncompressedSize;
    std::vector<tjs_uint8> compressed((size_t)(readSize - 8));
    {
        krkrsdl3::cpu_frame::EventScope cpuRead(krkrsdl3::cpu_frame::Kind::PSBRead);
        cpuRead.SetWorkBytes(4);
        uncompressedSize = file->ReadI32LE();
    }
    {
        krkrsdl3::cpu_frame::EventScope cpuRead(krkrsdl3::cpu_frame::Kind::PSBRead);
        cpuRead.SetWorkBytes(compressed.size());
        file->ReadBuffer(compressed.data(), (tjs_uint)compressed.size());
    }
    file.reset();
    std::unique_ptr<tTVPMemoryStream> memory(new tTVPMemoryStream(nullptr, (tjs_uint)uncompressedSize));
    int result;
    {
        krkrsdl3::cpu_frame::EventScope cpuDecompress(krkrsdl3::cpu_frame::Kind::PSBDecompress);
        cpuDecompress.SetWorkBytes(compressed.size());
        uLongf size = uncompressedSize;
        result = uncompress(static_cast<Bytef*>(memory->GetInternalBuffer()), &size, compressed.data(),
                            (uLong)compressed.size());
        if (result == Z_OK && size != uncompressedSize)
            result = Z_DATA_ERROR;
        cpuDecompress.Finish(result != Z_OK);
    }
    if (result != Z_OK)
        return nullptr;
    return memory.release();
}

void tTJSNI_PsbFile::Destruct()
{
    // The scope must surround deletion: a destructor-body scope would finish
    // before _root's member destructor releases its TJS object tree.
    krkrsdl3::cpu_frame::EventScope cpuDestroy(krkrsdl3::cpu_frame::Kind::PSBDestroy);
    releaseTree();
    delete this;
}

bool tTJSNI_PsbFile::load(const ttstr& filePath)
{
    tTJSBinaryStream* _filePtr;
    {
        krkrsdl3::cpu_frame::EventScope cpuOpen(krkrsdl3::cpu_frame::Kind::PSBOpen);
        _filePtr = TVPCreateStream(filePath);
        cpuOpen.Finish(_filePtr == nullptr);
    }
    return loadFromStream(_filePtr, filePath);
}

bool tTJSNI_PsbFile::parseTables()
{
    // ReadHeader
    filePtr->SetPosition(0);
    if (!_header.parsePSBHeader(filePtr))
        return false;

    // Encrypted
    if (_header.isEncrypted() && _header.getHeaderLength() > filePtr->GetSize())
    {
        TVPConsoleLog("psb file is encrypted");
        return false;
    }
    if (_header.version > 3)
    {
        TVPConsoleLog("not support psb file format version > 3");
        return false;
    }

    // Pre Load Strings
    filePtr->SetPosition(_header.offsetStrings);
    PSB::parsePSBArray(&stringsOffset,
                       filePtr->ReadI8LE() - static_cast<tjs_int8>(PSB::PSBObjType::ArrayN1) + 1,
                       filePtr);

    // Pre Load Names
    if (_header.version == 1)
    {
        // don't believe HeaderLength
        if (_header.offsetEncrypt >= filePtr->GetSize())
        {
            _header.offsetEncrypt = _header.getHeaderLength();
        }
        filePtr->SetPosition(_header.offsetEncrypt);
        PSB::parsePSBArray(
            &nameIndexes, filePtr->ReadI8LE() - static_cast<tjs_int8>(PSB::PSBObjType::ArrayN1) + 1,
            filePtr);
        // Load Names
        namesCache.reserve(nameIndexes.size());
        for (size_t i = 0; i < nameIndexes.size(); i++)
        {
            filePtr->SetPosition(_header.offsetNames + nameIndexes[i]);
            std::string str;
            tjs_uint8 val8 = 0;
            while (true)
            {
                if (filePtr->Read(&val8, 1) == 0)
                    break;

                if (val8 == '\0')
                {
                    str.append(1, val8);
                    break;
                }
                str.append(1, val8);
            }
            namesCache.push_back(str);
        }
    }
    else
    {
        filePtr->SetPosition(_header.offsetNames);
        PSB::parsePSBArray(
            &charset, filePtr->ReadI8LE() - static_cast<tjs_int8>(PSB::PSBObjType::ArrayN1) + 1,
            filePtr);
        PSB::parsePSBArray(
            &namesData, filePtr->ReadI8LE() - static_cast<tjs_int8>(PSB::PSBObjType::ArrayN1) + 1,
            filePtr);
        PSB::parsePSBArray(
            &nameIndexes, filePtr->ReadI8LE() - static_cast<tjs_int8>(PSB::PSBObjType::ArrayN1) + 1,
            filePtr);
        // Load Names
        namesCache.reserve(nameIndexes.size());
        for (int i = 0; i < nameIndexes.size(); i++)
        {
            std::string list;
            const auto index = nameIndexes[i];
            // A corrupt table can index past the arrays or loop; such a name
            // ends where it goes wrong.
            auto chr = index < namesData.size() ? namesData[index] : 0;
            for (size_t steps = 0; chr != 0 && steps < namesData.size(); ++steps)
            {
                if (chr >= namesData.size())
                    break;
                const auto code = namesData[chr];
                if (code >= charset.size())
                    break;
                const auto d = charset[code];
                const auto realChr = chr - d;
                chr = code;
                list.append(1, realChr);
            }
            std::reverse(list.begin(), list.end());
            namesCache.push_back(list);
        }
    }

    // Pre Load Resources (Chunks)
    filePtr->SetPosition(_header.offsetChunkOffsets);
    PSB::parsePSBArray(&chunkOffsets,
                       filePtr->ReadI8LE() - static_cast<tjs_int8>(PSB::PSBObjType::ArrayN1) + 1,
                       filePtr);
    filePtr->SetPosition(_header.offsetChunkLengths);
    PSB::parsePSBArray(&chunkLengths,
                       filePtr->ReadI8LE() - static_cast<tjs_int8>(PSB::PSBObjType::ArrayN1) + 1,
                       filePtr);

    if (_header.version >= 4)
    {
        // Pre Load Extra Resources (Chunks)
        filePtr->SetPosition(_header.offsetExtraChunkOffsets);
        PSB::parsePSBArray(
            &extraChunkOffsets,
            filePtr->ReadI8LE() - static_cast<tjs_int8>(PSB::PSBObjType::ArrayN1) + 1, filePtr);
        filePtr->SetPosition(_header.offsetExtraChunkLengths);
        PSB::parsePSBArray(
            &extraChunkLengths,
            filePtr->ReadI8LE() - static_cast<tjs_int8>(PSB::PSBObjType::ArrayN1) + 1, filePtr);
    }

    return true;
}

bool tTJSNI_PsbFile::loadFromStream(tTJSBinaryStream* _filePtr, const ttstr& filePath)
{
    krkrsdl3::cpu_frame::EventScope cpuLoad(krkrsdl3::cpu_frame::Kind::PSBLoad);
    if (!_filePtr)
    {
        cpuLoad.Finish(true);
        return false;
    }
    // The psb:// entry this instance registered stays until the new parse
    // succeeds and replaces it. A failed parse removes it: the tree is gone,
    // so psb:// must not keep serving the previous file.
    releaseLoad();
    _resources._resources.clear();
    stringsOffset.clear();
    namesData.clear();
    charset.clear();
    nameIndexes.clear();
    namesCache.clear();
    chunkOffsets.clear();
    chunkLengths.clear();
    extraChunkOffsets.clear();
    extraChunkLengths.clear();
    releaseTree();

    cpuLoad.SetWorkBytes(_filePtr->GetSize());
    tTVPMemoryStream* memory;
    try
    {
        memory = PSBReadDecompressed(_filePtr);
    }
    catch (...)
    {
        dropMedia();
        throw;
    }
    if (!memory)
    {
        dropMedia();
        cpuLoad.Finish(true);
        return false;
    }
    filePtr = memory;
    fileData = static_cast<const tjs_uint8*>(memory->GetInternalBuffer());
    fileSize = memory->GetSize();

    {
        krkrsdl3::cpu_frame::EventScope cpuTables(krkrsdl3::cpu_frame::Kind::PSBTables);
        if (!parseTables())
        {
            releaseLoad();
            dropMedia();
            cpuTables.Finish(true);
            cpuLoad.Finish(true);
            return false;
        }
    }
    _stringValues.assign(stringsOffset.size(), tTJSVariant());
    _stringBuilt.assign(stringsOffset.size(), false);

    // Time only the outer materialization, not each recursive node/dispatch.
    try
    {
        krkrsdl3::cpu_frame::EventScope cpuMaterialize(krkrsdl3::cpu_frame::Kind::PSBMaterialize);
        _root = readAllObjs("root", _header.offsetEntries);
    }
    catch (...)
    {
        releaseLoad();
        releaseTree();
        _resources._resources.clear();
        dropMedia();
        throw;
    }
    _stringValues.clear();
    _stringBuilt.clear();
    // The tree is complete and holds every resource as an octet; the
    // decompressed file is no longer needed.
    releaseLoad();
    // add To media
    if (psbVar != nullptr && !filePath.IsEmpty())
    {
        const ttstr key = PSB::PSBMedia::KeyFor(filePath);
        // A different name than before: the previous file is served from disk.
        if (key != _mediaKey)
            detachMedia();
        _mediaKey = key;
        _resources.Path = filePath;
        _resources.Owner = this;
        psbVar->AddPSBFile(_mediaKey, _resources);
    }
    else
    {
        detachMedia();
    }
    return true;
}

bool tTJSNI_PsbFile::scanResources(const tjs_uint8* data, tjs_uint64 size, PSB::PSBMediaInfo& out)
{
    if (!data || size > 0xFFFFFFFFull)
        return false;
    // A private parser, never registered: Owner stays null and _mediaKey
    // empty, so neither its parse nor its destruction touches psb://.
    tTJSNI_PsbFile scanner;
    scanner._scanOnly = true;
    // The stream only references data; several scans may share it.
    scanner.filePtr = new tTVPMemoryStream(data, (tjs_uint)size);
    scanner.fileData = data;
    scanner.fileSize = size;
    bool ok = false;
    try
    {
        ok = scanner.parseTables();
        if (ok)
            scanner.readAllObjs("root", scanner._header.offsetEntries);
    }
    catch (...)
    {
        ok = false;
    }
    scanner.releaseLoad();
    if (ok)
        out._resources = std::move(scanner._resources._resources);
    return ok;
}

tTJSVariant tTJSNI_PsbFile::root()
{
    return _root;
}

tjs_uint32 tTJSNI_PsbFile::readListInfo(std::vector<tjs_uint32>* target)
{
    PSB::parsePSBArray(
        target, filePtr->ReadI8LE() - static_cast<tjs_int8>(PSB::PSBObjType::ArrayN1) + 1, filePtr);
    return filePtr->GetPosition();
}

void tTJSNI_PsbFile::refreshListInfo(std::vector<tjs_uint32>* target1,
                                     std::vector<tjs_uint32>* target2)
{
    // v1 objects store each entry as a 32-bit name index followed by the
    // value.
    target2->resize(target1->size());
    tjs_uint32 basePose = filePtr->GetPosition();
    for (tjs_uint32 i = 0; i < target1->size(); i++)
    {
        filePtr->SetPosition(basePose + target1->at(i));
        target2->at(i) = filePtr->ReadI32LE();
        target1->at(i) += 4;
    }
}

tTJSVariant tTJSNI_PsbFile::readAllObjs(const ttstr& key, tjs_uint32 _objOffset)
{
    filePtr->SetPosition(_objOffset);
    auto typeByte = filePtr->ReadI8LE();
    switch (auto type = static_cast<PSB::PSBObjType>(typeByte))
    {
        case PSB::PSBObjType::None:
            return tTJSVariant();
            ;
        case PSB::PSBObjType::Null:
            return tTJSVariant();
        case PSB::PSBObjType::False:
        case PSB::PSBObjType::True:
            return tTJSVariant(type == PSB::PSBObjType::True);
        case PSB::PSBObjType::NumberN0:
            return tTJSVariant(0);
        case PSB::PSBObjType::NumberN1:
        {
            tjs_int8 val8 = 0;
            filePtr->ReadBuffer(&val8, 1);
            return tTJSVariant(val8);
        }
        case PSB::PSBObjType::NumberN2:
        {
            tjs_int16 val16 = 0;
            filePtr->ReadBuffer(&val16, 2);
            return tTJSVariant(val16);
        }
        case PSB::PSBObjType::NumberN3:
        {
            tjs_int32 val32 = 0;
            filePtr->ReadBuffer(&val32, 3);
            val32 |= ((val32 & 0x800000) == 0 ? 0 : (tjs_int32)0xFF000000);
            return tTJSVariant(val32);
        }
        case PSB::PSBObjType::NumberN4:
        {
            tjs_int32 val32 = 0;
            filePtr->ReadBuffer(&val32, 4);
            return tTJSVariant(val32);
        }
        case PSB::PSBObjType::NumberN5:
        {
            tjs_int64 val64 = 0;
            filePtr->ReadBuffer(&val64, 5);
            val64 |= ((val64 & 0x8000000000) == 0 ? 0 : (tjs_int64)0xFFFFFF0000000000);
            return tTJSVariant(val64);
        }
        case PSB::PSBObjType::NumberN6:
        {
            tjs_int64 val64 = 0;
            filePtr->ReadBuffer(&val64, 6);
            val64 |= ((val64 & 0x800000000000) == 0 ? 0 : (tjs_int64)0xFFFF000000000000);
            return tTJSVariant(val64);
        }
        case PSB::PSBObjType::NumberN7:
        {
            tjs_int64 val64 = 0;
            filePtr->ReadBuffer(&val64, 7);
            val64 |= ((val64 & 0x80000000000000) == 0 ? 0 : (tjs_int64)0xFF00000000000000);
            return tTJSVariant(val64);
        }
        case PSB::PSBObjType::NumberN8:
        {
            tjs_int64 val64 = 0;
            filePtr->ReadBuffer(&val64, 8);
            return tTJSVariant(val64);
        }
        case PSB::PSBObjType::Float0:
            return tTJSVariant(0.0f);
        case PSB::PSBObjType::Float:
        {
            tjs_uint8 buffer[4];
            filePtr->ReadBuffer(buffer, 4);
            float tmp;
            std::memcpy(&tmp, buffer, 4);
            return tTJSVariant((tjs_real)tmp);
        }
        case PSB::PSBObjType::Double:
        {
            tjs_uint8 buffer[8];
            filePtr->ReadBuffer(buffer, 8);
            double tmp;
            std::memcpy(&tmp, buffer, 8);
            return tTJSVariant((tjs_real)tmp);
        }
        case PSB::PSBObjType::ArrayN1:
        case PSB::PSBObjType::ArrayN2:
        case PSB::PSBObjType::ArrayN3:
        case PSB::PSBObjType::ArrayN4:
        case PSB::PSBObjType::ArrayN5:
        case PSB::PSBObjType::ArrayN6:
        case PSB::PSBObjType::ArrayN7:
        case PSB::PSBObjType::ArrayN8:
        {
            if (_scanOnly)
                return tTJSVariant();
            std::vector<tjs_uint32> tmp;
            PSB::parsePSBArray(&tmp, typeByte - static_cast<tjs_int8>(PSB::PSBObjType::ArrayN1) + 1,
                               filePtr);
            iTJSDispatch2* array = TJSCreateArrayObject();
            array->AddRef();
            _containers.push_back(array);
            for (tjs_int i = 0; i < (tjs_int)tmp.size(); i++)
            {
                tTJSVariant value(static_cast<tjs_int32>(tmp[i]));
                array->PropSetByNum(TJS_MEMBERENSURE, i, &value, array);
            }
            tTJSVariant result(array, array);
            array->Release();
            return result;
        }
        case PSB::PSBObjType::StringN1:
        case PSB::PSBObjType::StringN2:
        case PSB::PSBObjType::StringN3:
        case PSB::PSBObjType::StringN4:
        {
            if (_scanOnly)
                return tTJSVariant();
            tjs_int32 idx = 0;
            filePtr->ReadBuffer(&idx,
                                typeByte - static_cast<tjs_int8>(PSB::PSBObjType::StringN1) + 1);
            if (idx < 0 || (size_t)idx >= stringsOffset.size())
                return tTJSVariant("");
            if (_stringBuilt[idx])
                return _stringValues[idx];
            // A string runs to its NUL; one cut off by the end of the file
            // reads as empty, as before.
            tTJSVariant value("");
            const tjs_uint64 start = (tjs_uint64)_header.offsetStringsData + stringsOffset[idx];
            if (start < fileSize)
            {
                const auto* begin = reinterpret_cast<const char*>(fileData + start);
                if (const auto* end = static_cast<const char*>(std::memchr(begin, 0, (size_t)(fileSize - start))))
                    value = tTJSVariant(ttstr(std::string(begin, end)));
            }
            _stringValues[idx] = value;
            _stringBuilt[idx] = true;
            return value;
        }
        case PSB::PSBObjType::ResourceN1:
        case PSB::PSBObjType::ResourceN2:
        case PSB::PSBObjType::ResourceN3:
        case PSB::PSBObjType::ResourceN4:
        {
            if (key.IsEmpty())
                return tTJSVariant();

            tjs_int32 idx = 0;
            filePtr->ReadBuffer(&idx,
                                typeByte - static_cast<tjs_int8>(PSB::PSBObjType::ResourceN1) + 1);
            if (idx < 0 || (size_t)idx >= chunkOffsets.size() || (size_t)idx >= chunkLengths.size())
                return tTJSVariant();
            const tjs_uint64 offset = (tjs_uint64)_header.offsetChunkData + chunkOffsets[idx];
            const tjs_uint32 length = chunkLengths[idx];
            if (offset + length > fileSize)
                return tTJSVariant();
            // Scripts expect the resource bytes in the tree. psb:// shares
            // this octet rather than keeping its own copy.
            tTJSVariant ret;
            if (!_scanOnly)
                ret = tTJSVariant(fileData + offset, length);
            // The first entry for a key wins, as before.
            _resources._resources.emplace(key, PSB::PSBMediaInfo::PSBMediaItemInfo{(tjs_uint32)offset, length, ret});
            return ret;
        }
        case PSB::PSBObjType::ExtraChunkN1:
        case PSB::PSBObjType::ExtraChunkN2:
        case PSB::PSBObjType::ExtraChunkN3:
        case PSB::PSBObjType::ExtraChunkN4:
        {
            if (key.IsEmpty())
                return tTJSVariant();

            tjs_int32 idx = 0;
            filePtr->ReadBuffer(&idx, typeByte -
                                          static_cast<tjs_int8>(PSB::PSBObjType::ExtraChunkN1) + 1);

            if (idx < 0 || (size_t)idx >= extraChunkOffsets.size() || (size_t)idx >= extraChunkLengths.size() ||
                _resources._resources.count(key))
                return tTJSVariant();
            const tjs_uint64 offset = (tjs_uint64)_header.offsetExtraChunkData + extraChunkOffsets[idx];
            const tjs_uint32 length = extraChunkLengths[idx];
            if (offset + length > fileSize)
                return tTJSVariant();
            // The tree has no value for extra chunks. psb:// keeps this copy
            // while the owner is alive, so opens need not read the file.
            tTJSVariant data;
            if (!_scanOnly)
                data = tTJSVariant(fileData + offset, length);
            _resources._resources.emplace(key, PSB::PSBMediaInfo::PSBMediaItemInfo{(tjs_uint32)offset, length, data});
            return tTJSVariant();
        }
        case PSB::PSBObjType::List:
        {
            std::vector<tjs_uint32> _objsOffset;
            tjs_uint32 _tmpOffset = this->readListInfo(&_objsOffset);

            if (_scanOnly)
            {
                for (auto _offset : _objsOffset)
                    readAllObjs(ttstr(), _tmpOffset + _offset);
                return tTJSVariant();
            }
            iTJSDispatch2* array = TJSCreateArrayObject();
            array->AddRef();
            _containers.push_back(array);
            for (tjs_int i = 0; i < (tjs_int)_objsOffset.size(); i++)
            {
                tTJSVariant obj = readAllObjs(ttstr(), _tmpOffset + _objsOffset[i]);
                array->PropSetByNum(TJS_MEMBERENSURE, i, &obj, array);
            }
            tTJSVariant result(array, array);
            array->Release();
            return result;
        }
        case PSB::PSBObjType::Objects:
        {
            std::vector<tjs_uint32> _objsOffset, _objsNamesIdx;
            tjs_uint32 _tmpOffset = 0;
            if (_header.version == 1)
            {
                _tmpOffset = this->readListInfo(&_objsOffset);
                this->refreshListInfo(&_objsOffset, &_objsNamesIdx);
            }
            else
            {
                this->readListInfo(&_objsNamesIdx);
                _tmpOffset = this->readListInfo(&_objsOffset);
            }

            if (_scanOnly)
            {
                for (tjs_int i = 0; i < _objsNamesIdx.size(); i++)
                    readAllObjs(namesCache.at(_objsNamesIdx.at(i)), _tmpOffset + _objsOffset.at(i));
                return tTJSVariant();
            }
            iTJSDispatch2* dsp = TJSCreateDictionaryObject();
            dsp->AddRef();
            _containers.push_back(dsp);
            for (tjs_int i = 0; i < _objsNamesIdx.size(); i++)
            {
                const ttstr& keyName = namesCache.at(_objsNamesIdx.at(i));
                tTJSVariant obj = readAllObjs(keyName, _tmpOffset + _objsOffset.at(i));
                // The shared name string carries its hash, and every
                // dictionary with this key references it instead of a copy.
                tTJSVariantString* name = keyName.AsVariantStringNoAddRef();
                if (!name || TJS_FAILED(dsp->PropSetByVS(TJS_MEMBERENSURE, name, &obj, dsp)))
                    dsp->PropSet(TJS_MEMBERENSURE, keyName.c_str(), nullptr, &obj, dsp);
            }
            tTJSVariant result(dsp, dsp);
            dsp->Release();
            return result;
        }
        default:
            TVPConsoleLog("unknown psbObjType");
            return tTJSVariant();
    }
}

//---------------------------------------------------------------------------
// tTJSNC_PsbFile : PsbFile TJS native class
//---------------------------------------------------------------------------
tjs_uint32 tTJSNC_PsbFile::ClassID = (tjs_uint32)-1;
tTJSNC_PsbFile::tTJSNC_PsbFile()
  : tTJSNativeClass(TJS_N("PSBFile")){
    TJS_BEGIN_NATIVE_MEMBERS(PSBFile)
    //----------------------------------------------------------------------
    // finalize/methods
    //----------------------------------------------------------------------
    TJS_DECL_EMPTY_FINALIZE_METHOD
    //----------------------------------------------------------------------
    // constructor/methods
    //----------------------------------------------------------------------
    TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL(_this, tTJSNI_PsbFile, PSBFile){
    if (numparams == 0) return TJS_S_OK;
    else if (numparams == 1 && param[0]->Type() == tvtString)
    {
        ttstr path = *param[0];
        if (_this->load(path))
        {
            if (result)
                *result = true;
        }
        else
        {
            if (result)
                *result = false;
        }
    }
    else if (numparams == 1 && param[0]->Type() == tvtOctet)
    {
        tTJSVariantOctet* data = param[0]->AsOctetNoAddRef();
        // 创建stream存储数据
        tTVPMemoryStream* _memPtr = new tTVPMemoryStream(nullptr, data->GetLength());
        std::memcpy(_memPtr->GetInternalBuffer(), data->GetData(), data->GetLength());
        // 加载
        if (_this->loadFromStream(_memPtr, ""))
        {
            if (result)
                *result = true;
        }
        else
        {
            if (result)
                *result = false;
        }
    }
    else
    {
        return TJS_E_BADPARAMCOUNT;
    }
    return TJS_S_OK;
    }
    TJS_END_NATIVE_CONSTRUCTOR_DECL(PSBFile)
    //----------------------------------------------------------------------
    //-- methods
    //----------------------------------------------------------------------
    TJS_BEGIN_NATIVE_METHOD_DECL(load)
    {
        TJS_GET_NATIVE_INSTANCE(_this, tTJSNI_PsbFile);
        if (numparams != 1)
        {
            return TJS_E_BADPARAMCOUNT;
        }
        if (param[0]->Type() == tvtString)
        {
            ttstr path = *param[0];
            if (_this->load(path))
            {
                if (result)
                    *result = tTJSVariant(true);
            }
            else
            {
                if (result)
                    *result = tTJSVariant(false);
            }
        }
        else if (param[0]->Type() == tvtOctet)
        {
            TVPConsoleLog("PSBFile::load stream no implement!");
        }
        else
        {
            return TJS_E_INVALIDPARAM;
        }
        return TJS_S_OK;
    }
    TJS_END_NATIVE_METHOD_DECL(load)
    //----------------------------------------------------------------------
    // properties
    //----------------------------------------------------------------------
    TJS_BEGIN_NATIVE_PROP_DECL(root){
        TJS_BEGIN_NATIVE_PROP_GETTER{TJS_GET_NATIVE_INSTANCE(_this, tTJSNI_PsbFile);
    if (result)
        *result = _this->root();
    return TJS_S_OK;
    }
    TJS_END_NATIVE_PROP_GETTER
    TJS_DENY_NATIVE_PROP_SETTER
    }
    TJS_END_NATIVE_PROP_DECL(root)
    //----------------------------------------------------------------------
    TJS_END_NATIVE_MEMBERS
}

tTJSNativeInstance* tTJSNC_PsbFile::CreateNativeInstance()
{
    return new tTJSNI_PsbFile();
}

tTJSNativeClass* TVPCreateNativeClass_PsbFile()
{
    return new tTJSNC_PsbFile();
}
