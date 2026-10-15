
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
    static constexpr size_t MaxCachedFaces = 4;
    class tFreeTypeFace* Face; //!< Non-owning pointer to the active cached face.
    std::vector<std::unique_ptr<tFreeTypeFace>> FaceCache;
    class tFreeTypeFace* FaceFallback = nullptr;
    class tTVPNativeBaseBitmap* LastBitmap;
    tTVPFont CurrentFont;
    void ApplyFallbackFace();

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
