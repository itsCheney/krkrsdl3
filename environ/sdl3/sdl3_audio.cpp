#include "tjsCommHead.h"
#include "PlatformAudio.h"
#include "PlatformMutex.h"
#include "TVPSystem.h"
#include "TVPDebug.h"
#include <iomanip>
#include <unordered_set>
#include <algorithm>
#include <atomic>

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_init.h>

class tTVPSoundBufferSDL;
static tTJSCriticalSection s_hostAudioBuffersCS;
static std::unordered_set<tTVPSoundBufferSDL*> s_hostAudioBuffers;
static std::atomic<bool> s_hostAudioSuspended{false};

class tTVPSoundBufferSDL : public iTVPSoundBuffer
{
public:
    bool _playing = false;
    float _volume = 1;
    std::atomic<float> _pan{0};

    tjs_uint _bufferLimitCount = 0;
    int totalBufferSize = 0;
    tjs_uint* _bufferSizeCache;
    int _bufferIdx = -1;
    tTJSCriticalSection _buffer_mtx;

    int queued = 0, processed = 0;

    SDL_AudioDeviceID sdl_audio_device = 0;
    SDL_AudioStream* _stream = NULL;
    SDL_AudioSpec spec;
    tjs_int BitsPerSample = 0;
    int _frame_size = 0; // bytes per frame (all channels)
    tjs_uint64 _sentBytes = 0; // bytes put into _stream since the last Reset

    void UpdateQueueData()
    {
        int dataVar = SDL_GetAudioStreamQueued(_stream);
        int newSize = dataVar;
        int idxVar = _bufferIdx;
        if (idxVar < 0)
            return;
        // The resampler holds back a few frames (up to 7) until a flush, so
        // a tail that small counts as played; otherwise the last buffer
        // never ends.
        if (dataVar < _frame_size * 8)
        {
            queued = 0;
        }
        else
        {
            // queue
            queued = 0;
            while (dataVar > 0)
            {
                dataVar -= _bufferSizeCache[idxVar];
                idxVar--;
                if (idxVar < 0)
                    idxVar = _bufferLimitCount - 1;
                queued++;
            }
        }

        // remain
        int tmp = totalBufferSize;
        totalBufferSize = newSize - dataVar;
        dataVar = tmp;
        idxVar = _bufferIdx;
        processed = -queued;
        while (dataVar > 0)
        {
            dataVar -= _bufferSizeCache[idxVar];
            idxVar--;
            if (idxVar < 0)
                idxVar = _bufferLimitCount - 1;
            processed++;
        }
    }

    tTVPSoundBufferSDL(tTVPWaveFormat& fmt, int bufcount)
      : _bufferLimitCount(bufcount),
        _frame_size((fmt.BytesPerSample ? fmt.BytesPerSample : fmt.BitsPerSample / 8) * fmt.Channels)
    {
        {
            tTJSCriticalSectionHolder holder(s_hostAudioBuffersCS);
            s_hostAudioBuffers.insert(this);
        }
        _bufferSizeCache = new tjs_uint[bufcount]();
        memset(&spec, 0, sizeof(spec));
        spec.freq = fmt.SamplesPerSec;
        spec.channels = fmt.Channels;
        BitsPerSample = fmt.BitsPerSample;
        switch (fmt.BitsPerSample)
        {
            case 8:
                spec.format = SDL_AUDIO_S8;
                break;
            case 16:
                spec.format = SDL_AUDIO_S16LE;
                break;
            case 32:
                spec.format = SDL_AUDIO_S32LE;
                break;
            default:
                spec.format = SDL_AUDIO_UNKNOWN;
                break;
        }
    }

    virtual bool Init() override
    {
        if (spec.format == SDL_AUDIO_UNKNOWN)
        {
            SDL_Log("Couldn't create audio stream: Unknow Format!");
            delete this;
            return false;
        }

        sdl_audio_device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL);
        if (!sdl_audio_device)
        {
            SDL_Log("Couldn't open audio device: %s", SDL_GetError());
            delete this;
            return false;
        }

        _stream = SDL_CreateAudioStream(&spec, NULL);
        if (!_stream)
        {
            SDL_Log("Couldn't create audio stream: %s", SDL_GetError());
            delete this;
            return false;
        }
        SDL_BindAudioStream(sdl_audio_device, _stream);
        // Each buffer has its own logical device, so its final mix is this
        // buffer alone and pan applies at mix time.
        if (!SDL_SetAudioPostmixCallback(sdl_audio_device, ApplyPan, this))
            SDL_Log("Couldn't set audio pan callback: %s", SDL_GetError());
        if (s_hostAudioSuspended.load(std::memory_order_acquire))
            SDL_PauseAudioStreamDevice(_stream);
        return true;
    }

    virtual ~tTVPSoundBufferSDL()
    {
        {
            tTJSCriticalSectionHolder holder(s_hostAudioBuffersCS);
            s_hostAudioBuffers.erase(this);
        }
        // Blocks until a running callback returns.
        if (sdl_audio_device)
            SDL_SetAudioPostmixCallback(sdl_audio_device, NULL, NULL);
        Stop();

        if (_stream)
        {
            SDL_UnbindAudioStream(_stream);
            SDL_DestroyAudioStream(_stream);
        }
        if (sdl_audio_device)
            SDL_CloseAudioDevice(sdl_audio_device);
        delete[] _bufferSizeCache;
    }

    virtual void Release() override { delete this; }
    virtual void Play() override
    {
        if (_playing)
            return;
        _playing = true;
        if (!s_hostAudioSuspended.load(std::memory_order_acquire))
            SDL_ResumeAudioStreamDevice(_stream);
    }
    virtual void Pause() override
    {
        if (!_playing)
            return;
        SDL_PauseAudioStreamDevice(_stream);
        _playing = false;
    }

    virtual void Stop() override
    {
        _playing = false;
        totalBufferSize = 0;
        _bufferIdx = -1;
        Reset();
    }
    virtual void Reset() override
    {
        tTJSCSH lk(_buffer_mtx);
        SDL_ClearAudioStream(_stream);
        _sentBytes = 0;
        queued = 0;
        processed = 0;
    }
    virtual bool IsPlaying() override { return _playing; }
    virtual void SetVolume(float v) override
    {
        SDL_SetAudioStreamGain(_stream, v);
        _volume = v;
    }
    virtual float GetVolume() override
    {
        _volume = SDL_GetAudioStreamGain(_stream);
        return _volume;
    }
    virtual void SetPan(float v) override { _pan.store(std::clamp(v, -1.0f, 1.0f)); }
    virtual float GetPan() override { return _pan.load(); }

    // KiriKiri pan (-1 left .. 1 right) attenuates the opposite side
    // linearly, as AetherKrkr's RecalcVolume does. The mix is float in the
    // device's channel layout; left/right pairs are FL/FR and, for 4/6/8
    // channels, the back (and side) pairs too.
    static void SDLCALL ApplyPan(void* userdata, const SDL_AudioSpec* spec, float* buffer,
                                 int buflen)
    {
        const float pan = static_cast<tTVPSoundBufferSDL*>(userdata)->_pan.load();
        const int channels = spec->channels;
        if (pan == 0 || channels < 2)
            return;
        const float left = pan > 0 ? 1.0f - pan : 1.0f;
        const float right = pan < 0 ? 1.0f + pan : 1.0f;
        int pairs[4] = {0, -1, -1, -1};
        if (channels == 4)
            pairs[1] = 2;
        else if (channels == 6)
            pairs[1] = 4;
        else if (channels == 8)
            pairs[1] = 4, pairs[2] = 6;
        const int frames = buflen / (int)sizeof(float) / channels;
        for (int f = 0; f < frames; ++f)
        {
            float* frame = buffer + f * channels;
            for (int pair : pairs)
            {
                if (pair < 0)
                    break;
                frame[pair] *= left;
                frame[pair + 1] *= right;
            }
        }
    }
    virtual void AppendBuffer(const void* _inbuf, unsigned int inlen /*, int tag = 0*/) override
    {
        if (inlen <= 0)
            return;

        tTJSCSH lk(_buffer_mtx);
        UpdateQueueData();
        if (queued >= _bufferLimitCount)
            return;

        ++_bufferIdx;
        if (_bufferIdx >= _bufferLimitCount)
            _bufferIdx = 0;

        SDL_PutAudioStreamData(_stream, _inbuf, inlen);
        _sentBytes += inlen;
        _bufferSizeCache[_bufferIdx] = inlen;
        totalBufferSize += inlen;
    }
    virtual bool IsBufferValid() override
    {
        tTJSCSH lk(_buffer_mtx);
        UpdateQueueData();
        if (processed > 0)
            return true;
        return (processed + queued) < _bufferLimitCount;
        // unlimited buffer size
        // return !_buffers.empty(); // thread safe if read only
    }
    virtual bool IsValidFormat(tTVPWaveFormat& fmt) override
    {
        if (spec.freq != fmt.SamplesPerSec || BitsPerSample != fmt.BitsPerSample ||
            spec.channels != fmt.Channels)
            return false;
        return true;
    }
    virtual tjs_uint GetCurrentPlaySamples() override
    {
        // Samples already played since the last Reset: what was sent minus
        // what is still waiting in the stream.
        tTJSCSH lk(_buffer_mtx);
        int queuedBytes = SDL_GetAudioStreamQueued(_stream);
        if (queuedBytes < 0 || (tjs_uint64)queuedBytes > _sentBytes)
            return 0;
        return (tjs_uint)((_sentBytes - queuedBytes) / _frame_size);
    }
    virtual int GetRemainBuffers() override
    {
        tTJSCSH lk(_buffer_mtx);
        UpdateQueueData();
        return queued;
    }
    virtual tjs_uint GetLatencySamples() override { return 0; }
    virtual float GetLatencySeconds() override { return 0; }

    void SetHostSuspended(bool suspended)
    {
        if (!_stream)
            return;
        if (suspended)
            SDL_PauseAudioStreamDevice(_stream);
        else if (_playing)
            SDL_ResumeAudioStreamDevice(_stream);
    }

    virtual void SetPosition(float x, float y, float z) override
    {
        // not implemented
    }
};

static bool isGetSoundDevice = false;
void TVPInitDirectSound(int freq)
{
    if (!isGetSoundDevice)
    {
        isGetSoundDevice = true;
        if (SDL_InitSubSystem(SDL_INIT_AUDIO))
        {
            int i, num_devices;
            SDL_AudioDeviceID* devices = SDL_GetAudioPlaybackDevices(&num_devices);
            if (devices)
            {
                for (i = 0; i < num_devices; ++i)
                {
                    SDL_AudioDeviceID instance_id = devices[i];
                    SDL_Log("AudioDevice %" SDL_PRIu32 ": %s", instance_id,
                            SDL_GetAudioDeviceName(instance_id));
                }
                SDL_free(devices);
            }
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
        }
    }
}

void TVPUninitDirectSound()
{
}

iTVPSoundBuffer* TVPCreateSoundBuffer(tTVPWaveFormat& fmt, int bufcount)
{
    tTVPSoundBufferSDL* s = new tTVPSoundBufferSDL(fmt, bufcount);
    if (s == nullptr)
        return nullptr;
    if (s->Init())
        return s;
    else
        return nullptr;
}

void TVPSetAudioSuspended(bool suspended)
{
    s_hostAudioSuspended.store(suspended, std::memory_order_release);
    tTJSCriticalSectionHolder holder(s_hostAudioBuffersCS);
    for (auto* buffer : s_hostAudioBuffers)
        buffer->SetHostSuspended(suspended);
}

void TVPResetAudioSessionState()
{
    s_hostAudioSuspended.store(true, std::memory_order_release);
    tTJSCriticalSectionHolder holder(s_hostAudioBuffersCS);
    s_hostAudioBuffers.clear();
}
