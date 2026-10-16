
#ifndef __FREE_TYPE_FONT_RASTERIZER_H__
#define __FREE_TYPE_FONT_RASTERIZER_H__

#include "tjsCommHead.h"
#include "CharacterData.h"
#include "FontRasterizer.h"

#include <memory>
#include <vector>

class FreeTypeFontRasterizer : public FontRasterizer
{
    tjs_int RefCount;
    class tFreeTypeFace* Face; //!< Faceオブジェクト
    // Faces tried, in order, for characters Face lacks: the default font,
    // then every other registered font. Opened on first use.
    struct Fallback
    {
        ttstr Name;
        std::unique_ptr<class tFreeTypeFace> Face;
        bool Failed = false;
    };
    std::vector<Fallback> Fallbacks;
    bool FallbacksListed = false;
    bool Antialiased = true;
    bool Hinting = true;
    class tTVPNativeBaseBitmap* LastBitmap;
    tTVPFont CurrentFont;
    void ListFallbacks();
    void ClearFallbacks();
    void ApplyFaceOptions(class tFreeTypeFace* face);
    // The i-th fallback with the current options, or null when it can't be
    // opened. Returns false past the end of the chain.
    bool GetFallback(size_t i, class tFreeTypeFace*& face);

public:
    FreeTypeFontRasterizer();
    virtual ~FreeTypeFontRasterizer();
    void AddRef();
    void Release();
    void ApplyFont(class tTVPNativeBaseBitmap* bmp, bool force);
    void ApplyFont(const struct tTVPFont& font);
    void GetTextExtent(tjs_wchar ch, tjs_int& w, tjs_int& h);
    tjs_int GetAscentHeight();
    tTVPCharacterData* GetBitmap(const tTVPFontAndCharacterData& font,
                                 tjs_int aofsx,
                                 tjs_int aofsy);
    void GetGlyphDrawRect(std::vector<tjs_wchar> text, struct tTVPRect& area);
};

#endif // __FREE_TYPE_FONT_RASTERIZER_H__
