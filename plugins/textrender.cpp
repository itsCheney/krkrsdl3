// textrender.dll: the TextRenderBase layout engine used by some KiriKiri Z
// titles instead of KAG text.
//
// Layout, timing and option handling follow the AetherKrkr port
// (https://github.com/AetherKiri/AetherKrkr, plugins/textrender.cpp,
// GPL-3.0-or-later), which in turn is based on the drop-in replacement by
// Hikaru Terazono (3c1u, https://github.com/3c1u/TextRender, MIT / Apache-2.0).
// Adapted here to UTF-8 tjs_char, the krkrsdl3 FreeTypeFontRasterizer, and
// with graphical characters, %p and overflow reporting kept from the previous
// krkrsdl3 implementation.

#include "ncbind/ncbind.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "FreeTypeFontRasterizer.h"
#include "Platform.h"

#define NCB_MODULE_NAME TJS_N("textrender.dll")

extern bool Layer_FetchImageSize(ttstr imageName, int& w, int& h);
extern iTJSDispatch2* TVPCreateRectObject(tjs_int left, tjs_int top, tjs_int right, tjs_int bottom);

using RgbColor = uint32_t;
using tr_string = std::u32string;

// -------------------------------------------------------------------
// UTF-8 <-> code points

static tr_string utf8ToCodePoints(const ttstr& s)
{
    tr_string out;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.c_str());
    if (!p)
        return out;
    while (*p)
    {
        char32_t c = *p;
        int extra = 0;
        if (c >= 0xF0)
            c &= 0x07, extra = 3;
        else if (c >= 0xE0)
            c &= 0x0F, extra = 2;
        else if (c >= 0xC0)
            c &= 0x1F, extra = 1;
        else if (c >= 0x80)
        {
            // stray continuation byte
            out.push_back(0xFFFD);
            ++p;
            continue;
        }
        ++p;
        for (int i = 0; i < extra; ++i)
        {
            if ((*p & 0xC0) != 0x80)
            {
                c = 0xFFFD;
                extra = 0;
                break;
            }
            c = (c << 6) | (*p & 0x3F);
            ++p;
        }
        out.push_back(c);
    }
    return out;
}

static void appendUtf8(std::string& out, char32_t c)
{
    if (c < 0x80)
        out.push_back(static_cast<char>(c));
    else if (c < 0x800)
    {
        out.push_back(static_cast<char>(0xC0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
    else if (c < 0x10000)
    {
        out.push_back(static_cast<char>(0xE0 | (c >> 12)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
    else
    {
        out.push_back(static_cast<char>(0xF0 | (c >> 18)));
        out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
}

static ttstr codePointsToTtstr(const tr_string& s)
{
    std::string out;
    for (char32_t c : s)
        appendUtf8(out, c);
    return ttstr(out.c_str());
}

static bool containsCodePoint(const tr_string& s, char32_t ch)
{
    return s.find(ch) != tr_string::npos;
}

// -------------------------------------------------------------------
// dictionary helpers

#define setprop_t(d, p, ty) \
    { \
        tTJSVariant v(ty(p)); \
        d->PropSet(TJS_MEMBERENSURE, TJS_N(#p), nullptr, &v, d); \
    }

#define setprop_opt_t(d, p, ty) \
    if (p != std::nullopt) \
    { \
        tTJSVariant v(ty(*p)); \
        d->PropSet(TJS_MEMBERENSURE, TJS_N(#p), nullptr, &v, d); \
    } \
    else \
    { \
        tTJSVariant v; \
        d->PropSet(TJS_MEMBERENSURE, TJS_N(#p), nullptr, &v, d); \
    }

#define setprop(d, p) setprop_t(d, p, )

#define getprop_t(d, p, ty) \
    { \
        tTJSVariant v; \
        if (TJS_SUCCEEDED(d->PropGet(0, TJS_N(#p), nullptr, &v, d)) && v.Type() != tvtVoid) \
        { \
            p = ty((tjs_int)(v)); \
        } \
    }

#define getprop(d, p) getprop_t(d, p, )

static bool variantIsString(const tTJSVariant& value)
{
    return value.Type() == tvtString;
}

static bool variantIsNumeric(const tTJSVariant& value)
{
    const tTJSVariantType type = value.Type();
    return type == tvtInteger || type == tvtReal;
}

static bool readObjectProp(iTJSDispatch2* object, const tjs_char* name, tTJSVariant& value)
{
    return object && TJS_SUCCEEDED(object->PropGet(0, name, nullptr, &value, object)) &&
           value.Type() != tvtVoid;
}

static bool readIntPropAliases(iTJSDispatch2* object,
                               std::initializer_list<const tjs_char*> names,
                               int& value)
{
    for (const auto* name : names)
    {
        tTJSVariant prop;
        if (!readObjectProp(object, name, prop) || !variantIsNumeric(prop))
            continue;
        value = static_cast<int>((tjs_int)prop);
        return true;
    }
    return false;
}

static bool readRealPropAliases(iTJSDispatch2* object,
                                std::initializer_list<const tjs_char*> names,
                                double& value)
{
    for (const auto* name : names)
    {
        tTJSVariant prop;
        if (!readObjectProp(object, name, prop) || !variantIsNumeric(prop))
            continue;
        value = prop.AsReal();
        return true;
    }
    return false;
}

static bool readStringPropAliases(iTJSDispatch2* object,
                                  std::initializer_list<const tjs_char*> names,
                                  ttstr& value)
{
    for (const auto* name : names)
    {
        tTJSVariant prop;
        if (!readObjectProp(object, name, prop) || !variantIsString(prop))
            continue;
        value = ttstr(prop);
        return true;
    }
    return false;
}

static bool readCodePointsProp(iTJSDispatch2* object, const tjs_char* name, tr_string& value)
{
    tTJSVariant prop;
    if (!readObjectProp(object, name, prop) || !variantIsString(prop))
        return false;
    value = utf8ToCodePoints(ttstr(prop));
    return true;
}

// -------------------------------------------------------------------

struct TextRenderState
{
    bool bold = false;
    bool italic = false;
    ttstr face = TJS_N("user");
    int fontSize = 24;
    double fontScale = 1.0;
    RgbColor chColor = 0xffffff;
    int rubySize = 10;
    int rubyOffset = -2;
    bool shadow = true;
    RgbColor shadowColor = 0x000000;
    bool edge = false;
    RgbColor edgeColor = 0x0080ff;
    int edgeExtent = 2;
    int edgeEmphasis = 2048;
    int lineSpacing = 6;
    int pitch = 0;
    int lineSize = 0;
    int align = -1;
    int valign = -1;

    tTJSVariant serialize() const
    {
        auto dict = TJSCreateDictionaryObject();
        setprop(dict, bold);
        setprop(dict, italic);
        setprop(dict, fontSize);
        setprop(dict, fontScale);
        setprop(dict, face);
        setprop_t(dict, chColor, static_cast<tjs_int>);
        setprop(dict, rubySize);
        setprop(dict, rubyOffset);
        setprop(dict, shadow);
        setprop_t(dict, shadowColor, static_cast<tjs_int>);
        setprop(dict, edge);
        setprop_t(dict, edgeColor, static_cast<tjs_int>);
        setprop(dict, edgeExtent);
        setprop(dict, edgeEmphasis);
        setprop(dict, lineSpacing);
        setprop(dict, pitch);
        setprop(dict, lineSize);
        setprop(dict, align);
        setprop(dict, valign);
        auto res = tTJSVariant(dict, dict);
        dict->Release();
        return res;
    }

    void deserialize(tTJSVariant t)
    {
        auto dict = t.AsObjectNoAddRef();
        if (!dict)
            return;
        getprop(dict, bold);
        getprop(dict, italic);
        readIntPropAliases(dict, {TJS_N("fontSize"), TJS_N("fontsize"), TJS_N("fontheight"), TJS_N("fontHeight")},
                           fontSize);
        readRealPropAliases(dict, {TJS_N("fontScale"), TJS_N("fontscale")}, fontScale);
        readStringPropAliases(dict, {TJS_N("face"), TJS_N("fontface"), TJS_N("fontFace")}, face);
        {
            int color = static_cast<int>(chColor);
            if (readIntPropAliases(dict, {TJS_N("chColor"), TJS_N("color")}, color))
                chColor = static_cast<RgbColor>(color);
        }
        getprop(dict, rubySize);
        getprop(dict, rubyOffset);
        getprop(dict, shadow);
        getprop_t(dict, shadowColor, static_cast<RgbColor>);
        getprop(dict, edge);
        getprop_t(dict, edgeColor, static_cast<RgbColor>);
        getprop(dict, edgeExtent);
        getprop(dict, edgeEmphasis);
        readIntPropAliases(dict, {TJS_N("lineSpacing"), TJS_N("linespacing")}, lineSpacing);
        getprop(dict, pitch);
        readIntPropAliases(dict, {TJS_N("lineSize"), TJS_N("linesize"), TJS_N("linestep"), TJS_N("lineStep")},
                           lineSize);
        readIntPropAliases(dict, {TJS_N("align"), TJS_N("halign")}, align);
        readIntPropAliases(dict, {TJS_N("valign")}, valign);
    }
};

struct TextRenderOptions
{
    // kinsoku: characters that must not start a line
    tr_string following{
        U"%),:;]}\uff61\uff63\uff9e\uff9f\u3002\uff0c\u3001\uff0e\uff1a\uff1b\u309b\u309c\u30fd\u30fe\u309d\u309e"
        U"\u3005\u2019\u201d\uff09\u3015\uff3d\uff5d\u3009\u300b\u300d\u300f\u3011\u00b0\u2032\u2033\u2103\uffe0"
        U"\uff05\u2030\u3000!.?\uff64\uff65\uff67\uff68\uff69\uff6a\uff6b\uff6c\uff6d\uff6e\uff6f\uff70\u30fb"
        U"\uff1f\uff01\u30fc\u3041\u3043\u3045\u3047\u3049\u3063\u3083\u3085\u3087\u308e\u30a1\u30a3\u30a5"
        U"\u30a7\u30a9\u30c3\u30e3\u30e5\u30e7\u30ee\u30f5\u30f6"};
    // kinsoku: characters that must not end a line
    tr_string leading{U"\\$([{\uff62\u2018\u201c\uff08\u3014\uff3b\uff5b\u3008\u300a\u300c\u300e\u3010\uffe5\uff04\uffe1"};
    // auto-indent open / close brackets
    tr_string begin{U"\u300c\u300e\uff08\u2018\u201c\u3014\uff3b\uff5b\u3008\u300a"};
    tr_string end{U"\u300d\u300f\uff09\u2019\u201d\u3015\uff3d\uff5d\u3009\u300b"};
    bool ignoreDelay = false;
    bool widthTimeScale = false;

    void deserialize(tTJSVariant t)
    {
        auto dict = t.AsObjectNoAddRef();
        if (!dict)
            return;
        readCodePointsProp(dict, TJS_N("following"), following);
        readCodePointsProp(dict, TJS_N("leading"), leading);
        readCodePointsProp(dict, TJS_N("begin"), begin);
        readCodePointsProp(dict, TJS_N("end"), end);
        {
            int value = ignoreDelay ? 1 : 0;
            if (readIntPropAliases(dict, {TJS_N("ignore_delay"), TJS_N("ignoreDelay")}, value))
                ignoreDelay = value != 0;
        }
        {
            int value = widthTimeScale ? 1 : 0;
            if (readIntPropAliases(dict, {TJS_N("width_time_scale"), TJS_N("widthTimeScale")}, value))
                widthTimeScale = value != 0;
        }
    }
};

struct CharacterInfo
{
    bool bold = false;
    bool italic = false;
    bool graph = false;
    bool vertical = false;
    ttstr face = TJS_N("user");
    // position after alignment, as reported to scripts
    int x = 0;
    int y = 0;
    // position from line layout, before alignment
    int layoutX = 0;
    int layoutY = 0;
    int cw = 0;
    int size = 0;
    RgbColor color = 0xffffff;
    std::optional<RgbColor> edge = std::nullopt;
    int edgeExtent = 0;
    int edgeEmphasis = 0;
    std::optional<RgbColor> shadow = std::nullopt;
    ttstr text;
    double delay = 0.0;

    tTJSVariant serialize() const
    {
        auto dict = TJSCreateDictionaryObject();
        setprop(dict, bold);
        setprop(dict, italic);
        setprop(dict, graph);
        setprop(dict, vertical);
        setprop(dict, x);
        setprop(dict, y);
        setprop(dict, cw);
        setprop(dict, size);
        setprop(dict, face);
        setprop_t(dict, color, static_cast<tjs_int>);
        setprop_opt_t(dict, edge, static_cast<tjs_int>);
        setprop(dict, edgeExtent);
        setprop(dict, edgeEmphasis);
        setprop_opt_t(dict, shadow, static_cast<tjs_int>);
        setprop(dict, text);
        setprop(dict, delay);
        auto res = tTJSVariant(dict, dict);
        dict->Release();
        return res;
    }
};

struct TextRenderKeyWait
{
    int pos = 0;
    double time = 0.0;
};

#define property_accessor(name, type, storage) \
    type get_##name() const { return storage; } \
    void set_##name(type v) { storage = v; }

#define property_accessor_cast(name, type, cast, storage) \
    cast get_##name() const { return cast(storage); } \
    void set_##name(cast v) { storage = type(v); }

#define property_accessor_font(name, type, storage) \
    type get_##name() const { return storage; } \
    void set_##name(type v) \
    { \
        storage = v; \
        m_fontDirty = true; \
    }

#define property_accessor_string(name, storage, dirty) \
    tTJSVariant get_##name() const { return tTJSVariant(storage); } \
    void set_##name(tTJSVariant v) \
    { \
        storage = v.Type() == tvtVoid ? ttstr() : ttstr(v); \
        if (dirty) \
            m_fontDirty = true; \
    }

#define property_delegate(name) NCB_PROPERTY(name, get_##name, set_##name);

enum TextRenderMode
{
    kTextRenderModeLeading = 0,
    kTextRenderModeNormal,
    kTextRenderModeFollowing,
};

class TextRenderBase
{
public:
    TextRenderBase() : m_rasterizer(new FreeTypeFontRasterizer()) {}
    virtual ~TextRenderBase()
    {
        if (m_rasterizer)
        {
            m_rasterizer->Release();
            m_rasterizer = nullptr;
        }
    }

    bool render(const ttstr& text, int autoIndent, int diff, int all, bool same, bool plain,
                iTJSDispatch2* callbackThis);
    static tjs_error renderCallback(tTJSVariant* result,
                                    tjs_int numparams,
                                    tTJSVariant** param,
                                    iTJSDispatch2* objthis);
    void setRenderSize(int width, int height);
    void setDefault(tTJSVariant defaultSettings);
    void setOption(tTJSVariant options);
    tTJSVariant getCharacters(int start, int end);
    void clear();
    void done();
    void newline();

    tTJSVariant getKeyWait();
    double get_renderDelay() const;
    int calcShowCount(int elapsed);
    int get_renderCount() const;
    void set_renderCount(int value);
    void redraw();
    tTJSVariant renderText(tTJSVariant text);
    void resetStyle();
    void resetFont();
    bool get_renderOver() const { return m_overflow; }
    void setFontScale(double v) { set_fontScale(v); }
    tTJSVariant getRect();

    property_accessor(vertical, bool, m_vertical);
    property_accessor_font(bold, bool, m_state.bold);
    property_accessor_font(italic, bool, m_state.italic);
    property_accessor_string(face, m_state.face, true);
    property_accessor_font(fontSize, int, m_state.fontSize);
    property_accessor_font(fontScale, double, m_state.fontScale);
    property_accessor_cast(chColor, RgbColor, tjs_int, m_state.chColor);
    property_accessor(rubySize, int, m_state.rubySize);
    property_accessor(rubyOffset, int, m_state.rubyOffset);
    property_accessor(shadow, bool, m_state.shadow);
    property_accessor_cast(shadowColor, RgbColor, tjs_int, m_state.shadowColor);
    property_accessor(edge, bool, m_state.edge);
    property_accessor_cast(edgeColor, RgbColor, tjs_int, m_state.edgeColor);
    property_accessor(edgeExtent, int, m_state.edgeExtent);
    property_accessor(edgeEmphasis, int, m_state.edgeEmphasis);
    property_accessor(lineSpacing, int, m_state.lineSpacing);
    property_accessor(pitch, int, m_state.pitch);
    property_accessor(lineSize, int, m_state.lineSize);
    property_accessor(align, int, m_state.align);
    property_accessor(valign, int, m_state.valign);
    property_accessor(timeScale, double, m_timeScale);

    property_accessor(defaultBold, bool, m_default.bold);
    property_accessor(defaultItalic, bool, m_default.italic);
    property_accessor_string(defaultFace, m_default.face, false);
    property_accessor(defaultFontSize, int, m_default.fontSize);
    property_accessor(defaultFontScale, double, m_default.fontScale);
    property_accessor_cast(defaultChColor, RgbColor, tjs_int, m_default.chColor);
    property_accessor(defaultRubySize, int, m_default.rubySize);
    property_accessor(defaultRubyOffset, int, m_default.rubyOffset);
    property_accessor(defaultShadow, bool, m_default.shadow);
    property_accessor_cast(defaultShadowColor, RgbColor, tjs_int, m_default.shadowColor);
    property_accessor(defaultEdge, bool, m_default.edge);
    property_accessor_cast(defaultEdgeColor, RgbColor, tjs_int, m_default.edgeColor);
    property_accessor(defaultEdgeExtent, int, m_default.edgeExtent);
    property_accessor(defaultEdgeEmphasis, int, m_default.edgeEmphasis);
    property_accessor(defaultLineSpacing, int, m_default.lineSpacing);
    property_accessor(defaultPitch, int, m_default.pitch);
    property_accessor(defaultLineSize, int, m_default.lineSize);
    property_accessor(defaultAlign, int, m_default.align);
    property_accessor(defaultValign, int, m_default.valign);

    int get_renderLeft() { return getRenderedBounds().left; }
    int get_renderTop() { return getRenderedBounds().top; }
    int get_renderWidth()
    {
        const auto bounds = getRenderedBounds();
        return bounds.right - bounds.left;
    }
    int get_renderHeight()
    {
        const auto bounds = getRenderedBounds();
        return bounds.bottom - bounds.top;
    }
    int get_renderRight() { return getRenderedBounds().right; }
    int get_renderBottom() { return getRenderedBounds().bottom; }

private:
    struct RenderBounds
    {
        int left;
        int top;
        int right;
        int bottom;
    };

    FontRasterizer* m_rasterizer;
    int m_cachedAscentHeight = 0;
    bool m_fontDirty = true;
    bool m_layoutDirty = false;

    int m_boxWidth = 0;
    int m_boxHeight = 0;
    int m_x = 0;
    int m_y = 0;
    int m_indent = 0;
    int m_autoIndent = 0;
    int m_renderCountOverride = -1;
    bool m_overflow = false;
    bool m_isBeginningOfLine = true;
    bool m_vertical = false;
    double m_timeScale = 1.0;
    double m_baseCharacterDelay = 0.0;
    double m_characterDelay = 0.0;
    double m_currentDelay = 0.0;
    double m_renderDelay = 0.0;
    size_t m_timingSegmentStart = 0;
    double m_timingSegmentStartDelay = 0.0;
    size_t m_sameKeyWaitIndex = 0;
    bool m_sameTiming = false;

    TextRenderOptions m_options{};
    TextRenderState m_default{};
    TextRenderState m_state{};

    std::vector<CharacterInfo> m_characters{};
    std::vector<CharacterInfo> m_buffer{};
    std::vector<TextRenderKeyWait> m_keyWaits{};
    uint32_t m_mode = kTextRenderModeLeading;

    void pushCharacter(char32_t ch);
    void pushGraphicalCharacter(const ttstr& graph);
    void performLinebreak();
    void flush(bool force = false);
    void applyFont();
    void applyAlignment();
    int getEffectiveLineSpacing() const;
    int getEffectiveFontHeight() const;
    int getAscentHeight();
    RenderBounds getRenderedBounds();
    size_t getCharacterCount() const;
    void addDelay(double delay);
    void synchronizeTiming(double targetTime);
    void updateOverflow();
    std::optional<double> resolveDelayLabel(iTJSDispatch2* callbackThis, const tr_string& label) const;
};

// -------------------------------------------------------------------
// parser helpers; `i` indexes the current character and is advanced first

static bool readchar(const tr_string& str, size_t& i, char32_t& c)
{
    if (++i >= str.size())
        return false;
    c = str[i];
    return true;
}

static void read_integer(const tr_string& str, size_t& i, int& value)
{
    char32_t ch;
    bool is_negative = false;
    while (true)
    {
        if (!readchar(str, i, ch))
            TVPThrowExceptionMessage(
                TJS_N("TextRenderBase::render() parse error: expected integer or ';', found EOF"));
        if ('0' <= ch && ch <= '9')
        {
            value = value * 10 + static_cast<int>(ch - '0');
            continue;
        }
        if (ch == '-')
        {
            is_negative = !is_negative;
            continue;
        }
        if (ch == ';')
        {
            if (is_negative)
                value = -value;
            return;
        }
        TVPThrowExceptionMessage(TJS_N("TextRenderBase::render() parse error: unexpected char"));
    }
}

// Reads digits up to ';'. An empty token (e.g. "%d;") returns fallback.
static int read_optional_integer(const tr_string& str, size_t& i, int fallback)
{
    char32_t ch;
    bool negative = false;
    bool hasValue = false;
    int value = 0;
    while (true)
    {
        if (!readchar(str, i, ch))
            TVPThrowExceptionMessage(
                TJS_N("TextRenderBase::render() parse error: expected integer or ';', found EOF"));
        if (ch == ';')
            break;
        if ('0' <= ch && ch <= '9')
        {
            hasValue = true;
            value = value * 10 + static_cast<int>(ch - '0');
        }
        else if (ch == '-')
            negative = !negative;
        else
            TVPThrowExceptionMessage(TJS_N("TextRenderBase::render() parse error: unexpected char"));
    }
    if (!hasValue)
        return fallback;
    return negative ? -value : value;
}

static tr_string read_until_semicolon(const tr_string& str, size_t& i, const tjs_char* command)
{
    tr_string token;
    char32_t ch;
    while (true)
    {
        if (!readchar(str, i, ch))
            TVPThrowExceptionMessage(TJS_N("TextRenderBase::render() parse error: unterminated %1"), command);
        if (ch == ';')
            return token;
        token += ch;
    }
}

static tr_string read_delay_label(const tr_string& str, size_t& i, const tjs_char* command)
{
    char32_t ch;
    if (!readchar(str, i, ch) || ch != '$')
        TVPThrowExceptionMessage(TJS_N("TextRenderBase::render() parse error: expected '$' after %1"), command);
    return read_until_semicolon(str, i, command);
}

static bool read_flag(const tr_string& str, size_t& i, bool default_value, const tjs_char* command)
{
    char32_t ch;
    if (!readchar(str, i, ch))
        TVPThrowExceptionMessage(TJS_N("TextRenderBase::render() parse error: expected flag after %1"), command);
    if (ch == '0')
        return false;
    if (ch == '1')
        return true;
    return default_value;
}

// -------------------------------------------------------------------

tjs_error TextRenderBase::renderCallback(tTJSVariant* result,
                                         tjs_int numparams,
                                         tTJSVariant** param,
                                         iTJSDispatch2* objthis)
{
    if (!objthis)
        return TJS_E_NATIVECLASSCRASH;
    if (numparams < 1 || !param || !param[0])
        return TJS_E_BADPARAMCOUNT;

    auto* self = ncbInstanceAdaptor<TextRenderBase>::GetNativeInstance(objthis);
    if (!self)
        return TJS_E_NATIVECLASSCRASH;

    const auto intParam = [&](tjs_int index, int fallback)
    {
        return index < numparams && param[index] && param[index]->Type() != tvtVoid
                   ? static_cast<int>(static_cast<tjs_int>(*param[index]))
                   : fallback;
    };
    const int autoIndent = intParam(1, 0);
    const int diff = intParam(2, 0);
    const int all = intParam(3, 0);
    const bool same = intParam(4, 0) != 0;
    const bool plain = intParam(5, 0) != 0;

    const bool rendered = self->render(ttstr(*param[0]), autoIndent, diff, all, same, plain, objthis);
    if (result)
        *result = rendered;
    return TJS_S_OK;
}

void TextRenderBase::applyFont()
{
    if (!m_fontDirty)
        return;
    m_fontDirty = false;

    tTVPFont font;
    font.Height = static_cast<tjs_int>(getEffectiveFontHeight());
    font.Flags =
        static_cast<tjs_uint32>((m_state.bold ? TVP_TF_BOLD : 0) | (m_state.italic ? TVP_TF_ITALIC : 0));
    font.Angle = 0;
    font.Face = m_state.face;
    m_rasterizer->ApplyFont(font);
    m_cachedAscentHeight = m_rasterizer->GetAscentHeight();
}

int TextRenderBase::getEffectiveLineSpacing() const
{
    return m_state.lineSpacing;
}

int TextRenderBase::getEffectiveFontHeight() const
{
    const double scale = std::max(m_state.fontScale, 0.001);
    return std::max(1, static_cast<int>(std::lround(m_state.fontSize * scale)));
}

int TextRenderBase::getAscentHeight()
{
    applyFont();
    return m_cachedAscentHeight;
}

size_t TextRenderBase::getCharacterCount() const
{
    return m_characters.size() + m_buffer.size();
}

void TextRenderBase::addDelay(double delay)
{
    if (m_options.ignoreDelay)
        return;
    m_currentDelay += std::max(delay, 0.0);
}

void TextRenderBase::synchronizeTiming(double targetTime)
{
    targetTime = std::max(targetTime, m_timingSegmentStartDelay);
    const double sourceDuration = std::max(m_currentDelay - m_timingSegmentStartDelay, 0.0);
    const double targetDuration = targetTime - m_timingSegmentStartDelay;
    const double scale = sourceDuration > 0.0 ? targetDuration / sourceDuration : 1.0;

    size_t index = 0;
    const auto scaleCharacter = [&](CharacterInfo& character)
    {
        if (index++ < m_timingSegmentStart)
            return;
        character.delay = m_timingSegmentStartDelay + (character.delay - m_timingSegmentStartDelay) * scale;
    };
    for (auto& character : m_characters)
        scaleCharacter(character);
    for (auto& character : m_buffer)
        scaleCharacter(character);
    for (auto& wait : m_keyWaits)
    {
        if (wait.pos < static_cast<int>(m_timingSegmentStart))
            continue;
        wait.time = m_timingSegmentStartDelay + (wait.time - m_timingSegmentStartDelay) * scale;
    }

    m_currentDelay = targetTime;
    m_renderDelay = std::max(m_renderDelay, targetTime);
    m_timingSegmentStart = getCharacterCount();
    m_timingSegmentStartDelay = targetTime;
}

std::optional<double> TextRenderBase::resolveDelayLabel(iTJSDispatch2* callbackThis,
                                                        const tr_string& label) const
{
    if (!callbackThis)
        return std::nullopt;

    tTJSVariant labelValue(codePointsToTtstr(label));
    tTJSVariant* params[] = {&labelValue};
    tTJSVariant resolved;
    const tjs_error status =
        callbackThis->FuncCall(0, TJS_N("onLabel"), nullptr, &resolved, 1, params, callbackThis);
    if (TJS_FAILED(status) || (resolved.Type() != tvtInteger && resolved.Type() != tvtReal))
        return std::nullopt;
    return static_cast<double>(resolved.AsReal());
}

void TextRenderBase::updateOverflow()
{
    if (m_boxHeight <= 0 || getCharacterCount() == 0)
    {
        m_overflow = false;
        return;
    }
    m_overflow = m_y + getAscentHeight() > m_boxHeight;
}

bool TextRenderBase::render(const ttstr& source, int autoIndent, int diff, int all, bool same, bool plain,
                            iTJSDispatch2* callbackThis)
{
    const tr_string text = utf8ToCodePoints(source);

    // A new message keeps the already laid-out glyphs, but their reveal time
    // is reset to zero, so only the newly supplied text animates. `same` is
    // used for concurrently rendered language tracks: they share the first
    // track's key-wait anchors but each track starts at time zero.
    flush();
    if (!same)
    {
        for (auto& character : m_characters)
            character.delay = 0.0;
        m_keyWaits.clear();
        m_renderDelay = 0.0;
    }
    m_currentDelay = 0.0;

    m_autoIndent = autoIndent;
    m_sameTiming = same;
    m_sameKeyWaitIndex = 0;
    m_baseCharacterDelay = m_options.ignoreDelay ? 0.0 : std::max(static_cast<double>(diff), 0.0);
    if (all > 0 && m_baseCharacterDelay == 0.0 && !m_options.ignoreDelay)
    {
        // Keep distinct timestamps so the requested total time can be
        // distributed even when the caller did not provide a speed.
        m_baseCharacterDelay = 0.001;
    }
    m_characterDelay = m_baseCharacterDelay;
    m_timingSegmentStart = getCharacterCount();
    m_timingSegmentStartDelay = m_currentDelay;

    const size_t len = text.size();
    const auto finish = [&]()
    {
        if (all > 0 && !m_options.ignoreDelay)
            synchronizeTiming(static_cast<double>(all));
        else
            m_renderDelay = std::max(m_renderDelay, m_currentDelay);
        updateOverflow();
        return !m_overflow;
    };

    if (plain)
    {
        // The plain path recognizes LF only; every markup character is text.
        for (char32_t ch : text)
        {
            if (ch == '\n')
            {
                flush();
                performLinebreak();
            }
            else
                pushCharacter(ch);
        }
        return finish();
    }

    for (size_t i = 0; i < len; ++i)
    {
        char32_t ch = text[i];
        switch (ch)
        {
            case '%':
            {
                if (!readchar(text, i, ch))
                    TVPThrowExceptionMessage(TJS_N("TextRenderBase: parse error after %%"));

                switch (ch)
                {
                    case 'f':
                    {
                        tr_string fontname = read_until_semicolon(text, i, TJS_N("%f"));
                        if (!fontname.empty())
                            m_state.face = codePointsToTtstr(fontname);
                        m_fontDirty = true;
                        break;
                    }
                    case 'b':
                        m_state.bold = read_flag(text, i, m_default.bold, TJS_N("%b"));
                        m_fontDirty = true;
                        break;
                    case 'i':
                        m_state.italic = read_flag(text, i, m_default.italic, TJS_N("%i"));
                        m_fontDirty = true;
                        break;
                    case 's':
                        m_state.shadow = read_flag(text, i, m_default.shadow, TJS_N("%s"));
                        break;
                    case 'e':
                        m_state.edge = read_flag(text, i, m_default.edge, TJS_N("%e"));
                        break;
                    case 'p':
                    {
                        int value = 0;
                        read_integer(text, i, value);
                        m_state.pitch = value;
                        break;
                    }
                    case 'l':
                        // label definition; nothing to lay out
                        read_until_semicolon(text, i, TJS_N("%l"));
                        break;
                    case 'd':
                    {
                        // An empty %d; resets the speed to the caller supplied
                        // base delay. %d0; disables delay.
                        const int value = read_optional_integer(text, i, 100);
                        if (!m_options.ignoreDelay)
                            m_characterDelay = m_baseCharacterDelay * static_cast<double>(value) / 100.0;
                        break;
                    }
                    case 'a':
                    {
                        const int value =
                            read_optional_integer(text, i, static_cast<int>(m_baseCharacterDelay));
                        if (!m_options.ignoreDelay)
                            m_characterDelay = std::max(static_cast<double>(value), 0.0);
                        break;
                    }
                    case 'w':
                    {
                        if (i + 1 < len && text[i + 1] == '$')
                        {
                            const auto label = read_delay_label(text, i, TJS_N("%w"));
                            if (const auto value = resolveDelayLabel(callbackThis, label))
                                addDelay(*value);
                        }
                        else
                        {
                            int value = 0;
                            read_integer(text, i, value);
                            addDelay(m_baseCharacterDelay * static_cast<double>(value) / 100.0);
                        }
                        break;
                    }
                    case 't':
                    {
                        if (i + 1 < len && text[i + 1] == '$')
                        {
                            const auto label = read_delay_label(text, i, TJS_N("%t"));
                            if (const auto value = resolveDelayLabel(callbackThis, label))
                                addDelay(*value);
                        }
                        else
                        {
                            int value = 0;
                            read_integer(text, i, value);
                            addDelay(static_cast<double>(value));
                        }
                        break;
                    }
                    case 'D':
                    {
                        if (i + 1 < len && text[i + 1] == '$')
                        {
                            const auto label = read_delay_label(text, i, TJS_N("%D"));
                            if (!m_options.ignoreDelay)
                            {
                                if (const auto value = resolveDelayLabel(callbackThis, label))
                                    synchronizeTiming(*value);
                            }
                            break;
                        }
                        int value = 0;
                        read_integer(text, i, value);
                        if (!m_options.ignoreDelay)
                            synchronizeTiming(value);
                        break;
                    }
                    case 'n':
                    {
                        // KAGEX-compatible logical line break: %n[lines];
                        int lines = 0;
                        read_integer(text, i, lines);
                        if (lines == 0)
                            lines = 1;
                        flush();
                        performLinebreak();
                        for (int line = 0; line < lines; ++line)
                            performLinebreak();
                        break;
                    }
                    case 'r':
                        m_state = m_default;
                        m_fontDirty = true;
                        break;
                    case '0':
                    case '1':
                    case '2':
                    case '3':
                    case '4':
                    case '5':
                    case '6':
                    case '7':
                    case '8':
                    case '9':
                    {
                        int value = static_cast<int>(ch - '0');
                        read_integer(text, i, value);
                        m_state.fontSize = m_default.fontSize * value / 100;
                        m_fontDirty = true;
                        break;
                    }
                    default:
                        // %B %S %C %R %L and unknown commands are ignored
                        break;
                }
                break;
            }
            case '\\':
            {
                if (!readchar(text, i, ch))
                    TVPThrowExceptionMessage(TJS_N("TextRenderBase: parse error after backslash"));

                switch (ch)
                {
                    case 'n':
                        flush();
                        performLinebreak();
                        break;
                    case 't':
                        pushCharacter('\t');
                        break;
                    case 'i':
                        m_indent = m_x;
                        break;
                    case 'r':
                        m_indent = 0;
                        break;
                    case 'w':
                        pushCharacter(' ');
                        break;
                    case 'k':
                    {
                        const int position = static_cast<int>(getCharacterCount());
                        if (m_sameTiming && m_sameKeyWaitIndex < m_keyWaits.size())
                            synchronizeTiming(m_keyWaits[m_sameKeyWaitIndex++].time);
                        else if (!m_sameTiming)
                            m_keyWaits.push_back({position, m_currentDelay});
                        break;
                    }
                    case 'x':
                        break;
                    default:
                        pushCharacter(ch);
                        break;
                }
                break;
            }
            case '[':
            {
                // ruby text: consumed, not drawn yet
                while (true)
                {
                    if (!readchar(text, i, ch))
                        TVPThrowExceptionMessage(TJS_N("TextRenderBase: parse error in ruby []"));
                    if (ch == ']')
                        break;
                }
                break;
            }
            case '#':
            {
                RgbColor colour = 0x00;
                bool isNull = true;
                while (true)
                {
                    if (!readchar(text, i, ch))
                        TVPThrowExceptionMessage(TJS_N("TextRenderBase: parse error in color #"));
                    if (ch == ';')
                        break;
                    RgbColor c = 0;
                    if ('0' <= ch && ch <= '9')
                        c = static_cast<RgbColor>(ch - '0');
                    else if ('A' <= ch && ch <= 'F')
                        c = 0x0a + static_cast<RgbColor>(ch - 'A');
                    else if ('a' <= ch && ch <= 'f')
                        c = 0x0a + static_cast<RgbColor>(ch - 'a');
                    colour = (colour << 4) | c;
                    isNull = false;
                }
                m_state.chColor = isNull ? m_default.chColor : colour;
                break;
            }
            case '&':
            {
                tr_string graph = read_until_semicolon(text, i, TJS_N("&"));
                pushGraphicalCharacter(codePointsToTtstr(graph));
                break;
            }
            case '$':
            {
                // eval: not supported, skip the expression
                read_until_semicolon(text, i, TJS_N("$"));
                break;
            }
            case '\r':
                // CRLF counts as one line break; CR alone is also a break.
                if (i + 1 < len && text[i + 1] == '\n')
                    ++i;
                flush();
                performLinebreak();
                break;
            case '\n':
                flush();
                performLinebreak();
                break;
            default:
                pushCharacter(ch);
                break;
        }
    }

    return finish();
}

void TextRenderBase::performLinebreak()
{
    m_x = m_indent;
    m_isBeginningOfLine = true;
    m_y += getAscentHeight() + getEffectiveLineSpacing();
}

void TextRenderBase::pushGraphicalCharacter(const ttstr& graph)
{
    flush();

    int w = 0, h = 0;
    Layer_FetchImageSize(graph, w, h);

    CharacterInfo info;
    info.bold = m_state.bold;
    info.italic = m_state.italic;
    info.graph = true;
    info.face = m_state.face;
    info.cw = w;
    info.size = h;
    info.color = m_state.chColor;
    info.edge = m_state.edge ? std::make_optional(m_state.edgeColor) : std::nullopt;
    info.shadow = m_state.shadow ? std::make_optional(m_state.shadowColor) : std::nullopt;
    info.text = graph;
    info.delay = m_currentDelay;
    m_buffer.push_back(std::move(info));

    addDelay(m_characterDelay);
    m_mode = kTextRenderModeNormal;
    m_isBeginningOfLine = false;
}

void TextRenderBase::pushCharacter(char32_t ch)
{
    const bool isLeadingChar = containsCodePoint(m_options.leading, ch);
    const bool isFollowingChar = containsCodePoint(m_options.following, ch);
    const bool isIndent = containsCodePoint(m_options.begin, ch);
    const bool isIndentDecr = containsCodePoint(m_options.end, ch);

    uint32_t current;
    if (isLeadingChar)
        current = kTextRenderModeLeading;
    else if (isFollowingChar)
        current = kTextRenderModeFollowing;
    else
        current = kTextRenderModeNormal;

    // Kinsoku: characters stay buffered together, and move to the next line
    // as one unit, when the previous one must not end a line (an opening
    // bracket) or this one must not start a line (closing punctuation).
    if (m_mode != kTextRenderModeLeading && current != kTextRenderModeFollowing)
        flush();

    applyFont();

    int advance_width = 0, advance_height = 0;
    if (ch <= 0xFFFF)
        m_rasterizer->GetTextExtent(static_cast<tjs_wchar>(ch), advance_width, advance_height);
    else
        advance_width = getEffectiveFontHeight(); // the rasterizer only takes BMP characters

    CharacterInfo info;
    info.bold = m_state.bold;
    info.italic = m_state.italic;
    info.face = m_state.face;
    info.cw = advance_width;
    info.size = getEffectiveFontHeight();
    info.color = m_state.chColor;
    info.edge = m_state.edge ? std::make_optional(m_state.edgeColor) : std::nullopt;
    info.edgeExtent = m_state.edge ? m_state.edgeExtent : 0;
    info.edgeEmphasis = m_state.edge ? m_state.edgeEmphasis : 0;
    info.shadow = m_state.shadow ? std::make_optional(m_state.shadowColor) : std::nullopt;
    info.text = codePointsToTtstr(tr_string(1, ch));
    info.delay = m_currentDelay;
    m_buffer.push_back(std::move(info));

    double characterDelay = m_characterDelay;
    if (m_options.widthTimeScale)
    {
        characterDelay *= static_cast<double>(std::max(advance_width, 0)) /
                          static_cast<double>(std::max(getEffectiveFontHeight(), 1));
    }
    addDelay(characterDelay);

    if (m_autoIndent)
    {
        // hanging indent for a line that starts with an opening bracket
        if (m_isBeginningOfLine && m_autoIndent < 0)
            m_x -= advance_width;
        if (isIndent)
            m_indent = m_x + advance_width;
        if (isIndentDecr && m_indent > 0)
        {
            flush();
            m_indent = 0;
        }
    }

    m_mode = current;
    m_isBeginningOfLine = false;
}

void TextRenderBase::flush(bool force)
{
    if (m_buffer.empty())
        return;

    // place every buffered character on the current line, or move the whole
    // group to the next line when it does not fit
    auto x = m_x;
    for (auto& ch : m_buffer)
    {
        const auto advance_width = ch.cw;
        auto new_x = advance_width + x + m_state.pitch;

        if (m_boxWidth < new_x)
        {
            if (force)
            {
                performLinebreak();
                x = m_x;
                new_x = advance_width + x + m_state.pitch;
            }
            else
            {
                performLinebreak();
                flush(true);
                return;
            }
        }
        ch.layoutX = ch.x = x;
        ch.layoutY = ch.y = m_y;
        x = new_x;
    }

    m_x = x;
    m_characters.insert(m_characters.end(), m_buffer.begin(), m_buffer.end());
    m_buffer.clear();
    m_layoutDirty = true;
}

void TextRenderBase::applyAlignment()
{
    if (!m_layoutDirty || m_characters.empty())
        return;
    m_layoutDirty = false;

    struct LineBounds
    {
        size_t first = 0;
        size_t last = 0;
        int y = 0;
        int left = 0;
        int right = 0;
        int top = 0;
        int bottom = 0;
    };

    // Alignment is always computed from the unaligned layout positions, so
    // characters appended after a previous alignment pass line up correctly.
    std::vector<LineBounds> lines;
    for (size_t i = 0; i < m_characters.size(); ++i)
    {
        auto& ch = m_characters[i];
        ch.x = ch.layoutX;
        ch.y = ch.layoutY;
        const int left = ch.x;
        const int right = ch.x + std::max(ch.cw, 0);
        const int top = ch.y;
        const int bottom = ch.y + std::max(ch.size, getAscentHeight());
        if (lines.empty() || lines.back().y != ch.y)
        {
            lines.push_back({i, i, ch.y, left, right, top, bottom});
            continue;
        }
        auto& line = lines.back();
        line.last = i;
        line.left = std::min(line.left, left);
        line.right = std::max(line.right, right);
        line.top = std::min(line.top, top);
        line.bottom = std::max(line.bottom, bottom);
    }

    int blockTop = std::numeric_limits<int>::max();
    int blockBottom = std::numeric_limits<int>::min();
    for (auto& line : lines)
    {
        if (m_boxWidth > 0)
        {
            const int lineWidth = line.right - line.left;
            int offsetX = 0;
            if (m_state.align == 0)
                offsetX = (m_boxWidth - lineWidth) / 2 - line.left;
            else if (m_state.align > 0)
                offsetX = (m_boxWidth - lineWidth) - line.left;
            if (offsetX != 0)
            {
                for (size_t i = line.first; i <= line.last; ++i)
                    m_characters[i].x += offsetX;
            }
        }
        blockTop = std::min(blockTop, line.top);
        blockBottom = std::max(blockBottom, line.bottom);
    }

    if (m_boxHeight > 0 && blockTop <= blockBottom)
    {
        const int blockHeight = blockBottom - blockTop;
        int offsetY = 0;
        if (m_state.valign == 0)
            offsetY = (m_boxHeight - blockHeight) / 2 - blockTop;
        else if (m_state.valign > 0)
            offsetY = (m_boxHeight - blockHeight) - blockTop;
        if (offsetY != 0)
        {
            for (auto& ch : m_characters)
                ch.y += offsetY;
        }
    }
}

void TextRenderBase::setRenderSize(int width, int height)
{
    m_boxWidth = width;
    m_boxHeight = height;
    clear();
}

void TextRenderBase::setDefault(tTJSVariant defaultSettings)
{
    m_default.deserialize(defaultSettings);
}

void TextRenderBase::setOption(tTJSVariant options)
{
    m_options.deserialize(options);
}

tTJSVariant TextRenderBase::getCharacters(int start, int end)
{
    applyAlignment();
    auto array = TJSCreateArrayObject();

    // only (0, 0) has been observed: return everything
    if ((end < start) || (start == 0 && end == 0))
    {
        for (size_t i = 0, cnt = m_characters.size(); i < cnt; ++i)
        {
            auto ch = m_characters[i].serialize();
            array->PropSetByNum(TJS_MEMBERENSURE, (tjs_int)i, &ch, array);
        }
    }

    auto res = tTJSVariant(array, array);
    array->Release();
    return res;
}

void TextRenderBase::clear()
{
    m_characters.clear();
    m_buffer.clear();
    m_state = m_default;
    m_overflow = false;
    m_x = 0;
    m_y = 0;
    m_indent = 0;
    m_isBeginningOfLine = true;
    m_fontDirty = true;
    m_layoutDirty = false;
    m_mode = kTextRenderModeLeading;
    m_keyWaits.clear();
    m_baseCharacterDelay = 0.0;
    m_characterDelay = 0.0;
    m_currentDelay = 0.0;
    m_renderDelay = 0.0;
    m_timingSegmentStart = 0;
    m_timingSegmentStartDelay = 0.0;
    m_sameKeyWaitIndex = 0;
    m_sameTiming = false;
    m_renderCountOverride = -1;
}

void TextRenderBase::done()
{
    flush();
    applyAlignment();
    updateOverflow();
}

void TextRenderBase::newline()
{
    flush();
    performLinebreak();
}

void TextRenderBase::resetStyle()
{
    m_state = m_default;
    m_fontDirty = true;
    m_layoutDirty = true;
}

void TextRenderBase::resetFont()
{
    m_state.bold = m_default.bold;
    m_state.italic = m_default.italic;
    m_state.face = m_default.face;
    m_state.fontSize = m_default.fontSize;
    m_state.fontScale = m_default.fontScale;
    m_fontDirty = true;
    m_layoutDirty = true;
}

tTJSVariant TextRenderBase::getKeyWait()
{
    const double scale = std::max(m_timeScale, 0.0);
    auto array = TJSCreateArrayObject();
    for (size_t i = 0; i < m_keyWaits.size(); ++i)
    {
        auto dict = TJSCreateDictionaryObject();
        {
            tTJSVariant value(m_keyWaits[i].pos);
            dict->PropSet(TJS_MEMBERENSURE, TJS_N("pos"), nullptr, &value, dict);
        }
        {
            tTJSVariant value(m_keyWaits[i].time * scale);
            dict->PropSet(TJS_MEMBERENSURE, TJS_N("time"), nullptr, &value, dict);
        }
        tTJSVariant value(dict, dict);
        dict->Release();
        array->PropSetByNum(TJS_MEMBERENSURE, static_cast<tjs_int>(i), &value, array);
    }
    auto res = tTJSVariant(array, array);
    array->Release();
    return res;
}

double TextRenderBase::get_renderDelay() const
{
    return m_renderDelay * std::max(m_timeScale, 0.0);
}

int TextRenderBase::calcShowCount(int elapsed)
{
    // number of characters whose reveal time has passed
    const double scale = std::max(m_timeScale, 0.0);
    for (size_t i = m_characters.size(); i > 0; --i)
    {
        if (elapsed >= m_characters[i - 1].delay * scale)
            return static_cast<int>(i);
    }
    return 0;
}

int TextRenderBase::get_renderCount() const
{
    if (m_renderCountOverride >= 0)
        return m_renderCountOverride;
    return static_cast<int>(getCharacterCount());
}

void TextRenderBase::set_renderCount(int value)
{
    m_renderCountOverride = std::max(value, 0);
}

void TextRenderBase::redraw()
{
    done();
    m_renderCountOverride = -1;
}

tTJSVariant TextRenderBase::renderText(tTJSVariant text)
{
    clear();
    render(text.Type() == tvtVoid ? ttstr() : ttstr(text), 0, 0, 0, false, false, nullptr);
    done();
    return getCharacters(0, 0);
}

TextRenderBase::RenderBounds TextRenderBase::getRenderedBounds()
{
    applyAlignment();
    RenderBounds bounds{0, 0, 0, 0};
    bool hasCharacter = false;

    auto accumulate = [&](const CharacterInfo& character)
    {
        const int left = character.x;
        const int top = character.y;
        const int right = character.x + std::max(character.cw, 0);
        const int bottom = character.y + std::max(character.size, 0);
        if (!hasCharacter)
        {
            bounds = {left, top, right, bottom};
            hasCharacter = true;
            return;
        }
        bounds.left = std::min(bounds.left, left);
        bounds.top = std::min(bounds.top, top);
        bounds.right = std::max(bounds.right, right);
        bounds.bottom = std::max(bounds.bottom, bottom);
    };

    for (const auto& character : m_characters)
        accumulate(character);
    for (const auto& character : m_buffer)
        accumulate(character);

    if (!hasCharacter)
    {
        bounds.right = std::max(m_boxWidth, 0);
        bounds.bottom = std::max(m_boxHeight, 0);
    }
    return bounds;
}

tTJSVariant TextRenderBase::getRect()
{
    const auto bounds = getRenderedBounds();
    iTJSDispatch2* rect = TVPCreateRectObject(bounds.left, bounds.top, bounds.right, bounds.bottom);
    tTJSVariant result(rect, rect);
    rect->Release();
    return result;
}

NCB_REGISTER_CLASS(TextRenderBase)
{
    Constructor();

    NCB_METHOD_RAW_CALLBACK(render, &TextRenderBase::renderCallback, 0);
    NCB_METHOD(setRenderSize);
    NCB_METHOD(setDefault);
    NCB_METHOD(setOption);
    NCB_METHOD(getCharacters);
    NCB_METHOD(clear);
    NCB_METHOD(done);
    NCB_METHOD(newline);

    NCB_METHOD(getKeyWait);
    NCB_METHOD(calcShowCount);
    NCB_METHOD(redraw);
    NCB_METHOD(renderText);
    NCB_METHOD(resetStyle);
    NCB_METHOD(resetFont);
    NCB_METHOD(setFontScale);
    NCB_METHOD(getRect);

    property_delegate(vertical);
    property_delegate(bold);
    property_delegate(italic);
    property_delegate(face);
    property_delegate(fontSize);
    property_delegate(fontScale);
    property_delegate(chColor);
    property_delegate(rubySize);
    property_delegate(rubyOffset);
    property_delegate(shadow);
    property_delegate(shadowColor);
    property_delegate(edge);
    property_delegate(edgeColor);
    property_delegate(edgeExtent);
    property_delegate(edgeEmphasis);
    property_delegate(lineSpacing);
    property_delegate(pitch);
    property_delegate(lineSize);
    property_delegate(align);
    property_delegate(valign);
    property_delegate(timeScale);

    property_delegate(defaultBold);
    property_delegate(defaultItalic);
    property_delegate(defaultFace);
    property_delegate(defaultFontSize);
    property_delegate(defaultFontScale);
    property_delegate(defaultChColor);
    property_delegate(defaultRubySize);
    property_delegate(defaultRubyOffset);
    property_delegate(defaultShadow);
    property_delegate(defaultShadowColor);
    property_delegate(defaultEdge);
    property_delegate(defaultEdgeColor);
    property_delegate(defaultEdgeExtent);
    property_delegate(defaultEdgeEmphasis);
    property_delegate(defaultLineSpacing);
    property_delegate(defaultPitch);
    property_delegate(defaultLineSize);
    property_delegate(defaultAlign);
    property_delegate(defaultValign);

    NCB_PROPERTY(renderCount, get_renderCount, set_renderCount);
    // renderOver is a read-only property in the original textrender.dll; a
    // method would read as a truthy function object.
    NCB_PROPERTY_RO(renderOver, get_renderOver);
    NCB_PROPERTY_RO(renderDelay, get_renderDelay);
    NCB_PROPERTY_RO(keyWait, getKeyWait);
    NCB_PROPERTY_RO(renderLeft, get_renderLeft);
    NCB_PROPERTY_RO(renderTop, get_renderTop);
    NCB_PROPERTY_RO(renderWidth, get_renderWidth);
    NCB_PROPERTY_RO(renderHeight, get_renderHeight);
    NCB_PROPERTY_RO(renderRight, get_renderRight);
    NCB_PROPERTY_RO(renderBottom, get_renderBottom);
};
