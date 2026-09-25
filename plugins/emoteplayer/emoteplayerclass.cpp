#include "ncbind/ncbind.hpp"
#include "emoteplayerclass.h"
#include "emoteresourcecache.h"
#include "tjsArray.h"
#include "TVPStorage.h"
#include "Platform.h"
#include "Random.h"
#include "md5.h"
#include <SDL3/SDL.h>

#include "tjsCommHead.h"
#include "tjsNativeLayer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

namespace emoteplayer
{

iTJSDispatch2* ResourceManager::_kagWindow = nullptr;
static SeparateLayerAdaptor* _motionWorkLayer = nullptr;

static std::uint64_t nextEmoteManagerId()
{
    static std::atomic<std::uint64_t> next{0};
    return next.fetch_add(1, std::memory_order_relaxed) + 1;
}

static std::uint64_t emoteResourceId(const ttstr& placedPath)
{
    // Salted fingerprints correlate the same canonical resource across managers
    // during one process run, without recording a path or retaining a path table.
    // They are diagnostic identifiers, not an integrity/security primitive.
    static const std::array<md5_byte_t, 16> salt = [] {
        std::array<md5_byte_t, 16> value{};
        TVPGetRandomBits128(value.data());
        return value;
    }();
    md5_state_t state;
    md5_init(&state);
    md5_append(&state, salt.data(), static_cast<int>(salt.size()));
    md5_append(&state, reinterpret_cast<const md5_byte_t*>(placedPath.c_str()),
               placedPath.length() * sizeof(tjs_char));
    md5_byte_t digest[16];
    md5_finish(&state, digest);
    std::uint64_t id = 0;
    for (int i = 0; i < 8; ++i) id = (id << 8) | digest[i];
    return id;
}

static void recordEmoteCacheEvent(const char* action, const EmoteResourceDiagnostics& diagnostic,
                                 std::size_t entries, std::uint64_t resourceId = 0)
{
    static thread_local Uint64 lastReportAt = 0;
    static thread_local Uint64 suppressed = 0;
    const Uint64 now = SDL_GetTicksNS();
    if (lastReportAt && now - lastReportAt < 1000000000ULL) {
        ++suppressed;
        return;
    }
    try {
        const auto shared = GetSharedEmoteResourceCacheStats();
        TVPConsoleLog("emote.resourceCache action=%s managerId=%llu resourceId=%016llx "
                      "entries=%llu loads=%llu cacheHits=%llu cacheMisses=%llu failures=%llu "
                      "unloads=%llu unloadAlls=%llu clearCacheCalls=%llu suppressed=%llu "
                      "sharedHits=%llu sharedMisses=%llu sharedBytes=%llu "
                      "sharedEntries=%llu sharedEvictions=%llu",
                      action, static_cast<unsigned long long>(diagnostic.managerId),
                      static_cast<unsigned long long>(resourceId),
                      static_cast<unsigned long long>(entries),
                      static_cast<unsigned long long>(diagnostic.loads),
                      static_cast<unsigned long long>(diagnostic.hits),
                      static_cast<unsigned long long>(diagnostic.misses),
                      static_cast<unsigned long long>(diagnostic.failures),
                      static_cast<unsigned long long>(diagnostic.unloads),
                      static_cast<unsigned long long>(diagnostic.unloadAlls),
                      static_cast<unsigned long long>(diagnostic.clearCacheCalls),
                      static_cast<unsigned long long>(suppressed),
                      static_cast<unsigned long long>(shared.hits),
                      static_cast<unsigned long long>(shared.misses),
                      static_cast<unsigned long long>(shared.retainedBytes),
                      static_cast<unsigned long long>(shared.entries),
                      static_cast<unsigned long long>(shared.evictions));
    } catch (...) {
        // Logging must never change resource lifetime or load behavior.
    }
    lastReportAt = now;
    suppressed = 0;
}

// Keep long scene/resource stalls identifiable without logging every animation
// update. Durations include any blocking work; no asset paths or script values
// are written to the log. Resource loads and play calls each log at most once/s.
static void recordSlowEmoteOperation(bool resourceLoad, Uint64 started,
                                     Uint64 fileLoadNS = 0, Uint64 rootNS = 0,
                                     bool cacheHit = false,
                                     const EmoteResourceDiagnostics* diagnostic = nullptr,
                                     std::uint64_t resourceId = 0, std::size_t entries = 0,
                                     bool rootRequested = true, bool sharedCacheHit = false,
                                     bool customDecrypt = false, bool archiveFilter = false)
{
    const Uint64 finished = SDL_GetTicksNS();
    const Uint64 wallNS = finished - started;
    if (wallNS < 50000000ULL) return;
    struct ReportState {
        Uint64 lastReportAt = 0;
        Uint64 suppressed = 0;
        Uint64 suppressedPeakNS = 0;
    };
    static thread_local ReportState states[2];
    auto &state = states[resourceLoad ? 0 : 1];
    if (state.lastReportAt && finished - state.lastReportAt < 1000000000ULL) {
        ++state.suppressed;
        state.suppressedPeakNS = std::max(state.suppressedPeakNS, wallNS);
        return;
    }
    try {
        const auto shared = GetSharedEmoteResourceCacheStats();
        TVPConsoleLog("emote.slowOperation operation=%s wallMS=%.3f fileLoadMS=%.3f "
                      "rootMS=%.3f cacheHit=%d suppressed=%llu suppressedPeakWallMS=%.3f "
                      "rootRequested=%d managerId=%llu resourceId=%016llx entries=%llu "
                      "loads=%llu cacheHits=%llu cacheMisses=%llu failures=%llu "
                      "unloads=%llu unloadAlls=%llu clearCacheCalls=%llu "
                      "sharedCacheHit=%d customDecrypt=%d archiveFilter=%d sharedHits=%llu sharedMisses=%llu "
                      "sharedBytes=%llu sharedEntries=%llu sharedEvictions=%llu",
                      resourceLoad ? "resourceLoad" : "play",
                      static_cast<double>(wallNS) / 1000000.0,
                      static_cast<double>(fileLoadNS) / 1000000.0,
                      static_cast<double>(rootNS) / 1000000.0, cacheHit ? 1 : 0,
                      static_cast<unsigned long long>(state.suppressed),
                      static_cast<double>(state.suppressedPeakNS) / 1000000.0,
                      resourceLoad && rootRequested ? 1 : 0,
                      static_cast<unsigned long long>(diagnostic ? diagnostic->managerId : 0),
                      static_cast<unsigned long long>(resourceId),
                      static_cast<unsigned long long>(entries),
                      static_cast<unsigned long long>(diagnostic ? diagnostic->loads : 0),
                      static_cast<unsigned long long>(diagnostic ? diagnostic->hits : 0),
                      static_cast<unsigned long long>(diagnostic ? diagnostic->misses : 0),
                      static_cast<unsigned long long>(diagnostic ? diagnostic->failures : 0),
                      static_cast<unsigned long long>(diagnostic ? diagnostic->unloads : 0),
                      static_cast<unsigned long long>(diagnostic ? diagnostic->unloadAlls : 0),
                      static_cast<unsigned long long>(diagnostic ? diagnostic->clearCacheCalls : 0),
                      resourceLoad && !cacheHit && sharedCacheHit ? 1 : 0,
                      resourceLoad && !cacheHit && customDecrypt ? 1 : 0,
                      resourceLoad && !cacheHit && archiveFilter ? 1 : 0,
                      static_cast<unsigned long long>(shared.hits),
                      static_cast<unsigned long long>(shared.misses),
                      static_cast<unsigned long long>(shared.retainedBytes),
                      static_cast<unsigned long long>(shared.entries),
                      static_cast<unsigned long long>(shared.evictions));
    } catch (...) {
        // Diagnostics must not make an otherwise successful operation fail.
    }
    state.lastReportAt = finished;
    state.suppressed = state.suppressedPeakNS = 0;
}

static const emoterect* findShapeAreaRecursive(const emotemotionref* motion, const char* name)
{
    if (!motion) return nullptr;
    for (const auto& area : motion->shapeNodeAreas)
        if (std::strcmp(area.label.c_str(), name) == 0)
            return &area;
    for (const auto* sub : motion->_subMotionRefs)
        if (const auto* area = findShapeAreaRecursive(sub, name))
            return area;
    return nullptr;
}

static emotemotionref* findMotionRefRecursive(emotemotionref* motion, const char* name)
{
    if (!motion) return nullptr;
    for (auto* sub : motion->_subMotionRefs)
        if (std::strcmp(sub->label.c_str(), name) == 0)
            return sub;
    for (auto* sub : motion->_subMotionRefs)
        if (auto* found = findMotionRefRecursive(sub, name))
            return found;
    return nullptr;
}

ResourceManager::ResourceManager(iTJSDispatch2* kagWindow, tjs_int cacheSize)
{
    _diagnostics.managerId = nextEmoteManagerId();
    recordEmoteCacheEvent("create", _diagnostics, 0);
    // window info
    tjs_int sWidth = 1280, sHeight = 720;
    if (kagWindow != nullptr)
    {
        tTJSVariant val;
        kagWindow->PropGet(0, TJS_N("width"), NULL, &val, kagWindow);
        sWidth = (tjs_int)val;
        kagWindow->PropGet(0, TJS_N("height"), NULL, &val, kagWindow);
        sHeight = (tjs_int)val;
        _kagWindow = kagWindow;
    }
    // 放这里来吧，省得init时啥也没有
    if (_motionWorkLayer == nullptr && kagWindow != nullptr)
    {
        // kag.poolLayer作为父类
        tTJSVariant baseLayer;
        iTJSDispatch2* kag = kagWindow;
        if (TJS_FAILED(kag->PropGet(0, TJS_N("poolLayer"), NULL, &baseLayer, kag)) ||
            baseLayer.Type() != tvtObject)
        {
            TVPConsoleLog("create motionWorkLayer failed");
            return;
        }
        // 创建motionWorkLayer实例
        _motionWorkLayer = new SeparateLayerAdaptor(baseLayer.AsObjectThisNoAddRef());
        // 置入全局变量
        tTJSVariant val = tTJSVariant(_motionWorkLayer);
        _motionWorkLayer->Release();
        iTJSDispatch2* global = TVPGetScriptDispatch();
        if (global)
        {
            global->PropSet(TJS_MEMBERENSURE, TJS_N("motionWorkLayer"), NULL, &val, global);
            global->Release();
        }
    }
}
ResourceManager::~ResourceManager()
{
    unloadAllInternal("destroy");
}
tTJSVariant ResourceManager::load(tTJSString path)
{
    return loadInternal(path, true);
}
void ResourceManager::ensureLoaded(tTJSString path)
{
    loadInternal(path, false);
}
tTJSVariant ResourceManager::loadInternal(tTJSString path, bool materializeRoot)
{
    const Uint64 started = SDL_GetTicksNS();
    ttstr trimPath;
    if (path.StartsWith(TJS_N("lzfs://./")))
        trimPath = path.SubString(9, path.length() - 9);
    else
        trimPath = path;
    const ttstr placedPath = TVPGetPlacedPath(trimPath);
    const std::uint64_t resourceId = emoteResourceId(placedPath);
    ++_diagnostics.loads;
    auto rst = cacheData.find(placedPath);
    const bool cacheHit = rst != cacheData.end();
    const bool customDecrypt = !cacheHit && _decryptClo.Object != nullptr;
    if (cacheHit) ++_diagnostics.hits;
    else ++_diagnostics.misses;
    Uint64 fileLoadNS = 0;
    Uint64 rootNS = 0;
    try {
        emotefile* file = cacheHit ? rst->second : nullptr;
        if (!file) {
            auto pending = std::make_unique<emotefile>();
            pending->setSeed(_decryptkey);
            pending->setFun(_decryptClo);
            const Uint64 fileLoadStarted = SDL_GetTicksNS();
            const bool loaded = pending->load(trimPath);
            fileLoadNS = SDL_GetTicksNS() - fileLoadStarted;
            if (!loaded)
                TVPThrowExceptionMessage(TJS_N("Unable to load Emote resource"));
            auto inserted = cacheData.emplace(placedPath, pending.get());
            file = inserted.first->second;
            if (inserted.second) pending.release();
        }
        tTJSVariant root;
        if (materializeRoot) {
            // Script callers retain the contract of a fresh, mutable root tree.
            const Uint64 rootStarted = SDL_GetTicksNS();
            root = file->root();
            rootNS = SDL_GetTicksNS() - rootStarted;
        }
        recordSlowEmoteOperation(true, started, fileLoadNS, rootNS, cacheHit,
                                 &_diagnostics, resourceId, cacheData.size(), materializeRoot,
                                 !cacheHit && file->WasSharedCacheHit(), customDecrypt,
                                 !cacheHit && file->BypassedSharedCacheForArchiveFilter());
        return root;
    } catch (...) {
        ++_diagnostics.failures;
        recordEmoteCacheEvent("loadFailed", _diagnostics, cacheData.size(), resourceId);
        throw;
    }
}
void ResourceManager::unload(tTJSString path)
{
    ttstr trimPath;
    if (path.StartsWith(TJS_N("lzfs://./")))
        trimPath = path.SubString(9, path.length() - 9);
    else
        trimPath = path;
    const ttstr placedPath = TVPGetPlacedPath(trimPath);
    auto it = cacheData.find(placedPath);
    if (it != cacheData.end())
    {
        if (it->second != nullptr)
            delete it->second;
        cacheData.erase(it);
        ++_diagnostics.unloads;
    }
    recordEmoteCacheEvent("unload", _diagnostics, cacheData.size(), emoteResourceId(placedPath));
}
void ResourceManager::unloadAll()
{
    unloadAllInternal("unloadAll");
}
void ResourceManager::unloadAllInternal(const char* diagnosticAction)
{
    ++_diagnostics.unloadAlls;
    _diagnostics.unloads += cacheData.size();
    for (auto item : cacheData)
    {
        if (item.second != nullptr)
            delete item.second;
    }
    cacheData.clear();
    recordEmoteCacheEvent(diagnosticAction, _diagnostics, 0);
}
void ResourceManager::clearCache()
{
    // Drop reusable decoded data while preserving files used by active players.
    ClearSharedEmoteResourceCache();
    ++_diagnostics.clearCacheCalls;
    recordEmoteCacheEvent("clearCache", _diagnostics, cacheData.size());
}
emotefile* ResourceManager::GetPlayerByName(const tTJSString& name)
{
    auto it = cacheData.find(name);
    if (it != cacheData.end())
    {
        return it->second;
    }
    return nullptr;
}
void ResourceManager::setEmotePSBDecryptSeed(tjs_int decryptkey)
{
    if (_decryptkey == decryptkey) return;
    _decryptkey = decryptkey;
    ClearSharedEmoteResourceCache();
}
void ResourceManager::setEmotePSBDecryptFunc(tTJSVariant funclosure)
{
    _decryptClo = funclosure.AsObjectClosure();
    // Reinstalling the same closure can accompany a change in its script state.
    ClearSharedEmoteResourceCache();
}

SeparateLayerAdaptor::SeparateLayerAdaptor(iTJSDispatch2* targetLayer)
{
    // 创建实例
    _this = new tTJSNI_Layer();
    tTJSVariant kag(ResourceManager::_kagWindow);
    tTJSVariant layer(targetLayer);
    tTJSVariant* params[] = {&kag, &layer};
    if (TJS_FAILED(_this->Construct(2, params, this)))
        TVPThrowExceptionMessage(TVPSpecifyLayer);
    // 获取父类实例
    tTJSNI_Layer* ths = NULL;
    if (targetLayer->NativeInstanceSupport(TJS_NIS_GETINSTANCE, tTJSNC_Layer::ClassID,
                                           (iTJSNativeInstance**)&ths) < 0 ||
        ths == NULL)
        TVPThrowExceptionMessage(TVPSpecifyLayer);
    // 设置参数
    _this->SetSize(ths->GetWidth(), ths->GetHeight());
    _this->SetImageSize(ths->GetWidth(), ths->GetHeight());
    _this->SetVisible(true);
    _this->SetType(ltAlpha);
    _this->SetHitType(htProvince);
}
SeparateLayerAdaptor::~SeparateLayerAdaptor()
{
    clear();
}
void SeparateLayerAdaptor::assign(iTJSDispatch2* anotherAdaptor)
{
    // TODO
}
void SeparateLayerAdaptor::clear()
{
    // 渲染目标由 core/render 的 2D 渲染器统一管理
    krkrsdl3::iTVPRenderBackend* renderer = krkrsdl3::TVPGetRenderBackend();
    if (renderer)
    {
        if (target)
        {
            renderer->DestroyTarget(target);
            target = nullptr;
        }
        if (maskTarget)
        {
            renderer->DestroyTarget(maskTarget);
            maskTarget = nullptr;
        }
    }
    if (_this != nullptr)
    {
        _this->Invalidate();
        delete _this;
        _this = nullptr;
    }
}
tjs_int SeparateLayerAdaptor::get_absolute()
{
    if (_this != nullptr)
    {
        return _this->GetAbsoluteOrderIndex();
    }
    return 0;
}
void SeparateLayerAdaptor::set_absolute(tjs_int v)
{
    if (_this != nullptr)
    {
        _this->SetAbsoluteOrderIndex(v);
    }
}
bool SeparateLayerAdaptor::get_isPrimary()
{
    if (_this != nullptr)
    {
        return _this->IsPrimary();
    }
    return false;
}
void SeparateLayerAdaptor::set_isPrimary(bool v)
{
    //
}
tTJSVariant SeparateLayerAdaptor::get_parent()
{
    if (_this != nullptr)
    {
        return _this->GetParent();
    }
    return tTJSVariant();
}
void SeparateLayerAdaptor::set_parent(tTJSVariant v)
{
    //
}
void SeparateLayerAdaptor::checkDrawArea(tjs_int width, tjs_int height)
{
    // 通过 2D 渲染器创建/重建渲染目标（GL 后端= FBO，软渲染后端= CPU 缓冲）
    if (target == nullptr || _width != width || _height != height)
    {
        krkrsdl3::iTVPRenderBackend* renderer = krkrsdl3::TVPGetRenderBackend();
        if (!renderer)
            return;
        if (target)
        {
            renderer->DestroyTarget(target);
            target = nullptr;
        }
        if (maskTarget)
        {
            renderer->DestroyTarget(maskTarget);
            maskTarget = nullptr;
        }
        _width = width;
        _height = height;
        target = renderer->CreateTarget(width, height);
        maskTarget = renderer->CreateTarget(width, height); // 蒙版目标（与主体同尺寸）
    }
}

D3DAdaptor::D3DAdaptor(
    iTJSDispatch2* winRef, tjs_int width, tjs_int height, tjs_int orgX, tjs_int orgY)
{
    krkrsdl3::iTVPRenderBackend* renderer = krkrsdl3::TVPGetRenderBackend();
    if (renderer)
    {
        _target = renderer->CreateTarget(width, height);
        _maskTarget = renderer->CreateTarget(width, height);
        _width = width;
        _height = height;
        _orgX = orgX;
        _orgY = orgY;
    }
}
D3DAdaptor::~D3DAdaptor()
{
    krkrsdl3::iTVPRenderBackend* renderer = krkrsdl3::TVPGetRenderBackend();
    if (renderer)
    {
        if (_target)
        {
            renderer->DestroyTarget(_target);
            _target = nullptr;
        }
        if (_maskTarget)
        {
            renderer->DestroyTarget(_maskTarget);
            _maskTarget = nullptr;
        }
    }
}
void D3DAdaptor::setClearColor(tjs_uint32 color)
{
    _clearColor = color;
}
void D3DAdaptor::captureCanvas(iTJSDispatch2* targetLayer)
{
    tTJSNI_BaseLayer* ths = NULL;
    if (targetLayer->NativeInstanceSupport(TJS_NIS_GETINSTANCE, tTJSNC_Layer::ClassID,
                                       (iTJSNativeInstance**)&ths) < 0)
        return;
    krkrsdl3::iTVPRenderBackend* renderer = krkrsdl3::TVPGetRenderBackend();
    if (!renderer)
        return;

    struct CaptureProfileScope {
        Uint64 started = SDL_GetTicksNS();
        ~CaptureProfileScope() {
            krkrsdl3::TVPRecordEmoteCaptureTime(SDL_GetTicksNS() - started);
        }
    } captureProfile;
    krkrsdl3::TVPRecordEmoteCaptureCall();
    const tjs_int layerWidth = ths->GetWidth();
    const tjs_int layerHeight = ths->GetHeight();
    const tjs_int copyWidth = std::min(_width, layerWidth);
    const tjs_int copyHeight = std::min(_height, layerHeight);
    const bool fullOverwrite =
        copyWidth == layerWidth && copyHeight == layerHeight &&
        _width == layerWidth && _height == layerHeight;
    const uint64_t fullBytes =
        fullOverwrite && _width > 0 && _height > 0 ? uint64_t(_width) * uint64_t(_height) * 4 : 0;

    if (fullOverwrite)
    {
        void* destination = ths->GetMainImageGPUHandleForOverwrite();
        if (destination && renderer->CopyTargetToLayerTexture(_target, destination))
        {
            ths->CommitMainImageGPUOverwrite();
            krkrsdl3::TVPRecordEmoteCaptureGPUCopy(fullBytes);
            ths->Update();
            return;
        }
    }

    int pitch = 0;
    uint8_t* pixels = renderer->LockTarget(_target, pitch);
    if (!pixels || pitch <= 0)
    {
        renderer->UnlockTarget(_target);
        return;
    }
    krkrsdl3::TVPRecordEmoteCaptureCPUFallback(
        _height > 0 ? uint64_t(pitch) * uint64_t(_height) : 0);

    tjs_uint8* buff = (tjs_uint8*)(fullOverwrite
        ? ths->GetMainImagePixelBufferForOverwrite()
        : ths->GetMainImagePixelBufferForWrite());

    if (buff && copyWidth > 0 && copyHeight > 0)
    {
        const tjs_int dstPitch = ths->GetMainImagePixelBufferPitch();
        const size_t rowBytes = (size_t)copyWidth * 4;
        if (dstPitch >= (tjs_int)rowBytes && pitch >= (tjs_int)rowBytes)
        {
            if (pitch == dstPitch && copyWidth == _width && copyWidth == layerWidth)
                std::memcpy(buff, pixels, rowBytes * copyHeight);
            else
                for (tjs_int y = 0; y < copyHeight; ++y)
                    std::memcpy(buff + (size_t)y * dstPitch,
                                pixels + (size_t)y * pitch,
                                rowBytes);
        }
    }

    renderer->UnlockTarget(_target);
    ths->ReleaseMainImagePixelBufferForWrite(tTVPRect(0, 0, copyWidth, copyHeight));
    ths->Update();
}
void D3DAdaptor::unloadUnusedTextures()
{
    // 做一下清屏操作
    // D3D自己清理
    krkrsdl3::iTVPRenderBackend* renderer = krkrsdl3::TVPGetRenderBackend();
    if (renderer)
    {
        renderer->SetTarget(_target);
        renderer->ClearTarget(true);
    }
}

// Getters retain the player and resolve their path against the last draw. Holding a raw
// sub-motion pointer here is unsafe because the tree is rebuilt for each rendered frame.
static tTJSVariant makeLayerGetter(EmotePlayer* player, emotemotionref* motion, const char* name);
class TmpMotionObj : public tTJSNativeClass
{
public:
    TmpMotionObj(EmotePlayer* player, emotemotionref* motion, const char* shape = nullptr)
        : tTJSNativeClass(TJS_N("MotionObj")), _player(player), _isShape(shape != nullptr),
          _shape(shape ? shape : "")
    {
        _player->AddRef();
        for (auto* ref = motion; ref && ref->parent; ref = ref->parent->refMtn)
            _path.insert(_path.begin(), ref->label.c_str());
    }
    ~TmpMotionObj() { _player->Release(); }

    tjs_error FuncCall(tjs_uint32 flag, const tjs_char* membername, tjs_uint32* hint,
                       tTJSVariant* result, tjs_int numparams, tTJSVariant** param,
                       iTJSDispatch2* objthis) override
    {
        if (!membername) return TJS_E_MEMBERNOTFOUND;
        auto frame = _player->getHitFrame();
        auto* motion = frame.motion.get();
        for (const auto& name : _path)
        {
            emotemotionref* next = nullptr;
            if (motion)
                for (auto* sub : motion->_subMotionRefs)
                    if (std::strcmp(sub->label.c_str(), name.c_str()) == 0)
                    {
                        next = sub;
                        break;
                    }
            motion = next;
        }
        if (std::strcmp(membername, "contains") == 0 || std::strcmp(membername, "hitTest") == 0)
        {
            frame.motion = std::shared_ptr<emotemotionref>(frame.motion, motion);
            return dispatchEmoteHitTest(result, numparams, param,
                [&](const char* label, tjs_real x, tjs_real y)
                {
                    const bool local = label && std::strcmp(membername, "contains") == 0;
                    return frame.contains(_isShape ? _shape.c_str() : label, x, y, local);
                });
        }
        if (std::strcmp(membername, "getLayerGetter") == 0)
        {
            if (numparams < 1) return TJS_E_BADPARAMCOUNT;
            if (result) *result = makeLayerGetter(_player, motion, tTJSString(*param[0]).c_str());
            return TJS_S_OK;
        }
        if (std::strcmp(membername, "getLayerMotion") == 0)
        {
            if (numparams < 1) return TJS_E_BADPARAMCOUNT;
            auto* sub = findMotionRefRecursive(motion, tTJSString(*param[0]).c_str());
            if (result)
            {
                result->Clear();
                if (sub)
                {
                    auto* getter = new TmpMotionObj(_player, sub);
                    *result = tTJSVariant(getter);
                    getter->Release();
                }
            }
            return TJS_S_OK;
        }
        if (std::strcmp(membername, "setVariable") == 0)
        {
            if (numparams < 2) return TJS_E_BADPARAMCOUNT;
            _player->setVariable(tTJSString(*param[0]), (tjs_real)*param[1]);
            return TJS_S_OK;
        }
        return TJS_E_MEMBERNOTFOUND;
    }

protected:
    tTJSNativeInstance* CreateNativeInstance() override { return nullptr; }

private:
    EmotePlayer* _player;
    std::vector<std::string> _path;
    bool _isShape;
    std::string _shape;
};

static tTJSVariant makeLayerGetter(EmotePlayer* player, emotemotionref* motion, const char* name)
{
    auto* dict = TJSCreateDictionaryObject();
    if (auto* sub = findMotionRefRecursive(motion, name))
    {
        auto* getter = new TmpMotionObj(player, sub);
        tTJSVariant value(getter);
        dict->PropSet(TJS_MEMBERENSURE, TJS_N("motion"), nullptr, &value, dict);
        dict->PropSet(TJS_MEMBERENSURE, TJS_N("shape"), nullptr, &value, dict);
        getter->Release();
    }
    else
    {
        if (const auto* area = findShapeAreaRecursive(motion, name))
        {
            auto* getter = new TmpMotionObj(player, motion, name);
            tTJSVariant l(area->left), t(area->top), w(area->width), h(area->height), st(area->shapeType);
            getter->PropSet(TJS_MEMBERENSURE, TJS_N("l"), nullptr, &l, getter);
            getter->PropSet(TJS_MEMBERENSURE, TJS_N("t"), nullptr, &t, getter);
            getter->PropSet(TJS_MEMBERENSURE, TJS_N("w"), nullptr, &w, getter);
            getter->PropSet(TJS_MEMBERENSURE, TJS_N("h"), nullptr, &h, getter);
            getter->PropSet(TJS_MEMBERENSURE, TJS_N("shapeType"), nullptr, &st, getter);
            tTJSVariant value(getter);
            dict->PropSet(TJS_MEMBERENSURE, TJS_N("shape"), nullptr, &value, dict);
            getter->Release();
        }
    }
    tTJSVariant result(dict);
    dict->Release();
    return result;
}

#define setprop_t(d, p, ty) \
    { \
        tTJSVariant v(ty(p)); \
        d->PropSet(TJS_MEMBERENSURE, TJS_N(#p), nullptr, &v, d); \
    }
#define setprop(d, p) setprop_t(d, p, )
#define getprop_t(d, p, ty) \
    { \
        tTJSVariant v; \
        if (TJS_SUCCEEDED(d->PropGet(0, TJS_N(#p), nullptr, &v, d)) && v.Type() != tvtVoid) \
        { \
            p = ty(v); \
        } \
    }
#define getprop(d, p) getprop_t(d, p, )
EmotePlayer::~EmotePlayer()
{
    // 渲染目标由 core/render 的 2D 渲染器统一管理
    krkrsdl3::iTVPRenderBackend* renderer = krkrsdl3::TVPGetRenderBackend();
    if (renderer)
    {
        if (_target)
        {
            renderer->DestroyTarget(_target);
            _target = nullptr;
        }
        if (_maskTarget)
        {
            renderer->DestroyTarget(_maskTarget);
            _maskTarget = nullptr;
        }
    }
}
int32_t EmotePlayer::get_loopTime()
{
    if (emtEngine._mainfile != nullptr && emtEngine._mainfile->_objects.size() > 0 &&
        emtEngine._mainfile->_objects.begin()->second->motion.size() > 0)
    {
        return emtEngine._mainfile->_objects.begin()->second->motion.begin()->second->loopTime;
    }
    return 0;
}
tTJSVariant EmotePlayer::get_variableKeys()
{
    iTJSDispatch2* array = TJSCreateArrayObject();
    if (emtEngine._mainfile != nullptr)
    {
        std::set<std::string> varList;
        for (auto varItm : emtEngine._mainfile->_metadata->_varList)
        {
            tTJSVariant tmp(varItm.first);
            tTJSVariant* args[] = {&tmp};
            static tjs_uint addHint = 0;
            array->FuncCall(0, TJS_N("add"), &addHint, nullptr, 1, args, array);
        }
    }
    tTJSVariant result(array, array);
    array->Release();
    return result;
}
tTJSVariant EmotePlayer::serialize()
{
    auto dict = TJSCreateDictionaryObject();

    setprop(dict, currCoordx);
    setprop(dict, currCoordy);
    setprop(dict, currCoordz);
    setprop(dict, currAngle);
    setprop(dict, currZx);
    setprop(dict, currZy);

    auto res = tTJSVariant(dict, dict);
    dict->Release();
    return res;
}
void EmotePlayer::unserialize(tTJSVariant data)
{
    auto dict = data.AsObjectNoAddRef();
    if (!dict)
    {
        return;
    }

    getprop_t(dict, currCoordx, static_cast<tjs_real>);
    getprop_t(dict, currCoordy, static_cast<tjs_real>);
    getprop_t(dict, currCoordz, static_cast<tjs_real>);
    getprop_t(dict, currAngle, static_cast<tjs_real>);
    getprop_t(dict, currZx, static_cast<tjs_real>);
    getprop_t(dict, currZy, static_cast<tjs_real>);
}
void EmotePlayer::play(tTJSString name, int flag)
{
    const Uint64 started = SDL_GetTicksNS();
    if (emtEngine._mainfile != nullptr && !isMotion) // motionKey的启动模式
    {
        // motion
        auto it = emtEngine._mainfile->_objects.find(emtEngine._mainfile->_metadata->chara.c_str());
        if (it != emtEngine._mainfile->_objects.end())
        {
            auto it1 = it->second->motion.find(emtEngine._mainfile->_metadata->motion.c_str());
            if (it1 != it->second->motion.end())
            {
                emtEngine._mainmotion = it1->second;
            }
        }
        // start
        clockPassed = 0.0;
        _motion = name;
        _playing = true;
        _allplaying = true;
        isSelfClear = true;
    }
    else if (_resourceManager->cacheData.size() > 0 && isMotion) // chara+motion启动方案
    {
        _motion = name;
        emtEngine._mainmotion = nullptr;
        // motion
        for (auto tmpFile : _resourceManager->cacheData)
        {
            auto it = tmpFile.second->_objects.find(_chara.AsStdString().c_str());
            if (it != tmpFile.second->_objects.end())
            {
                auto it1 = it->second->motion.find(_motion.AsStdString().c_str());
                if (it1 != it->second->motion.end())
                {
                    emtEngine._mainmotion = it1->second;
                }
            }
            if (emtEngine._mainmotion)
            {
                emtEngine._mainfile = tmpFile.second;
                break;
            }
                
        }
        // start
        clockPassed = 0.0;
        _playing = true;
        _allplaying = true;
        isSelfClear = true;
    }
    else // 群体启动模式，即对manager的所有file进行拼好件(其会存在互相索引的情况，结构可能得改改了)
    {
        emtEngine._mainfile = nullptr;
        emtEngine._mainmotion = nullptr;
        for (auto itm : _resourceManager->cacheData)
        {
            // 查询
            auto it = itm.second->_objects.find(itm.second->_metadata->chara.c_str());
            if (it != itm.second->_objects.end())
            {
                auto it1 = it->second->motion.find(itm.second->_metadata->motion.c_str());
                if (it1 != it->second->motion.end())
                {
                    emtEngine._mainfile = itm.second;
                    emtEngine._mainmotion = it1->second;
                    break;
                }
            }
        }
        // 对剩下文件进行相互并连(通过引擎管理附属文件)
        for (auto itm : _resourceManager->cacheData)
        {
            if (itm.second != emtEngine._mainfile)
                emtEngine.addEmoteFile(itm.second);
        }
        // start
        clockPassed = 0.0;
        _motion = name;
        _playing = true;
        _allplaying = true;
        isSelfClear = false;
    }
    recordSlowEmoteOperation(false, started);
}
void EmotePlayer::initPhysics(tTJSVariant metadata)
{
    TVPConsoleLog("EmotePlayer::initPhysics TODO");
}
void EmotePlayer::clear(iTJSDispatch2* layer, tjs_uint32 neutralColor)
{
    auto* self = ncbInstanceAdaptor<SeparateLayerAdaptor>::GetNativeInstance(layer);
    tTJSNI_BaseLayer* ths = NULL;
    if (self != nullptr)
    {
        ths = self->GetLayer();
    }
    else
    {
        if (layer->NativeInstanceSupport(TJS_NIS_GETINSTANCE, tTJSNC_Layer::ClassID,
                                         (iTJSNativeInstance**)&ths) < 0)
            return;
        withoutAdaptor = true;
    }
    if (ths == NULL)
        return;

    // 清屏统一走 2D 渲染器（GL 后端清 FBO，软渲染后端清 CPU 缓冲）
    krkrsdl3::iTVPRenderBackend* renderer = krkrsdl3::TVPGetRenderBackend();
    if (renderer)
    {
        void* target = withoutAdaptor ? _target : (self ? self->target : nullptr);
        if (target)
        {
            renderer->SetTarget(target);
            renderer->ClearTarget(true);
            _hitFrame = {};
        }
    }
}
void EmotePlayer::progress(tjs_real mstime)
{
    if (_isStop)
        return;
    const Uint64 profileStarted = SDL_GetTicksNS();
    if (emtEngine._mainfile != nullptr && emtEngine._mainmotion != nullptr && clockPassed > -1.0 &&
        _limitArea.width != _limitArea.originX && _limitArea.height != _limitArea.originY)
    {
        if (_playing)
            clockPassed += mstime / speedRatio;
        // condition
        if (emtEngine._mainfile->_metadata->_varList.size() > 0)
        {
            // 更新控制参数(通过引擎调用)
            emtEngine.updateEyeControl(clockPassed, true);
            emtEngine.updateTimelineControl(clockPassed, true);
        }
        // 是否结束 —— 结束判定与 variableList 参数控制无关：
        // 若仅在 varList 为空分支判定，带眨眼/口型/时间线控制的立绘
        // 会 animating 永真，游戏等待动画结束的对话流程将永久挂起（修复猫娘乐园2出现emote立绘时无法进行任何操作）
        if (!isMotion && clockPassed > emtEngine._mainmotion->lastTime)
        {
            _playing = false;
        }
        // 对于motion限制最后时间并结束
        // 参考ref逻辑: syncTime优先, 其次是selfSyncTime, 最后用lastTime作为兜底
        if (isMotion && emtEngine._mainmotion->loopTime < 0)
        {
            tjs_real endTime = emtEngine._mainmotion->syncTime;
            if (endTime <= 0.0) endTime = emtEngine._mainmotion->selfSyncTime;
            if (endTime <= 0.0) endTime = emtEngine._mainmotion->lastTime;
            if (clockPassed > endTime)
            {
                clockPassed = endTime;
                _playing = false;
            }
        }
        // ping-pong触发更新draw，位置暂时选这里，让它频繁点
        if (_pipoVal == 0)
            _pipoVal = 1;
        else
            _pipoVal = 0;
    }
    krkrsdl3::TVPRecordEmoteProgress(SDL_GetTicksNS() - profileStarted);
}
void EmotePlayer::prepareFrame()
{
    const Uint64 profileStarted = SDL_GetTicksNS();
    krkrsdl3::TVPBeginEmotePrepareDetail();

    const Uint64 transformStarted = SDL_GetTicksNS();
    updateTransMat();
    std::vector<emoteRender> methods{_renderMethod};
    if (emtEngine._mainfile->isMirror)
        methods.front().attachMat = glm::scale(methods.front().attachMat, glm::vec3(-1, 1, 1));
    const float tick = emtEngine._mainfile->_metadata->_varList.empty() ? clockPassed : 0;
    const Uint64 transformTimeNS = SDL_GetTicksNS() - transformStarted;

    const Uint64 motionStarted = SDL_GetTicksNS();
    emtEngine.progress(tick, methods, _limitArea);
    const Uint64 motionTimeNS = SDL_GetTicksNS() - motionStarted;

    const Uint64 snapshotStarted = SDL_GetTicksNS();
    _hitFrame.motion = emtEngine._mainMotionRef;
    _hitFrame.inputToClip = _renderMethod.attachMat;
    _hitFrame.width = _limitArea.viewW > 0 ? _limitArea.viewW : _limitArea.width;
    _hitFrame.height = _limitArea.viewH > 0 ? _limitArea.viewH : _limitArea.height;
    const Uint64 snapshotTimeNS = SDL_GetTicksNS() - snapshotStarted;

    krkrsdl3::TVPCommitEmotePrepareDetail(transformTimeNS, motionTimeNS, snapshotTimeNS);
    krkrsdl3::TVPRecordEmotePrepare(SDL_GetTicksNS() - profileStarted);
}

void EmotePlayer::draw(iTJSDispatch2* objthis)
{
    withD3DAdaptor = withoutAdaptor = false;
    _targetTrans = glm::mat4(1.0f);
    _limitArea.viewW = _limitArea.viewH = 0;
    auto* self = ncbInstanceAdaptor<SeparateLayerAdaptor>::GetNativeInstance(objthis);
    tTJSNI_BaseLayer* ths = NULL;
    D3DAdaptor* d3dAdaptor = NULL;
    if (self != nullptr)
    {
        ths = self->GetLayer();
        if (ths == NULL)
            return;
    }
    if (ths == NULL)
    {
        d3dAdaptor = ncbInstanceAdaptor<D3DAdaptor>::GetNativeInstance(objthis);
        if (d3dAdaptor != nullptr)
            withD3DAdaptor = true;
    }
    if (d3dAdaptor == NULL && ths == NULL)
    {
        if (objthis->NativeInstanceSupport(TJS_NIS_GETINSTANCE, tTJSNC_Layer::ClassID,
                                           (iTJSNativeInstance**)&ths) < 0)
            return;
        withoutAdaptor = true;
    }

    if (emtEngine._mainfile != nullptr && emtEngine._mainmotion != nullptr)
    {
        // 渲染统一走 core/render 的 2D 渲染抽象（GL/软渲染选择在 core 内部完成）
        krkrsdl3::iTVPRenderBackend* renderer = krkrsdl3::TVPGetRenderBackend();
        if (!renderer)
            return;
        void* target = nullptr;
        void* maskTarget = nullptr;
        if (withD3DAdaptor)
        {
            target = d3dAdaptor->_target;
            maskTarget = d3dAdaptor->_maskTarget;
            _limitArea.width = d3dAdaptor->_width;
            _limitArea.height = d3dAdaptor->_height;
            // TODO 关于位置的锚定，后续再研究研究，如何进行全面的统一
            _limitArea.originX = d3dAdaptor->_orgX - d3dAdaptor->_width / 2;
            _limitArea.originY = d3dAdaptor->_orgY - d3dAdaptor->_height / 2;
            _width = d3dAdaptor->_width;
            _height = d3dAdaptor->_height;
            if (emtEngine._mainfile != nullptr)
            {
                _limitArea.zMax = emtEngine.getZMax() * 2;
            }
            if (_limitArea.zMax < 30.0f)
                _limitArea.zMax = 30.0f;
            updateTransMat();
        }
        else
        {
            ResetDrawArea(ths->GetWidth(), ths->GetHeight());
            if (withoutAdaptor)
            {
                target = _target;
                maskTarget = _maskTarget;
            }
            else
            {
                self->checkDrawArea(ths->GetWidth(), ths->GetHeight());
                target = self->target;
                maskTarget = self->maskTarget;
            }
            if (!target || !maskTarget)
                return;
            // 启用
            renderer->SetTarget(target);
            // isSelfClear: 自主清屏模式；否则由脚本 clear() 完成清屏
            renderer->ClearTarget(isSelfClear);
        }
        if (!target) return;
        krkrsdl3::TVPRecordEmotePlayerDraw(
            reinterpret_cast<uintptr_t>(this), reinterpret_cast<uintptr_t>(target));
        prepareFrame();
        const Uint64 drawStarted = SDL_GetTicksNS();
        emtEngine.draw(renderer, target, _limitArea, maskTarget);
        krkrsdl3::TVPRecordEmoteDraw(SDL_GetTicksNS() - drawStarted);
        if (!withD3DAdaptor)
        {
            // The destination is overwritten in full. Keep the frame on the
            // GPU when the Layer renderer can lend us its writable texture.
            void* destination = ths->GetMainImageGPUHandleForOverwrite();
            if (destination && renderer->CopyTargetToLayerTexture(target, destination))
            {
                ths->CommitMainImageGPUOverwrite();
                krkrsdl3::TVPRecordEmoteLayerGPUCopy(
                    static_cast<uint64_t>(_width) * static_cast<uint64_t>(_height) * 4);
                ths->Update();
                return;
            }
            // 回读 CPU 像素并交给图层（GL 后端经 glReadPixels，软渲染后端零拷贝）
            int pitch = 0;
            const Uint64 readbackStarted = SDL_GetTicksNS();
            uint8_t* pixels = renderer->LockTarget(target, pitch);
            krkrsdl3::TVPRecordEmoteLayerCPUReadback(
                pixels && pitch > 0 ? static_cast<uint64_t>(pitch) * static_cast<uint64_t>(_height) : 0,
                SDL_GetTicksNS() - readbackStarted);
            // 紧接的 memcpy 覆盖整个图层（_width/_height 来自上面的 ResetDrawArea），
            // 图层原有像素不会被读取，因此无需为保留它们做一次 GPU 回读。
            tjs_uint8* buff = (tjs_uint8*)ths->GetMainImagePixelBufferForOverwrite();
            if (buff && pixels)
                std::memcpy(buff, pixels, (size_t)_width * _height * 4);
            renderer->UnlockTarget(target);
            // 写入范围即上面这块，据此关闭写租约，GPU 后端只需上传该区域
            ths->ReleaseMainImagePixelBufferForWrite(tTVPRect(0, 0, _width, _height));
            ths->Update();
        }
    }
}

// GPU 直通绘制：直接绘制到给定后端离屏目标（不回读、不经引擎 Layer）
void EmotePlayer::drawToTarget(krkrsdl3::iTVPRenderBackend* renderer,
                               void* target,
                               void* maskTarget,
                               bool selfClear,
                               tjs_int width,
                               tjs_int height,
                               tjs_int originX,
                               tjs_int originY,
                               tjs_int viewW,
                               tjs_int viewH,
                               const glm::mat4& transform)
{
    if (emtEngine._mainfile == nullptr || emtEngine._mainmotion == nullptr)
        return;
    if (!renderer || !target)
        return;
    if (width <= 0 || height <= 0) return;
    _width = width;
    _height = height;
    _limitArea = {(float)originX, (float)originY, (float)width, (float)height,
                  std::max(30.0f, emtEngine.getZMax() * 2), (float)viewW, (float)viewH};
    _targetTrans = transform;
    krkrsdl3::TVPRecordEmotePlayerDraw(
        reinterpret_cast<uintptr_t>(this), reinterpret_cast<uintptr_t>(target));
    prepareFrame();
    renderer->SetTarget(target);
    renderer->ClearTarget(selfClear);
    const Uint64 drawStarted = SDL_GetTicksNS();
    emtEngine.draw(renderer, target, _limitArea, maskTarget);
    krkrsdl3::TVPRecordEmoteDraw(SDL_GetTicksNS() - drawStarted);
}
void EmotePlayer::assign(iTJSDispatch2* anotherAdaptor)
{
    TVPConsoleLog("EmotePlayer::assign TODO");
}
void EmotePlayer::setCoord(tjs_real x, tjs_real y)
{
    currCoordx = x;
    currCoordy = y;
    updateTransMat();
}
void EmotePlayer::setScale(tjs_real scale)
{
    currZx = scale;
    currZy = scale;
    updateTransMat();
}
void EmotePlayer::setRotate(tjs_real rotate)
{
    currAngle = rotate;
    updateTransMat();
}
void EmotePlayer::setColor(tjs_uint32 color)
{
    TVPConsoleLog("EmotePlayer::setColor TODO");
}
void EmotePlayer::setVariable(tTJSString name, tjs_real value)
{
    if (emtEngine._mainfile != nullptr)
    {
        std::string tmpName = name.AsStdString();
        tmpName.append(1, '\0'); // 终有一天，我会把这sb字符串给优化掉
        emtEngine.setVariable(tmpName, value); // 管你有没有，设了再说
    }
}
tjs_real EmotePlayer::getVariable(tTJSString name)
{
    if (emtEngine._mainfile != nullptr)
    {
        std::string tmpName = name.AsStdString();
        tmpName.append(1, '\0'); // 终有一天，我会把这sb字符串给优化掉
        return emtEngine.getVariable(tmpName);
    }
    return 0.0;
}
void EmotePlayer::setOuterForce(tTJSString name, tjs_real ofx, tjs_real ofy)
{
    TVPConsoleLog("EmotePlayer::setOuterForce TODO");
}
void EmotePlayer::setDrawAffineTranslateMatrix(
    tjs_real a, tjs_real b, tjs_real c, tjs_real d, tjs_int tx, tjs_int ty)
{
    if (emtEngine._mainfile != nullptr)
    {
        _affineTrans = glm::mat4(a, -c, 0.0f, 0.0f, -b, d, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, tx,
                                 ty, 0.0f, 1.0f);
        updateTransMat();
    }
}
void EmotePlayer::setCameraOffset(tjs_int w, tjs_int h)
{
    currCamX = w;
    currCamY = h;
}
void EmotePlayer::startWind(
    tjs_real start, tjs_real goal, tjs_real speed, tjs_real powMin, tjs_real powMax)
{
    TVPConsoleLog("EmotePlayer::startWind TODO");
}
void EmotePlayer::stopWind()
{
    TVPConsoleLog("EmotePlayer::stopWind TODO");
}
tjs_error EmotePlayer::cb_contains(
    tTJSVariant* result, tjs_int numparams, tTJSVariant** param, EmotePlayer* objthis)
{
    if (!objthis) return TJS_E_FAIL;
    return dispatchEmoteHitTest(result, numparams, param,
        [&](const char* label, tjs_real x, tjs_real y)
        { return objthis->_hitFrame.contains(label, x, y, label != nullptr); });
}
tjs_error EmotePlayer::cb_hitTest(
    tTJSVariant* result, tjs_int numparams, tTJSVariant** param, EmotePlayer* objthis)
{
    if (!objthis) return TJS_E_FAIL;
    return dispatchEmoteHitTest(result, numparams, param,
        [&](const char* label, tjs_real x, tjs_real y)
        { return objthis->_hitFrame.contains(label, x, y); });
}
void EmotePlayer::skip()
{
    TVPConsoleLog("EmotePlayer::skip TODO");
}
void EmotePlayer::skipToSync()
{
    if (emtEngine._mainmotion != nullptr)
    {
        clockPassed = emtEngine._mainfile->getSyncTime();
    }
    TVPConsoleLog("EmotePlayer::skipToSync TODO");
}
void EmotePlayer::pass()
{
    TVPConsoleLog("EmotePlayer::pass TODO");
}
void EmotePlayer::stop()
{
    _isStop = true;
    _playing = false;
    _allplaying = false;
    TVPConsoleLog("EmotePlayer::stop TODO");
}
void EmotePlayer::playTimeline(tTJSString name, tjs_int flags)
{
    if (emtEngine._mainfile != nullptr)
    {
        emtEngine.startTimeline(-10000.0f, name.AsStdString(),
                                true); // 管你有没有，设了再说
    }
}
void EmotePlayer::stopTimeline(tTJSString name)
{
    if (emtEngine._mainfile != nullptr)
    {
        emtEngine.stopTimeline(name.AsStdString(), true); // 管你有没有，设了再说
    }
}
bool EmotePlayer::getTimelinePlaying(tTJSString name)
{
    if (emtEngine._mainfile != nullptr)
    {
        bool ret = false;
        if (emtEngine.checkTimline(name.AsStdString(), ret, true))
            return ret;
    }
    return false;
}
bool EmotePlayer::getLoopTimeline(tTJSString name)
{
    if (emtEngine._mainfile != nullptr)
    {
        for (auto itm : emtEngine._mainfile->_metadata->_timelineControl)
        {
            if (strcmp(itm->label.c_str(), name.AsStdString().c_str()) == 0)
            {
                return itm->lastTime < 0;
            }
        }
    }
    return false;
}
tjs_real EmotePlayer::getTimelineTotalFrameCount(tTJSString name)
{
    if (emtEngine._mainfile != nullptr)
    {
        for (auto itm : emtEngine._mainfile->_metadata->_timelineControl)
        {
            if (strcmp(itm->label.c_str(), name.AsStdString().c_str()) == 0)
            {
                return itm->loopEnd - itm->loopBegin + 1;
            }
        }
    }
    return 0;
}
tTJSVariant EmotePlayer::getMainTimelineLabelList()
{
    iTJSDispatch2* array = TJSCreateArrayObject();
    if (emtEngine._mainfile != nullptr)
    {
        for (auto itm : emtEngine._mainfile->_metadata->_timelineControl)
        {
            if (itm->diff == 0)
            {
                tTJSVariant tmp(ttstr(itm->label));
                tTJSVariant* args[] = {&tmp};
                static tjs_uint addHint = 0;
                array->FuncCall(0, TJS_N("add"), &addHint, nullptr, 1, args, array);
            }
        }
    }
    tTJSVariant result(array, array);
    array->Release();
    return result;
}
tTJSVariant EmotePlayer::getDiffTimelineLabelList()
{
    iTJSDispatch2* array = TJSCreateArrayObject();
    if (emtEngine._mainfile != nullptr)
    {
        for (auto itm : emtEngine._mainfile->_metadata->_timelineControl)
        {
            if (itm->diff == 1)
            {
                tTJSVariant tmp(ttstr(itm->label));
                tTJSVariant* args[] = {&tmp};
                static tjs_uint addHint = 0;
                array->FuncCall(0, TJS_N("add"), &addHint, nullptr, 1, args, array);
            }
        }
    }
    tTJSVariant result(array, array);
    array->Release();
    return result;
}
void EmotePlayer::setTimelineBlendRatio(tTJSString name,
                                        tjs_real ratio,
                                        tjs_real time,
                                        tjs_real easing)
{
    TVPConsoleLog("EmotePlayer::setTimelineBlendRatio TODO");
}
void EmotePlayer::fadeInTimeline(tTJSString name, tjs_real time, tjs_real easing)
{
    playTimeline(name);
}
void EmotePlayer::fadeOutTimeline(tTJSString name, tjs_real time, tjs_real easing)
{
    stopTimeline(name);
}
tTJSVariant EmotePlayer::getPlayingTimelineInfoList()
{
    iTJSDispatch2* array = TJSCreateArrayObject();
    tTJSVariant result(array, array);
    if (emtEngine._mainfile != nullptr)
    {
        for (auto playingTimeline : emtEngine.currTimeline)
        {
            iTJSDispatch2* obj = TJSCreateDictionaryObject();
            tTJSVariant val = tTJSVariant(playingTimeline->label);
            obj->PropSet(TJS_MEMBERENSURE, TJS_N("label"), NULL, &val, obj);
            tTJSVariant objItm(obj, obj);
            obj->Release();
            tTJSVariant tmp(objItm);
            tTJSVariant* args[] = {&tmp};
            static tjs_uint addHint = 0;
            array->FuncCall(0, TJS_N("add"), &addHint, nullptr, 1, args, array);
        }
    }
    array->Release();
    return result;
}
tTJSVariant EmotePlayer::getVariableFrameList(tTJSString name)
{
    if (emtEngine._mainfile != nullptr)
    {
        return emtEngine._mainfile->readVariableFrameList(name);
    }
    else
    {
        iTJSDispatch2* array = TJSCreateArrayObject();
        TVPConsoleLog("EmotePlayer::getVariableFrameList TODO");
        tTJSVariant result(array, array);
        array->Release();
        return result;
    }
}
tTJSVariant EmotePlayer::getCommandList()
{
    iTJSDispatch2* dsp = TJSCreateArrayObject();

    // 让它能触发更新就行了
    tTJSVariant tmp(_pipoVal);
    tTJSVariant* args[] = {&tmp};
    static tjs_uint addHint = 0;
    dsp->FuncCall(0, TJS_N("add"), &addHint, nullptr, 1, args, dsp);

    tTJSVariant var(dsp);
    dsp->Release();
    return var;
}
tTJSVariant EmotePlayer::getLayerGetter(tTJSString name)
{
    return makeLayerGetter(this, _hitFrame.motion.get(), name.c_str());
}
tTJSVariant EmotePlayer::getLayerMotion(tTJSString name)
{
    // 调用getLayerGetter后返回motion属性
    tTJSVariant getter = getLayerGetter(name);
    if (getter.Type() == tvtObject)
    {
        iTJSDispatch2* dsp = getter.AsObjectNoAddRef();
        if (dsp)
        {
            tTJSVariant motion;
            if (TJS_SUCCEEDED(dsp->PropGet(0, TJS_N("motion"), NULL, &motion, dsp)))
            {
                return motion;
            }
        }
    }
    return tTJSVariant();
}
void EmotePlayer::setFlip(bool isFlip)
{
    if (isFlip)
        currZy = -currZy;
}
void EmotePlayer::setSlant(tjs_real x, tjs_real y)
{
    // unknow
}
void EmotePlayer::setZoom(tjs_real x, tjs_real y)
{
    currZx = x;
    currZy = y;
    updateTransMat();
}
void EmotePlayer::updateTransMat()
{
    _renderMethod.type = 3;
    _renderMethod.opa = 1.0f;
    if (_limitArea.width <= 0 || _limitArea.height <= 0) return;
    // 构建变换矩阵
    glm::mat4 projection = glm::ortho(-_limitArea.originX, _limitArea.width - _limitArea.originX,
                                      _limitArea.height - _limitArea.originY, -_limitArea.originY,
                                      _limitArea.zMax, -_limitArea.zMax);
    _renderMethod.attachMat = projection * _targetTrans * _affineTrans;
    _renderMethod.currCoordx = currCoordx + currCamX;
    _renderMethod.currCoordy = currCoordy + currCamY;
    _renderMethod.currAngle = currAngle;
    _renderMethod.currZx = currZx;
    _renderMethod.currZy = currZy;
    // 记录根投影参照系（shape 判定区域 clip→像素换算所用，
    // 须与 cb_contains 的判定使用同一 limitArea；未传真实视口时的回退基准）
    _renderMethod.originX = _limitArea.originX;
    _renderMethod.originY = _limitArea.originY;
    _renderMethod.width = _limitArea.width;
    _renderMethod.height = _limitArea.height;
}
void EmotePlayer::ResetDrawArea(tjs_int width, tjs_int height)
{
    if (_width != width || _height != height || !_target || !_maskTarget ||
        _limitArea.originX != 0 || _limitArea.originY != 0)
    {
        // limit
        _limitArea.originX = 0;
        _limitArea.originY = 0;
        _width = width;
        _height = height;
        _limitArea.width = width;
        _limitArea.height = height;
        if (emtEngine._mainfile != nullptr)
        {
            _limitArea.zMax = emtEngine.getZMax() * 2;
        }
        if (_limitArea.zMax < 30.0f)
            _limitArea.zMax = 30.0f;
        // transForm
        updateTransMat();
        // 渲染目标（主体 + 蒙版）由 core/render 的 2D 渲染器统一管理
        krkrsdl3::iTVPRenderBackend* renderer = krkrsdl3::TVPGetRenderBackend();
        if (renderer)
        {
            if (_target)
            {
                renderer->DestroyTarget(_target);
                _target = nullptr;
            }
            if (_maskTarget)
            {
                renderer->DestroyTarget(_maskTarget);
                _maskTarget = nullptr;
            }
            _target = renderer->CreateTarget(_width, _height);
            _maskTarget = renderer->CreateTarget(_width, _height);
        }
    }
}

} // namespace emoteplayer
