#include "tjsCommHead.h"
#include "WaveDecodeThread.h"

#include "Platform.h"
#include "PlatformAudio.h"
#include "PlatformMutex.h"
#include "PlatformThread.h"

#include "NativeEventQueue.h"
#include "Random.h"

#include "TVPEvent.h"
#include "TVPSystem.h"
#include "TVPSettings.h"

#include <chrono>
#include <condition_variable>
#include <mutex>

static bool TVPControlPrimaryBufferRun = true;
#if defined(_KRKRSDL3_EMSCRIPTEN)
static int TVPDecodeThreadCount = 1;
#else
static int TVPDecodeThreadCount = 0; // 0 = auto (number of CPUs)
#endif

//---------------------------------------------------------------------------
// tTVPWaveSoundBufferDecodePool : shared decode thread pool
//---------------------------------------------------------------------------
class tTVPWaveSoundBufferDecodePool
{
    friend class tTVPWaveSoundBufferDecodePoolWorker;

    class tTVPWaveSoundBufferDecodePoolWorker : public tTVPThread
    {
        tTVPWaveSoundBufferDecodePool* Pool;

    public:
        tTVPWaveSoundBufferDecodePoolWorker(tTVPWaveSoundBufferDecodePool* pool)
          : tTVPThread("TVPWaveSoundBufferDecodePool"),
            Pool(pool)
        {
            Resume();
        }
        ~tTVPWaveSoundBufferDecodePoolWorker()
        {
            Terminate();
            Resume();
            WaitFor();
        }
        void Execute() override
        {
            while (!GetTerminated() && !Pool->_terminated)
                Pool->WorkerLoop();
        }
    };

    // Consumption (ReadL2Buffer) wakes the pool when a unit becomes free.
    // Keep a timeout as a fallback while a buffer remains active.
    static constexpr int TVPFullBufferWaitMS = 50;

    tTJSCriticalSection _cs;
    // tTVPThreadEvent is a bare condition variable: a Set() with no waiter is
    // lost. The serial makes a wake that races a worker's scan observable.
    std::mutex _waitMutex;
    std::condition_variable _waitCond;
    uint64_t _wakeSerial = 0;
    std::atomic_bool _terminated{false};
    std::vector<tTVPWaveSoundBufferDecodePoolWorker*> _workers;
    std::vector<iTVPDecodeSoundBuffer*> _buffers;
    std::atomic_size_t _nextCheck{0};

public:
    tTVPWaveSoundBufferDecodePool()
    {
        int n = TVPDecodeThreadCount;
        if (n <= 0)
            n = std::max(1, TVPGetProcessorNum());
        n = std::min(n, TVPMaxThreadNum);
        _workers.resize(n);
        for (int i = 0; i < n; i++)
            _workers[i] = new tTVPWaveSoundBufferDecodePoolWorker(this);
    }

    ~tTVPWaveSoundBufferDecodePool()
    {
        _terminated = true;
        Wake();
        for (auto* w : _workers)
            delete w;
    }

    void AddBuffer(iTVPDecodeSoundBuffer* buf)
    {
        tTJSCriticalSectionHolder holder(_cs);
        _buffers.push_back(buf);
    }

    void RemoveBuffer(iTVPDecodeSoundBuffer* buf)
    {
        buf->DecodeActive.store(false, std::memory_order_release);
        {
            tTJSCriticalSectionHolder holder(_cs);
            for (auto it = _buffers.begin(); it != _buffers.end(); ++it)
            {
                if (*it == buf)
                {
                    _buffers.erase(it);
                    break;
                }
            }
        }
        // Wait for any in-flight decode on this buffer to complete
        while (buf->DecodeInProgress.load(std::memory_order_acquire))
            TVPSleepFor(1);
    }

    void Wake()
    {
        {
            std::lock_guard<std::mutex> lock(_waitMutex);
            ++_wakeSerial;
        }
        _waitCond.notify_all();
    }

private:
    void WorkerLoop()
    {
        uint64_t serial;
        {
            std::lock_guard<std::mutex> lock(_waitMutex);
            serial = _wakeSerial;
        }
        // Offer every playing buffer one unit per pass. A buffer stays
        // DecodeActive while its L2 ring is full, and FillL2Buffer then returns
        // false at once; looping without a wait kept one core at 100% per
        // playing sound. Only wait once no buffer took a unit.
        bool anyActive = false, filledAny = false;
        const size_t start = _nextCheck.fetch_add(1, std::memory_order_relaxed);
        for (size_t i = 0;; i++)
        {
            iTVPDecodeSoundBuffer* target = nullptr;
            {
                tTJSCriticalSectionHolder holder(_cs);
                const size_t n = _buffers.size();
                if (i >= n)
                    break;
                auto* buf = _buffers[(start + i) % n];
                if (buf->DecodeActive.load(std::memory_order_acquire))
                {
                    anyActive = true;
                    bool expected = false;
                    if (buf->DecodeInProgress.compare_exchange_strong(expected, true,
                                                                      std::memory_order_acq_rel))
                        target = buf;
                }
            }
            if (target)
            {
                if (target->FillL2Buffer(false, true))
                    filledAny = true;
                target->DecodeInProgress.store(false, std::memory_order_release);
            }
        }
        if (filledAny)
            return;
        // Wait for Play, consumption or shutdown. With nothing playing there
        // is no timeout: idle workers no longer wake 100 times a second each.
        std::unique_lock<std::mutex> lock(_waitMutex);
        auto woken = [&] { return _wakeSerial != serial || _terminated; };
        if (anyActive)
            _waitCond.wait_for(lock, std::chrono::milliseconds(TVPFullBufferWaitMS), woken);
        else
            _waitCond.wait(lock, woken);
    }
};
//---------------------------------------------------------------------------
static tTVPWaveSoundBufferDecodePool* _tvpSoundBufferPool = nullptr;
void TVPInitWaveDecodeThread()
{
    if (_tvpSoundBufferPool)
        return;

    tTJSVariant val;
    if (TVPGetCommandLine(TJS_N("-wscontrolpri"), &val))
    {
        if (ttstr(val) == TJS_N("yes"))
            TVPControlPrimaryBufferRun = true;
        else
            TVPControlPrimaryBufferRun = false;
    }
    if (TVPGetCommandLine(TJS_N("-wsdecodethreads"), &val))
    {
        tjs_int n = (tjs_int)val;
        if (n >= 0 && n <= TVPMaxThreadNum)
            TVPDecodeThreadCount = n;
    }
    _tvpSoundBufferPool = new tTVPWaveSoundBufferDecodePool;
}
void TVPUninitWaveDecodeThread()
{
    if (_tvpSoundBufferPool)
    {
        delete _tvpSoundBufferPool;
        _tvpSoundBufferPool = nullptr;
    }
}
void TVPAddDecodeSoundBuffer(iTVPDecodeSoundBuffer* buf)
{
    if (!_tvpSoundBufferPool)
        return;
    _tvpSoundBufferPool->AddBuffer(buf);
}
void TVPRemoveDecodeSoundBuffer(iTVPDecodeSoundBuffer* buf)
{
    if (!_tvpSoundBufferPool)
        return;
    _tvpSoundBufferPool->RemoveBuffer(buf);
}
void TVPWakeDecodeSoundBuffer()
{
    if (!_tvpSoundBufferPool)
        return;
    _tvpSoundBufferPool->Wake();
}

//---------------------------------------------------------------------------
// DirectSound management
//---------------------------------------------------------------------------
static bool TVPPrimaryBufferPlayingByProgram = false;
bool TVPPrimarySoundBufferPlaying = false;
bool TVPDirectSoundShutdown = false;
bool TVPDeferedSettingAvailable = false;
//---------------------------------------------------------------------------
void TVPEnsurePrimaryBufferPlay()
{
    if (!TVPControlPrimaryBufferRun)
        return;
    TVPInitDirectSound();
    if (!TVPPrimaryBufferPlayingByProgram)
    {
        TVPPrimaryBufferPlayingByProgram = true;
    }
}
//---------------------------------------------------------------------------
void TVPMakeSilentWave(void* dest, tjs_int count, const tTVPWaveFormat* format)
{
    tjs_int bytes = count * format->Channels * format->BytesPerSample;
    memset(dest, 0x00, bytes);
    // TVPMakeSilentWaveBytes(dest, bytes, format);
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// Buffer management
//---------------------------------------------------------------------------
std::vector<iTVPDecodeSoundBuffer*>& TVPWaveSoundBufferVector =
    *(new std::vector<iTVPDecodeSoundBuffer*>); // to avoid release order in
                                                 // shutdown
tTJSCriticalSection TVPWaveSoundBufferVectorCS;

//---------------------------------------------------------------------------
// tTVPWaveSoundBufferThread : playing thread
//---------------------------------------------------------------------------
/*
    The system has one playing thread.
    The playing thread fills each sound buffer's L1 (DirectSound) buffer, and
    also manages timing for label events.
    The technique used in this algorithm is similar to Timer claass
   implementation.
*/
class tTVPWaveSoundBufferThread : public tTVPThread
{
    tTVPThreadEvent Event;

    // HWND UtilWindow; // utility window to notify the pending events occur
    bool PendingLabelEventExists;
    bool WndProcToBeCalled;
    uint64_t NextLabelEventTick;
    uint64_t LastFilledTick;

    NativeEventQueue<tTVPWaveSoundBufferThread> EventQueue;

public:
    tTVPWaveSoundBufferThread();
    ~tTVPWaveSoundBufferThread();

private:
    // void __fastcall UtilWndProc(Messages::TMessage &Msg);
    void UtilWndProc(NativeEvent& ev);

public:
    void ReschedulePendingLabelEvent(tjs_int tick);

protected:
    void Execute(void);

public:
    void Start(void);
} static* TVPWaveSoundBufferThread = NULL;
//---------------------------------------------------------------------------
void TVPLockSoundMixer()
{
    TVPPrimaryBufferPlayingByProgram = false;
}
void TVPUnlockSoundMixer()
{
    if (TVPWaveSoundBufferThread)
        TVPEnsurePrimaryBufferPlay();
}
//---------------------------------------------------------------------------
tTVPWaveSoundBufferThread::tTVPWaveSoundBufferThread()
  : tTVPThread("TVPWaveSoundBufferThread"),
    EventQueue(this, &tTVPWaveSoundBufferThread::UtilWndProc)
{
    EventQueue.Allocate();
    PendingLabelEventExists = false;
    NextLabelEventTick = 0;
    LastFilledTick = 0;
    WndProcToBeCalled = false;
    SetPriority(ttpHighest);
    Resume();
}
//---------------------------------------------------------------------------
tTVPWaveSoundBufferThread::~tTVPWaveSoundBufferThread()
{
    SetPriority(ttpNormal);
    Terminate();
    Resume();
    Event.Set();
    WaitFor();
    EventQueue.Deallocate();
}
//---------------------------------------------------------------------------
// void __fastcall tTVPWaveSoundBufferThread::UtilWndProc(Messages::TMessage
// &Msg)
void tTVPWaveSoundBufferThread::UtilWndProc(NativeEvent& ev)
{
    // Window procedure of UtilWindow
    if (ev.Message == TVP_EV_WAVE_SND_BUF_THREAD && !GetTerminated())
    {
        // pending events occur
        tTJSCriticalSectionHolder holder(TVPWaveSoundBufferVectorCS); // protect the object

        WndProcToBeCalled = false;

        tjs_int64 tick = TVPGetTickCount();

        int nearest_next = TVP_TIMEOFS_INVALID_VALUE;

        std::vector<iTVPDecodeSoundBuffer*>::iterator i;
        for (i = TVPWaveSoundBufferVector.begin(); i != TVPWaveSoundBufferVector.end(); i++)
        {
            int next = (*i)->FireLabelEventsAndGetNearestLabelEventStep(tick);
            // fire label events and get nearest label event step
            if (next != TVP_TIMEOFS_INVALID_VALUE)
            {
                if (nearest_next == TVP_TIMEOFS_INVALID_VALUE || nearest_next > next)
                    nearest_next = next;
            }
        }

        if (nearest_next != TVP_TIMEOFS_INVALID_VALUE)
        {
            PendingLabelEventExists = true;
            NextLabelEventTick = TVPGetRoughTickCount() + nearest_next;
        }
        else
        {
            PendingLabelEventExists = false;
        }
    }
    else
    {
        EventQueue.HandlerDefault(ev);
    }
}
//---------------------------------------------------------------------------
void tTVPWaveSoundBufferThread::ReschedulePendingLabelEvent(tjs_int tick)
{
    if (tick == TVP_TIMEOFS_INVALID_VALUE)
        return; // no need to reschedule
    uint64_t eventtick = TVPGetRoughTickCount() + tick;

    tTJSCriticalSectionHolder holder(TVPWaveSoundBufferVectorCS);

    if (PendingLabelEventExists)
    {
        if (NextLabelEventTick - eventtick > 0)
            NextLabelEventTick = eventtick;
    }
    else
    {
        PendingLabelEventExists = true;
        NextLabelEventTick = eventtick;
    }
}
//---------------------------------------------------------------------------
#define TVP_WSB_THREAD_SLEEP_TIME 60
void tTVPWaveSoundBufferThread::Execute(void)
{
    while (!GetTerminated())
    {
        // thread loop for playing thread
        uint64_t time = TVPGetRoughTickCount();
        TVPPushEnvironNoise(&time, sizeof(time));

        { // thread-protected
            tTJSCriticalSectionHolder holder(TVPWaveSoundBufferVectorCS);

            if (TVPPrimaryBufferPlayingByProgram != TVPPrimarySoundBufferPlaying)
            {
                TVPPrimarySoundBufferPlaying = TVPPrimaryBufferPlayingByProgram;
                std::vector<iTVPDecodeSoundBuffer*>::iterator i;
                for (i = TVPWaveSoundBufferVector.begin(); i != TVPWaveSoundBufferVector.end(); i++)
                {
                    if ((*i)->ThreadCallbackEnabled)
                        (*i)->SetBufferPaused(!TVPPrimaryBufferPlayingByProgram); // for
                                                                                  // preventing
                                                                                  // buffer runs
                                                                                  // out on iOS'
                                                                                  // OpenAL
                                                                                  // implement
                }
            }

            // check PendingLabelEventExists
            if (PendingLabelEventExists)
            {
                if (!WndProcToBeCalled)
                {
                    WndProcToBeCalled = true;
                    EventQueue.PostEvent(NativeEvent(TVP_EV_WAVE_SND_BUF_THREAD));
                }
            }

            if (TVPPrimarySoundBufferPlaying && time - LastFilledTick >= TVP_WSB_THREAD_SLEEP_TIME)
            {
                std::vector<iTVPDecodeSoundBuffer*>::iterator i;
                for (i = TVPWaveSoundBufferVector.begin(); i != TVPWaveSoundBufferVector.end(); i++)
                {
                    if ((*i)->ThreadCallbackEnabled)
                        (*i)->FillBuffer(false, true); // fill sound buffer
                }
                LastFilledTick = time;
            }
        } // end-of-thread-protected

        uint64_t time2;
        time2 = TVPGetRoughTickCount();
        time = time2 - time;

        if (time < TVP_WSB_THREAD_SLEEP_TIME)
        {
            tjs_int sleep_time = TVP_WSB_THREAD_SLEEP_TIME - time;
            if (PendingLabelEventExists)
            {
                tjs_int step_to_next = (tjs_int32)NextLabelEventTick - (tjs_int32)time2;
                if (step_to_next < sleep_time)
                    sleep_time = step_to_next;
                if (sleep_time < 1)
                    sleep_time = 1;
            }
            Event.WaitFor(sleep_time);
        }
        else
        {
            Event.WaitFor(1);
        }
    }
}
//---------------------------------------------------------------------------
void tTVPWaveSoundBufferThread::Start()
{
    TVPPrimaryBufferPlayingByProgram = true;
    Event.Set();
    Resume();
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
static void TVPReleaseSoundBuffers(bool disableevent = true)
{
    // release all secondary buffers.
    tTJSCriticalSectionHolder holder(TVPWaveSoundBufferVectorCS);
    std::vector<iTVPDecodeSoundBuffer*>::iterator i;
    for (i = TVPWaveSoundBufferVector.begin(); i != TVPWaveSoundBufferVector.end(); i++)
    {
        (*i)->FreeDirectSoundBuffer(disableevent);
    }
    TVPWaveSoundBufferVector.clear();
}
//---------------------------------------------------------------------------
static void TVPShutdownWaveSoundBuffers()
{
    // clean up soundbuffers at exit
    if (TVPWaveSoundBufferThread)
        delete TVPWaveSoundBufferThread, TVPWaveSoundBufferThread = NULL;
    TVPReleaseSoundBuffers();
    TVPPrimaryBufferPlayingByProgram = false;
}
static tTVPAtExit TVPShutdownWaveSoundBuffersAtExit(TVP_ATEXIT_PRI_PREPARE,
                                                    TVPShutdownWaveSoundBuffers);
//---------------------------------------------------------------------------
void TVPEnsureWaveSoundBufferWorking()
{
    if (!TVPWaveSoundBufferThread)
        TVPWaveSoundBufferThread = new tTVPWaveSoundBufferThread();
    TVPWaveSoundBufferThread->Start();
}
//---------------------------------------------------------------------------
void TVPAddWaveSoundBuffer(iTVPDecodeSoundBuffer* buffer)
{
    tTJSCriticalSectionHolder holder(TVPWaveSoundBufferVectorCS);
    TVPWaveSoundBufferVector.push_back(buffer);
}
//---------------------------------------------------------------------------
void TVPRemoveWaveSoundBuffer(iTVPDecodeSoundBuffer* buffer)
{
    tTJSCriticalSectionHolder holder(TVPWaveSoundBufferVectorCS);
    std::vector<iTVPDecodeSoundBuffer*>::iterator i;
    i = std::find(TVPWaveSoundBufferVector.begin(), TVPWaveSoundBufferVector.end(), buffer);
    if (i != TVPWaveSoundBufferVector.end())
        TVPWaveSoundBufferVector.erase(i);
}
//---------------------------------------------------------------------------
void TVPReschedulePendingLabelEvent(tjs_int tick)
{
    if (TVPWaveSoundBufferThread)
        TVPWaveSoundBufferThread->ReschedulePendingLabelEvent(tick);
}
//---------------------------------------------------------------------------
void TVPResetVolumeToAllSoundBuffer()
{
    // call each SoundBuffer's SetVolumeToSoundBuffer
    tTJSCriticalSectionHolder holder(TVPWaveSoundBufferVectorCS);
    std::vector<iTVPDecodeSoundBuffer*>::iterator i;
    for (i = TVPWaveSoundBufferVector.begin(); i != TVPWaveSoundBufferVector.end(); i++)
    {
        (*i)->SetVolumeToSoundBuffer();
    }
}
//---------------------------------------------------------------------------
void TVPReleaseDirectSound()
{
    TVPReleaseSoundBuffers(false);
    TVPUninitDirectSound();
}
//---------------------------------------------------------------------------