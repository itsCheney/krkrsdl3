#include "FreeTypeFontRasterizer.h"
#include "LayerBitmap.h"
#include "FreeTypeImp.h"
#include "TVPMsg.h"
#include "TVPFont.h"
#include <algorithm>

extern void TVPUninitializeFreeFont();
extern const ttstr& TVPGetDefaultFontName();
static bool isUnicodeSpace(tjs_wchar ch);
// Zero-width format characters and variation selectors: no glyph, no advance.
static bool isDefaultIgnorableUnicode(tjs_wchar ch)
{
    return ch == 0x00AD || (ch >= 0x200B && ch <= 0x200F) || ch == 0x2060 ||
           (ch >= 0xFE00 && ch <= 0xFE0F) || ch == 0xFEFF;
}

void FreeTypeFontRasterizer::ApplyFaceOptions(Fallback& fallback)
{
    tFreeTypeFace* face = fallback.Face.get();
    tjs_int height = CurrentFont.Height < 0 ? -CurrentFont.Height : CurrentFont.Height;
    // FT_Set_Pixel_Sizes is not free; only resize when the size changed.
    if (fallback.Height != height)
    {
        face->SetHeight(height);
        fallback.Height = height;
    }
    // Underline and strikeout are left to Face so the line matches the rest
    // of the run (GetBitmap, GetGlyphDrawRect).
    const tjs_uint32 styles[] = {TVP_TF_ITALIC, TVP_TF_BOLD};
    for (tjs_uint32 style : styles)
    {
        if (CurrentFont.Flags & style)
            face->SetOption(style);
        else
            face->ClearOption(style);
    }
    if (Antialiased)
        face->ClearOption(TVP_FACE_OPTIONS_NO_ANTIALIASING);
    else
        face->SetOption(TVP_FACE_OPTIONS_NO_ANTIALIASING);
    if (Hinting)
        face->ClearOption(TVP_FACE_OPTIONS_NO_HINTING);
    else
        face->SetOption(TVP_FACE_OPTIONS_NO_HINTING);
}

void FreeTypeFontRasterizer::ListFallbacks()
{
    // Relist only when fonts were registered since the last listing (addFont).
    if (TVPFontNames.GetCount() == ListedFontCount)
        return;
    ListedFontCount = TVPFontNames.GetCount();
    std::vector<ttstr> names;
    names.push_back(TVPGetDefaultFontName());
    // TVPFontNames iterates newest first; walk back for registration order.
    for (auto it = TVPFontNames.GetLast(); !it.IsNull(); --it)
        names.push_back(it.GetKey());
    bool added = false;
    for (const ttstr& name : names)
    {
        const TVPFontNamePathInfo* info = name.IsEmpty() ? nullptr : TVPFindFont(name);
        if (!info)
            continue;
        bool known = false;
        for (const Fallback& fallback : Fallbacks)
            known = known || (fallback.Path == info->Path && fallback.Index == info->Index);
        if (!known)
        {
            Fallbacks.emplace_back();
            Fallbacks.back().Name = name;
            Fallbacks.back().Path = info->Path;
            Fallbacks.back().Index = info->Index;
            added = true;
        }
    }
    // New faces go last, so earlier hits stay valid; only misses may change.
    if (added)
    {
        for (auto it = FallbackForChar.begin(); it != FallbackForChar.end();)
            it = it->second < 0 ? FallbackForChar.erase(it) : std::next(it);
    }
}

void FreeTypeFontRasterizer::ClearFallbacks()
{
    Fallbacks.clear();
    FallbackForChar.clear();
    ListedFontCount = 0;
}

tFreeTypeFace* FreeTypeFontRasterizer::FindFallback(tjs_wchar ch)
{
    ListFallbacks();
    int found = -1;
    auto cached = FallbackForChar.find(ch);
    if (cached != FallbackForChar.end())
    {
        found = cached->second;
    }
    else
    {
        // Face itself lacks ch; don't open a second copy of it.
        const TVPFontNamePathInfo* current =
            Face ? TVPFindFontOrDefault(Face->GetFontName()) : nullptr;
        for (size_t i = 0; found < 0 && i < Fallbacks.size(); i++)
        {
            Fallback& fallback = Fallbacks[i];
            if (fallback.Failed ||
                (current && fallback.Path == current->Path && fallback.Index == current->Index))
                continue;
            if (!fallback.Face)
            {
                try
                {
                    fallback.Face.reset(new tFreeTypeFace(fallback.Name, 0));
                }
                catch (...)
                {
                    fallback.Failed = true;
                    continue;
                }
            }
            if (fallback.Face->HasGlyph(ch))
                found = static_cast<int>(i);
        }
        FallbackForChar[ch] = found;
    }
    if (found < 0)
        return nullptr;
    ApplyFaceOptions(Fallbacks[found]);
    return Fallbacks[found].Face.get();
}

FreeTypeFontRasterizer::FreeTypeFontRasterizer() : RefCount(0), Face(NULL), LastBitmap(NULL)
{
    FaceCache.reserve(MaxCachedFaces);
    AddRef();
}
FreeTypeFontRasterizer::~FreeTypeFontRasterizer()
{
    Face = NULL;
    ClearFallbacks();
}
void FreeTypeFontRasterizer::AddRef()
{
    RefCount++;
}
//---------------------------------------------------------------------------
void FreeTypeFontRasterizer::Release()
{
    RefCount--;
    LastBitmap = NULL;
    if (RefCount == 0)
    {
        delete this;
    }
}
//---------------------------------------------------------------------------
void FreeTypeFontRasterizer::ApplyFont(class tTVPNativeBaseBitmap* bmp, bool force)
{
    if (bmp != LastBitmap || force)
    {
        ApplyFont(bmp->GetFont());
        LastBitmap = bmp;
    }
}
//---------------------------------------------------------------------------
void FreeTypeFontRasterizer::ApplyFont(const tTVPFont& font)
{
    CurrentFont = font;
    ttstr stdname = TVPGetBeingFont(font.Face);
    // TVP_FACE_OPTIONS_NO_ANTIALIASING
    // TVP_FACE_OPTIONS_NO_HINTING
    // TVP_FACE_OPTIONS_FORCE_AUTO_HINTING
    tjs_uint32 opt = 0;
    opt |= (font.Flags & TVP_TF_ITALIC) ? TVP_TF_ITALIC : 0;
    opt |= (font.Flags & TVP_TF_BOLD) ? TVP_TF_BOLD : 0;
    opt |= (font.Flags & TVP_TF_UNDERLINE) ? TVP_TF_UNDERLINE : 0;
    opt |= (font.Flags & TVP_TF_STRIKEOUT) ? TVP_TF_STRIKEOUT : 0;
    opt |= (font.Flags & TVP_TF_FONTFILE) ? TVP_FACE_OPTIONS_FILE : 0;
    if (!Face || Face->GetFontName() != stdname)
    {
        // Captions alternate font families frequently. Keep a small per-rasterizer
        // LRU so switching does not reopen and parse the same font every time.
        auto cached = std::find_if(FaceCache.begin(), FaceCache.end(),
                                  [&](const auto& face)
                                  { return face->GetFontName() == stdname; });
        if (cached != FaceCache.end())
        {
            std::rotate(cached, cached + 1, FaceCache.end());
        }
        else
        {
            // Construct first so an opening failure leaves the current face valid.
            auto face = std::make_unique<tFreeTypeFace>(stdname, opt);
            if (FaceCache.size() == MaxCachedFaces)
                FaceCache.erase(FaceCache.begin());
            FaceCache.push_back(std::move(face));
        }
        Face = FaceCache.back().get();
        // A reused face must start with the same sampling options as a new one.
        Face->ClearOption(TVP_FACE_OPTIONS_NO_ANTIALIASING | TVP_FACE_OPTIONS_NO_HINTING);
    }
    Face->ClearOption(TVP_TF_ITALIC | TVP_TF_BOLD | TVP_TF_UNDERLINE | TVP_TF_STRIKEOUT |
                      TVP_FACE_OPTIONS_FILE);
    Face->SetOption(opt);
    Face->SetHeight(font.Height < 0 ? -font.Height : font.Height);
    LastBitmap = NULL;
}
//---------------------------------------------------------------------------
void FreeTypeFontRasterizer::GetTextExtent(tjs_wchar ch, tjs_int& w, tjs_int& h)
{
    if (!Face)
        return;
    if (isDefaultIgnorableUnicode(ch))
    {
        w = h = 0;
        return;
    }
    tGlyphMetrics metrics;
    if (Face->GetGlyphSizeFromCharcode(ch, metrics))
    {
        w = metrics.CellIncX;
        h = metrics.CellIncY;
        return;
    }
    // Measure with the face GetBitmap will draw with, so a fallback glyph
    // advances by its own width rather than an em square.
    if (!isUnicodeSpace(ch))
    {
        tFreeTypeFace* fallback = FindFallback(ch);
        if (fallback && fallback->GetGlyphSizeFromCharcode(ch, metrics))
        {
            w = metrics.CellIncX;
            h = metrics.CellIncY;
            return;
        }
    }
    w = Face->GetHeight();
    h = w;
}
//---------------------------------------------------------------------------
tjs_int FreeTypeFontRasterizer::GetAscentHeight()
{
    if (Face)
        return Face->GetAscent();
    return 0;
}
static bool isUnicodeSpace(tjs_wchar ch)
{
    return (ch >= 0x0009 && ch <= 0x000D) || ch == 0x0020 || ch == 0x0085 || ch == 0x00A0 ||
           ch == 0x1680 || (ch >= 0x2000 && ch <= 0x200A) || ch == 0x2028 || ch == 0x2029 ||
           ch == 0x202F || ch == 0x205F || ch == 0x3000;
}
//---------------------------------------------------------------------------
tTVPCharacterData* FreeTypeFontRasterizer::GetBitmap(const tTVPFontAndCharacterData& font,
                                                     tjs_int aofsx,
                                                     tjs_int aofsy)
{
    if (isDefaultIgnorableUnicode(font.Character))
    {
        tTVPCharacterData* data = new tTVPCharacterData();
        data->OriginX = data->OriginY = 0;
        data->Metrics.CellIncX = data->Metrics.CellIncY = 0;
        data->Pitch = 0;
        data->BlackBoxX = data->BlackBoxY = 0;
        data->Antialiased = font.Antialiased;
        data->FullColored = false;
        data->Blured = font.Blured;
        data->BlurWidth = font.BlurWidth;
        data->BlurLevel = font.BlurLevel;
        return data;
    }
    // Fallback faces take these too (ApplyFaceOptions).
    Antialiased = font.Antialiased;
    Hinting = font.Hinting;
    if (font.Antialiased)
    {
        Face->ClearOption(TVP_FACE_OPTIONS_NO_ANTIALIASING);
    }
    else
    {
        Face->SetOption(TVP_FACE_OPTIONS_NO_ANTIALIASING);
    }
    if (font.Hinting)
    {
        Face->ClearOption(TVP_FACE_OPTIONS_NO_HINTING);
        // Face->SetOption( TVP_FACE_OPTIONS_FORCE_AUTO_HINTING );
    }
    else
    {
        Face->SetOption(TVP_FACE_OPTIONS_NO_HINTING);
        // Face->ClearOption( TVP_FACE_OPTIONS_FORCE_AUTO_HINTING );
    }
    tTVPCharacterData* data = Face->GetGlyphFromCharcode(font.Character);
    if (!data && !isUnicodeSpace(font.Character))
    {
        tFreeTypeFace* fallback = FindFallback(font.Character);
        if (fallback)
            data = fallback->GetGlyphFromCharcode(font.Character);
        if (data)
        {
            // Glyph origins are relative to their own face's ascent; put a
            // fallback glyph on the requested face's baseline, and draw the
            // requested face's underline / strikeout on it.
            data->OriginY += Face->GetAscent() - fallback->GetAscent();
            Face->AddLinesToGlyph(data);
        }
    }
    if (data == NULL)
    {
        data = Face->GetGlyphFromCharcode(Face->GetDefaultChar());
    }
    if (data == NULL)
    {
        data = Face->GetGlyphFromCharcode(Face->GetFirstChar());
    }
    if (data == NULL)
    {
        TVPThrowExceptionMessage(TVPFontRasterizeError);
    }

    int cx = data->Metrics.CellIncX;
    int cy = data->Metrics.CellIncY;
    if (font.Font.Angle == 0)
    {
        data->Metrics.CellIncX = cx;
        data->Metrics.CellIncY = 0;
    }
    else if (font.Font.Angle == 2700)
    {
        data->Metrics.CellIncX = 0;
        data->Metrics.CellIncY = cx;
    }
    else
    {
        double angle = font.Font.Angle * (M_PI / 1800);
        data->Metrics.CellIncX = static_cast<tjs_int>(std::cos(angle) * cx);
        data->Metrics.CellIncY = static_cast<tjs_int>(-std::sin(angle) * cx);
    }

    data->Antialiased = font.Antialiased;
    data->FullColored = false;
    data->Blured = font.Blured;
    data->BlurWidth = font.BlurWidth;
    data->BlurLevel = font.BlurLevel;
    data->OriginX += aofsx; // for vertical text
                            //	data->OriginY += aofsy;

    // apply blur
    if (font.Blured)
        data->Blur(); // nasty ...
    return data;
}
//---------------------------------------------------------------------------
void FreeTypeFontRasterizer::GetGlyphDrawRect(std::vector<tjs_wchar> text, tTVPRect& area)
{
    // アンチエイリアスとヒンティングは有効にする
    Face->ClearOption(TVP_FACE_OPTIONS_NO_ANTIALIASING);
    Face->ClearOption(TVP_FACE_OPTIONS_NO_HINTING);

    area.left = area.top = area.right = area.bottom = 0;
    tjs_int offsetx = 0;
    tjs_int offsety = 0;
    tjs_uint len = text.size();
    for (tjs_uint i = 0; i < len; i++)
    {
        tjs_wchar ch = text[i];
        tjs_int ax, ay;
        tTVPRect rt(0, 0, 0, 0);
        if (isDefaultIgnorableUnicode(ch))
            continue;
        bool result = Face->GetGlyphRectFromCharcode(rt, ch, ax, ay);
        if (result == false && !isUnicodeSpace(ch))
        {
            tFreeTypeFace* fallback = FindFallback(ch);
            if (fallback && fallback->GetGlyphRectFromCharcode(rt, ch, ax, ay))
            {
                rt.add_offsets(0, Face->GetAscent() - fallback->GetAscent());
                Face->AddLinesToRect(rt, ax);
                result = true;
            }
        }
        if (result == false)
            result = Face->GetGlyphRectFromCharcode(rt, Face->GetDefaultChar(), ax, ay);
        if (result == false)
            result = Face->GetGlyphRectFromCharcode(rt, Face->GetFirstChar(), ax, ay);
        if (result)
        {
            rt.add_offsets(offsetx, offsety);
            if (i != 0)
            {
                area.do_union(rt);
            }
            else
            {
                area = rt;
            }
        }
        offsetx += ax;
        offsety = 0;
    }
}
