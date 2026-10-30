#pragma once

#include "tjsNative.h"
#include "tjsDictionary.h"

#include "PSBMedia.h"
#include "PSBData.h"

class tTVPMemoryStream;

// Reads a whole PSB into memory, decompressing an MDF wrapper. Takes
// ownership of src (deleted, also on failure). Returns null on failure.
tTVPMemoryStream* PSBReadDecompressed(tTJSBinaryStream* src);

//---------------------------------------------------------------------------
// tTJSNI_PsbFile
//---------------------------------------------------------------------------
class tTJSNI_PsbFile : public tTJSNativeInstance
{
    typedef tTJSNativeInstance inherited;

public:
    tTJSNI_PsbFile();
    tjs_error Construct(tjs_int numparams, tTJSVariant** param, iTJSDispatch2* tjs_obj);
    void Invalidate();
    void Destruct() override;

    bool load(const ttstr& filePath);
    bool loadFromStream(tTJSBinaryStream* filePtr, const ttstr& filePath);
    tTJSVariant root();
    tjs_uint32 readListInfo(std::vector<tjs_uint32>* target);
    void refreshListInfo(std::vector<tjs_uint32>* target1, std::vector<tjs_uint32>* target2);
    tTJSVariant readAllObjs(const ttstr& key, tjs_uint32 _objOffset);

    // Reads the resource names and offsets of a decompressed PSB into out,
    // leaving each Data void. Builds no TJS objects and registers nothing
    // with psb://, so it can run on loader threads. data is not modified.
    static bool scanResources(const tjs_uint8* data, tjs_uint64 size, PSB::PSBMediaInfo& out);

private:
    iTJSDispatch2* Owner;
    // Only collect _resources; readAllObjs builds no TJS values.
    bool _scanOnly = false;
    // The decompressed PSB, only while load parses it.
    tTJSBinaryStream* filePtr = nullptr;
    const tjs_uint8* fileData = nullptr;
    tjs_uint64 fileSize = 0;
    // psb:// key this instance registered, if any.
    ttstr _mediaKey;

    void releaseLoad();
    // Drops the tree; its containers are freed over the next frames.
    void releaseTree();
    void detachMedia();
    void dropMedia();
    // Reads the header and tables of filePtr; false if they are unusable.
    bool parseTables();

    PSB::PSBHeader _header;
    std::vector<tjs_uint32> stringsOffset;
    std::vector<tjs_uint32> namesData;
    std::vector<tjs_uint32> charset;
    std::vector<tjs_uint32> nameIndexes;
    std::vector<ttstr> namesCache;

    std::vector<tjs_uint32> chunkOffsets;
    std::vector<tjs_uint32> chunkLengths;

    std::vector<tjs_uint32> extraChunkOffsets;
    std::vector<tjs_uint32> extraChunkLengths;

    PSB::PSBMediaInfo _resources;
    tTJSVariant _root;
    // Every array and dictionary of the tree, each with a reference of its
    // own. Releasing the root then frees only the root's storage, and the
    // rest goes one container at a time instead of one 125 ms cascade.
    std::vector<iTJSDispatch2*> _containers;
    // String values by string table index, built once per load. Scenario
    // files repeat the same strings many times.
    std::vector<tTJSVariant> _stringValues;
    std::vector<bool> _stringBuilt;
};

//---------------------------------------------------------------------------
// tTJSNC_PsbFile
//---------------------------------------------------------------------------
class tTJSNC_PsbFile : public tTJSNativeClass
{
    typedef tTJSNativeClass inherited;

public:
    tTJSNC_PsbFile();

    static tjs_uint32 ClassID;

protected:
    tTJSNativeInstance* CreateNativeInstance();
};
//---------------------------------------------------------------------------
tTJSNativeClass* TVPCreateNativeClass_PsbFile();
