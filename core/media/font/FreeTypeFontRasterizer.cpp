#include "FreeTypeFontRasterizer.h"
#include "LayerBitmap.h"
#include "FreeTypeImp.h"
#include "TVPMsg.h"
#include "TVPFont.h"

extern void TVPUninitializeFreeFont();
extern const ttstr& TVPGetDefaultFontName();
static bool isUnicodeSpace(tjs_wchar ch);
// Zero-width format characters and variation selectors: no glyph, no advance.
static bool isDefaultIgnorableUnicode(tjs_wchar ch)
{
    return ch == 0x00AD || (ch >= 0x200B && ch <= 0x200F) || ch == 0x2060 ||
           (ch >= 0xFE00 && ch <= 0xFE0F) || ch == 0xFEFF;
}

void FreeTypeFontRasterizer::ApplyFaceOptions(tFreeTypeFace* face)
{
    face->SetHeight(CurrentFont.Height < 0 ? -CurrentFont.Height : CurrentFont.Height);
    const tjs_uint32 styles[] = {TVP_TF_ITALIC, TVP_TF_BOLD, TVP_TF_UNDERLINE, TVP_TF_STRIKEOUT};
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
    if (FallbacksListed)
        return;
    FallbacksListed = true;
    std::vector<ttstr> names;
    names.push_back(TVPGetDefaultFontName());
    TVPGetAllFontList(names);
    for (const ttstr& name : names)
    {
        if (name.IsEmpty())
            continue;
        bool known = false;
        for (const Fallback& fallback : Fallbacks)
            known = known || fallback.Name == name;
        if (!known)
        {
            Fallbacks.emplace_back();
            Fallbacks.back().Name = name;
        }
    }
}

void FreeTypeFontRasterizer::ClearFallbacks()
{
    Fallbacks.clear();
    FallbacksListed = false;
}

bool FreeTypeFontRasterizer::GetFallback(size_t i, tFreeTypeFace*& face)
{
    ListFallbacks();
    face = nullptr;
    if (i >= Fallbacks.size())
        return false;
    Fallback& fallback = Fallbacks[i];
    if (fallback.Failed || (Face && fallback.Name == Face->GetFontName()))
        return true;
    if (!fallback.Face)
    {
        try
        {
            fallback.Face.reset(new tFreeTypeFace(fallback.Name, 0));
        }
        catch (...)
        {
            fallback.Failed = true;
            return true;
        }
    }
    face = fallback.Face.get();
    ApplyFaceOptions(face);
    return true;
}

FreeTypeFontRasterizer::FreeTypeFontRasterizer() : RefCount(0), Face(NULL), LastBitmap(NULL)
{
    AddRef();
}
FreeTypeFontRasterizer::~FreeTypeFontRasterizer()
{
    if (Face)
        delete Face;
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
    bool recreate = false;
    if (Face)
    {
        if (Face->GetFontName() != stdname)
        {
            delete Face;
            Face = new tFreeTypeFace(stdname, opt);
            recreate = true;
        }
    }
    else
    {
        Face = new tFreeTypeFace(stdname, opt);
        recreate = true;
    }
    Face->SetHeight(font.Height < 0 ? -font.Height : font.Height);
    if (recreate == false)
    {
        if (font.Flags & TVP_TF_ITALIC)
        {
            Face->SetOption(TVP_TF_ITALIC);
        }
        else
        {
            Face->ClearOption(TVP_TF_ITALIC);
        }
        if (font.Flags & TVP_TF_BOLD)
        {
            Face->SetOption(TVP_TF_BOLD);
        }
        else
        {
            Face->ClearOption(TVP_TF_BOLD);
        }
        if (font.Flags & TVP_TF_UNDERLINE)
        {
            Face->SetOption(TVP_TF_UNDERLINE);
        }
        else
        {
            Face->ClearOption(TVP_TF_UNDERLINE);
        }
        if (font.Flags & TVP_TF_STRIKEOUT)
        {
            Face->SetOption(TVP_TF_STRIKEOUT);
        }
        else
        {
            Face->ClearOption(TVP_TF_STRIKEOUT);
        }
    }
    // Pick up fonts registered since the last listing (addFont).
    FallbacksListed = false;
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
        tFreeTypeFace* fallback;
        for (size_t i = 0; GetFallback(i, fallback); i++)
        {
            if (fallback && fallback->GetGlyphSizeFromCharcode(ch, metrics))
            {
                w = metrics.CellIncX;
                h = metrics.CellIncY;
                return;
            }
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
        tFreeTypeFace* fallback;
        for (size_t i = 0; !data && GetFallback(i, fallback); i++)
        {
            if (!fallback)
                continue;
            data = fallback->GetGlyphFromCharcode(font.Character);
            // Glyph origins are relative to their own face's ascent; put a
            // fallback glyph on the requested face's baseline.
            if (data)
                data->OriginY += Face->GetAscent() - fallback->GetAscent();
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
            tFreeTypeFace* fallback;
            for (size_t f = 0; !result && GetFallback(f, fallback); f++)
            {
                if (fallback && fallback->GetGlyphRectFromCharcode(rt, ch, ax, ay))
                {
                    rt.add_offsets(0, Face->GetAscent() - fallback->GetAscent());
                    result = true;
                }
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
