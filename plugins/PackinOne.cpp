#include "tjsCommHead.h"
#include "ncbind/ncbind.hpp"
#include "LayerExBase.hpp"
#include "TVPStorage.h"
#include "TVPApplication.h"

#include "TVPColor.h"

#define NCB_MODULE_NAME TJS_N("PackinOne.dll")

static void InitPlugin_PackinOne()
{
    // PackinOne.dll bundles these plugins; a game that links only PackinOne
    // still expects all of them.
    static const tjs_char* const bundled[] = {
        TJS_N("fstat.dll"), TJS_N("dirlist.dll"), TJS_N("addFont.dll"), TJS_N("saveStruct.dll"),
    };
    for (const tjs_char* module : bundled)
        ncbAutoRegister::LoadModule(module);

    try
    {
        // 我们并不知道这个插件是干啥的，只能根据情况猜一个
        ncbAutoRegister::LoadModule(TJS_N("LayerExMovie.dll"));
        // 以全局对象为上下文执行，使脚本内 typeof extSourceMap 等全局引用可用
        TVPExecuteScript(TJS_N("\
class AffineSourceMovie extends AffineSource {\
	var _movie;\
	var _width = 0;\
	var _height = 0;\
	var _lastOwner = true;\
	function AffineSourceMovie(window) {\
        super.AffineSource(window);\
	}\
	function createLayer(orig=void) {\
        var src = new global.Layer(_window, _pool);\
        if (orig != void)\
        {\
            src.assignImages(orig);\
            src.width = orig.width;\
            src.height = orig.height;\
            src.scale = orig.scale;\
        }\
        else\
        {\
            src.scale = 1.0;\
        }\
        return src;\
	}\
	function finalize()\
	{\
        if (_lastOwner)\
        {\
            clear();\
            invalidate _movie;\
        }\
	}\
	function clear()\
	{\
        notifyOwner(\"onMotionStop\");\
        onMovieStop();\
        kag.conductor.trigger(\"movie_world_foremovie\");\
	}\
	function clone(newwindow, instance) {\
        if (newwindow === void)\
        {\
            newwindow = _window;\
        }\
        if (instance === void)\
        {\
            instance = new global.AffineSourceMovie(newwindow);\
        }\
        instance._movie = _movie;\
        instance._width = _width;\
        instance._height = _height;\
        _lastOwner = false;\
        super.clone(newwindow, instance);\
        return instance;\
	}\
	function canWaitMovie() {\
        return _movie.isPlayingMovie();\
	}\
	function isFlip() {\
        if (_movie.isPlayingMovie())\
        {\
            return true;\
        }\
        else\
        {\
            clear();\
            return false;\
        }\
	}\
	function stopMovie() {\
        _movie.stopMovie();\
	}\
	function drawAffine(target, mtx, src) {\
        (global.Layer.copyRect incontextof target)(0, 0, _movie, 0, 0, _width, _height);\
	}\
	function loadImages(storage,colorKey=clNone,options=void) {\
        _movie = createLayer();\
        _movie.openMovie(storage, false);\
        _movie.setSizeToImageSize();\
        _width = _movie.width;\
        _height = _movie.height;\
        _movie.startMovie(false);\
	}\
};\
if (typeof global.extSourceMap != \"undefined\") {\
    global.extSourceMap[\".WMV\"] = AffineSourceMovie;\
    global.extSourceMap[\".MPG\"] = AffineSourceMovie;\
    global.extSourceMap[\".MPEG\"] = AffineSourceMovie;\
}\
        "), TVPGetScriptEngine()->GetGlobalNoAddRef());
    }
    catch (const eTJS& e)
    {
        TVPAddImportantLog(TJS_N("(warning) PackinOne: AffineSourceMovie setup failed: ") +
                           e.GetMessage());
    }
    catch (...)
    {
        TVPAddImportantLog(TJS_N("(warning) PackinOne: AffineSourceMovie setup failed"));
    }
}

NCB_PRE_REGIST_CALLBACK(InitPlugin_PackinOne);

// CompoundStorageMedia is exported by PackinOne. There is no compound storage
// support here, so every call succeeds without doing anything; that is enough
// for startup scripts that only construct and configure it.
class CompoundStorageMedia
{
public:
    CompoundStorageMedia()
    {
        TVPAddImportantLog(TJS_N("(warning) CompoundStorageMedia is a stub; its storages are not mounted"));
    }

    ttstr getArchiveUniqueKey() const { return TJS_N("CompoundStorageMedia"); }

    static tjs_error ok(tTJSVariant* result, tjs_int, tTJSVariant**, CompoundStorageMedia*)
    {
        if (result)
            *result = (tjs_int)1;
        return TJS_S_OK;
    }

    static tjs_error emptyString(tTJSVariant* result, tjs_int, tTJSVariant**, CompoundStorageMedia*)
    {
        if (result)
            *result = ttstr();
        return TJS_S_OK;
    }
};

NCB_REGISTER_CLASS(CompoundStorageMedia)
{
    Constructor();
    NCB_METHOD_RAW_CALLBACK(addArchive, &CompoundStorageMedia::ok, 0);
    NCB_METHOD_RAW_CALLBACK(addStorage, &CompoundStorageMedia::ok, 0);
    NCB_METHOD_RAW_CALLBACK(addAutoToolsPath, &CompoundStorageMedia::ok, 0);
    NCB_METHOD_RAW_CALLBACK(setCurrentDirectory, &CompoundStorageMedia::ok, 0);
    NCB_METHOD_RAW_CALLBACK(register, &CompoundStorageMedia::ok, 0);
    NCB_METHOD_RAW_CALLBACK(unregister, &CompoundStorageMedia::ok, 0);
    NCB_METHOD_RAW_CALLBACK(parseArchiveIndex, &CompoundStorageMedia::ok, 0);
    NCB_METHOD_RAW_CALLBACK(getLocallyAccessibleName, &CompoundStorageMedia::emptyString, 0);
    NCB_PROPERTY_RO(archiveUniqueKey, getArchiveUniqueKey);
}
