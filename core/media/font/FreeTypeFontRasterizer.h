
#ifndef __FREE_TYPE_FONT_RASTERIZER_H__
#define __FREE_TYPE_FONT_RASTERIZER_H__

#include "tjsCommHead.h"
#include "CharacterData.h"
#include "FontRasterizer.h"

#include <memory>
#include <unordered_map>
#include <vector>

class FreeTypeFontRasterizer : public FontRasterizer
{
    tjs_int RefCount;
    static constexpr size_t MaxCachedFaces = 4;
    class tFreeTypeFace* Face; //!< Non-owning pointer to the active cached face.
    std::vector<std::unique_ptr<class tFreeTypeFace>> FaceCache;
    // Faces tried, in order, for characters Face lacks: the default font,
    // then every other registered font in registration order. A file face
    // registered under several names is listed once. Opened on first use.
    struct Fallback
    {
        ttstr Name; //!< The first name it was registered under; opens it.
        ttstr Path;
        int Index = 0; //!< Face index within Path.
        std::unique_ptr<class tFreeTypeFace> Face;
        bool Failed = false;
        tjs_int Height = -1; //!< Pixel size last set on Face.
    };
    std::vector<Fallback> Fallbacks;
    tjs_uint ListedFontCount = 0;
    // Per character Face lacks: the Fallbacks index that has it, or -1.
    std::unordered_map<tjs_wchar, int> FallbackForChar;
    bool Antialiased = true;
    bool Hinting = true;
    class tTVPNativeBaseBitmap* LastBitmap;
    tTVPFont CurrentFont;
    void ListFallbacks();
    void ClearFallbacks();
    void ApplyFaceOptions(Fallback& fallback);
    // The fallback face, with the current options, that has ch; null when no
    // fallback has it.
    class tFreeTypeFace* FindFallback(tjs_wchar ch);

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
