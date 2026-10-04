#include "tjsCommHead.h"
#include "PSBData.h"
#include "Platform.h"

namespace PSB
{

bool PSBHeader::isEncrypted() const
{
    return offsetEncrypt > MAX_LENGTH + 16 || offsetNames == 0 ||
           (version > 1 && offsetEncrypt != offsetNames && offsetEncrypt != 0);
}

tjs_uint32 PSBHeader::getHeaderLength() const
{
    if (version < 3)
        return 40u;
    if (version > 3)
        return 56u;
    return 44u;
}

bool PSBHeader::parsePSBHeader(TJS::tTJSBinaryStream* stream)
{
    stream->ReadBuffer(signature, 4);
    stream->ReadBuffer(&version, 2);
    stream->ReadBuffer(&encrypt, 2);
    stream->ReadBuffer(&offsetEncrypt, 4);
    stream->ReadBuffer(&offsetNames, 4);

    if (!memcmp(signature, "MDF", 3) || !memcmp(signature, "MFL", 3))
    {
        TVPConsoleLog("Maybe a MDF file");
        return false;
    }
    if (memcmp(signature, "PSB", 3))
    {
        TVPConsoleLog("Not a valid PSB file");
        return false;
    }
    if (offsetNames < stream->GetSize())
    {
        stream->ReadBuffer(&offsetStrings, 4);
        stream->ReadBuffer(&offsetStringsData, 4);
        stream->ReadBuffer(&offsetChunkOffsets, 4);
        stream->ReadBuffer(&offsetChunkLengths, 4);
        stream->ReadBuffer(&offsetChunkData, 4);
        stream->ReadBuffer(&offsetEntries, 4);

        if (version > 2)
        {
            stream->ReadBuffer(&checksum, 4);
        }
        if (version > 3)
        {
            stream->ReadBuffer(&offsetExtraChunkOffsets, 4);
            stream->ReadBuffer(&offsetExtraChunkLengths, 4);
            stream->ReadBuffer(&offsetExtraChunkData, 4);
        }
    }
    return true;
}

bool parsePSBArray(std::vector<tjs_uint32>* target, tjs_int8 n, TJS::tTJSBinaryStream* stream)
{
    if (n < 0 || n > 8)
    {
        TVPConsoleLog("bad length type size");
        return false;
    }
    tjs_uint32 count = 0;
    stream->ReadBuffer(&count, n);
    if (count > INT32_MAX)
    {
        TVPConsoleLog("Long array is not supported yet");
        return false;
    }
    tjs_uint32 entryLength = stream->ReadI8LE() - static_cast<tjs_uint32>(PSBObjType::NumberN8);
    target->reserve(count);
    // One stream read per entry; the previous loop issued one virtual call per
    // byte. Only the low four bytes can survive in the 32-bit target, but the
    // stream still advances by the whole entry so later entries stay aligned.
    tjs_uint8 bytes[8] = {0};
    const tjs_uint32 scratch = entryLength < sizeof(bytes) ? entryLength : sizeof(bytes);
    const tjs_uint32 used = entryLength < 4 ? entryLength : 4;
    for (int i = 0; i < count; i++)
    {
        const tjs_uint got = scratch ? stream->Read(bytes, scratch) : 0;
        for (tjs_uint32 j = got; j < scratch; ++j)
            bytes[j] = 0;
        // A malformed length byte can exceed the scratch buffer; consume the
        // remainder one byte at a time rather than reading past it.
        for (tjs_uint32 j = scratch; j < entryLength; ++j)
            stream->ReadI8LE();
        tjs_uint32 result = 0;
        for (tjs_uint32 j = 0; j < used; ++j)
            result |= static_cast<tjs_uint32>(bytes[j]) << (j * 8);
        target->push_back(result);
    }
    return true;
}
} // namespace PSB
