#pragma once
#include "tjs.h"
#include "tjsHashSearch.h"
#include "TVPFontstruc.h"

void TVPGetAllFontList(std::vector<ttstr>& list);
void TVPInitFontNames();
int TVPEnumFontsProc(const ttstr& FontPath);
const ttstr& TVPGetDefaultFontName();
// faceIndex receives the face the name was registered from within its file.
tTJSBinaryStream* TVPCreateFontStream(const ttstr& fontname, int* faceIndex = nullptr);

struct TVPFontNamePathInfo
{
    ttstr Path;
    std::function<tTJSBinaryStream*(TVPFontNamePathInfo*)> Getter;
    int Index;
};
TVPFontNamePathInfo* TVPFindFont(const ttstr& name);
// The font TVPCreateFontStream opens for name: the registered one, else the default.
TVPFontNamePathInfo* TVPFindFontOrDefault(const ttstr& name);

//---------------------------------------------------------------------------
// font enumeration and existence check
//---------------------------------------------------------------------------
class tTVPttstrHash
{
public:
    static tjs_uint32 Make(const ttstr& val);
};
extern tTJSHashTable<ttstr, TVPFontNamePathInfo, tTVPttstrHash> TVPFontNames;

bool TVPFontExists(const ttstr& name);
ttstr TVPGetBeingFont(ttstr fonts);
void TVPCreateDefaultFont();
const tTVPFont& TVPGetDefaultFont();
