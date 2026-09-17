extern "C"
{
#include "libswscale/swscale.h"
}

#include "tjsCommHead.h"

#include "KRMovieOverlay.h"
#include "NativeEventQueue.h"
#include "CodecVideo.h"
#include "PlatformAudio.h"
#include "TVPDebug.h"
#include "TVPMsg.h"

#include <atomic>

namespace
{
std::atomic<tjs_uint64> TVPMovieSessionGeneration{0};
}

void TVPBeginMovieSession()
{
    const auto generation = TVPMovieSessionGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    TVPAddImportantLog(TVPFormatMessage(TJS_N("(info) Movie session %1 started"),
                                       static_cast<tjs_int64>(generation)));
}

void TVPInvalidateMovieSession()
{
    const auto generation = TVPMovieSessionGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    TVPAddImportantLog(TVPFormatMessage(TJS_N("(info) Movie session invalidated at %1"),
                                       static_cast<tjs_int64>(generation)));
}

tjs_uint64 TVPGetMovieSessionGeneration()
{
    return TVPMovieSessionGeneration.load(std::memory_order_acquire);
}

NS_KRMOVIE_BEGIN
#define DRAW_VIDEO_FRAME 30

VideoPresentOverlay::VideoPresentOverlay()
{
    sessionGeneration = TVPGetMovieSessionGeneration();
    pSprite = new TVPSprite;
    pSprite->isVisible = true;
    pSprite->type = 2;
    pSprite->xPos = 0;
    pSprite->yPos = 0;
}

VideoPresentOverlay::~VideoPresentOverlay()
{
    // Session-switch diagnostics: a player destroyed right after presenting its
    // first frame means the sprite leaves the compositor while the game still
    // expects the movie on screen.
    TVPAddImportantLog(ttstr(TJS_N("(info) Video overlay destroyed: session ")) +
                       ttstr((tjs_int64)sessionGeneration) + TJS_N(", joined ") +
                       ttstr((tjs_int)spriteJoined) + TJS_N(", presented ") +
                       ttstr((tjs_int)firstFramePresented));
    if (pSprite != NULL)
    {
        if (spriteJoined)
            krkrsdl3::TVPDepartTexture(pSprite);
        spriteJoined = false;
        if (pSprite->texture != nullptr)
            krkrsdl3::TVPDestroyTexture(pSprite);
        delete pSprite;
        pSprite = nullptr;
    }
    if (continuousHookRegistered)
    {
        TVPRemoveContinuousEventHook(this);
        continuousHookRegistered = false;
    }
}

void VideoPresentOverlay::Play()
{
    if (pSprite != NULL)
        pSprite->isVisible = true;
    TVPMoviePlayer::Play();
}

void VideoPresentOverlay::Stop()
{
    if (pSprite != NULL)
        pSprite->isVisible = false;
    TVPMoviePlayer::Stop();
}

void VideoPresentOverlay::SetVisible(bool b)
{
    // Compositor visibility is owned by Play()/Stop() and by session teardown,
    // not by this call. tTJSNI_VideoOverlay::ResetOverlayParams() re-pushes the
    // wrapper's Visible member on every window geometry change, and that member
    // is still false while a freshly opened movie is already playing, so binding
    // pSprite->isVisible here hid movies the game was still showing.
    TVPMoviePlayer::SetVisible(b);
}

void VideoPresentOverlay::OnContinuousCallback(tjs_uint64 tick)
{
    if (sessionGeneration != TVPGetMovieSessionGeneration())
        return;
    if (!m_usedPicture)
        return;
    double m_curpts = m_pPlayer->GetClock() / DVD_TIME_BASE;
    {
        tTJSCriticalSectionHolder lk(m_mtxPicture);
        BitmapPicture& picbuf = m_picture[m_curPicture];
        // check pts
        if (picbuf.pts > m_curpts)
        { // present in future
            return;
        }
    }

    BitmapPicture pic;
    do
    { // skip frame
        pic.Clear();
        m_picture[m_curPicture].swap(pic);
        m_curPicture = (m_curPicture + 1) & (MAX_BUFFER_COUNT - 1);
        --m_usedPicture;
    } while (m_usedPicture > 0 && m_curpts >= m_picture[m_curPicture].pts);
    assert(m_usedPicture >= 0);
    m_condPicture.notify_all();

    FrameMove();
    if (pic.rgba == NULL)
        return;
    {
        if (pSprite->texture == nullptr)
        {
            pSprite->width = pic.width;
            pSprite->height = pic.height;
            krkrsdl3::TVPCreateTexture(*pSprite);
            if (!pSprite->texture)
            {
                TVPAddImportantLog(TJS_N("(error) Video overlay texture creation failed"));
                return;
            }
        }
        if (!spriteJoined)
        {
            krkrsdl3::TVPJoinTexture(pSprite);
            spriteJoined = true;
        }
        int pitch = pic.width * 4;
        krkrsdl3::TVPUpdateTexture(pSprite, pic.rgba, pic.width, pic.height, pitch);
        if (!firstFramePresented)
        {
            firstFramePresented = true;
            TVPAddImportantLog(TVPFormatMessage(
                TJS_N("(info) Video overlay first frame: %1x%2"),
                pic.width, pic.height));
        }
    }
}

MoviePlayerOverlay::~MoviePlayerOverlay()
{
    delete m_pPlayer;
    m_pPlayer = nullptr;
}

void MoviePlayerOverlay::SetWindow(tTJSNI_Window* window)
{
    if (!continuousHookRegistered)
    {
        TVPAddContinuousEventHook(this);
        continuousHookRegistered = true;
    }
}

void MoviePlayerOverlay::BuildGraph(iTVPVideoCallback* callbackwin,
                                    tTJSBinaryStream* stream,
                                    const tjs_char* streamname,
                                    const tjs_char* type,
                                    uint64_t size)
{
    m_pCallbackWin = callbackwin;
    m_pPlayer->SetCallback(std::bind(&MoviePlayerOverlay::OnPlayEvent, this, std::placeholders::_1,
                                     std::placeholders::_2));
    m_pPlayer->OpenFromStream(stream, streamname, type, size);
}

void KRMovie::MoviePlayerOverlay::SetVisible(bool b)
{
    VideoPresentOverlay::SetVisible(b);
}

void MoviePlayerOverlay::OnPlayEvent(KRMovieEvent msg, void* p)
{
    if (msg == KRMovieEvent::Ended)
    {
        NativeEvent ev(WM_GRAPHNOTIFY);
        ev.WParam = EC_COMPLETE;
        ev.LParam = 0;
        m_pCallbackWin->PostEvent(ev);
    }
}

NS_KRMOVIE_END
