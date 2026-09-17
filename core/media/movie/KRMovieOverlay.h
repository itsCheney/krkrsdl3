#pragma once

#include "krffmpeg.h"
#include "TVPEvent.h"
#include "ComplexRect.h"

#include "TVPCompositor.h"

struct SwsContext;
class iTVPSoundBuffer;

NS_KRMOVIE_BEGIN

class VideoPresentOverlay : public TVPMoviePlayer, public tTVPContinuousEventCallbackIntf
{
protected:
    TVPSprite* pSprite;
    bool spriteJoined = false;
    bool continuousHookRegistered = false;
    tjs_uint64 sessionGeneration = 0;
    bool firstFramePresented = false;

    VideoPresentOverlay();
    ~VideoPresentOverlay();

public:
    virtual void Stop() override;
    virtual void Play() override;
    virtual void SetVisible(bool b) override;
    virtual void OnContinuousCallback(tjs_uint64 tick) override;
};

class MoviePlayerOverlay : public VideoPresentOverlay
{
    iTVPVideoCallback* m_pCallbackWin = nullptr;

    void OnPlayEvent(KRMovieEvent msg, void* p);

public:
    ~MoviePlayerOverlay();
    virtual void SetWindow(class tTJSNI_Window* window) override;

    void BuildGraph(iTVPVideoCallback* callbackwin,
                    tTJSBinaryStream* stream,
                    const tjs_char* streamname,
                    const tjs_char* type,
                    uint64_t size);

    virtual void SetVisible(bool b) override;
};

NS_KRMOVIE_END

// Embedded hosts run multiple games in one process. A generation invalidates
// decoder callbacks that arrive after their owning game session has ended.
void TVPBeginMovieSession();
void TVPInvalidateMovieSession();
tjs_uint64 TVPGetMovieSessionGeneration();
