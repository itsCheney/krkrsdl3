#include "tjsCommHead.h"
#include "PlatformAudio.h"
#include "PlatformMutex.h"
#include "TVPSystem.h"
#include "TVPDebug.h"
#include <iomanip>
#include <unordered_set>
#include <algorithm>
#include <cmath>
#include <vector>

#include <SDL.h>

class tTVPSoundBufferSDL : public iTVPSoundBuffer
{
public:
    bool _playing = false;
    float _volume = 1;
    float _pan = 0;

    tjs_uint _bufferLimitCount = 0;
    int totalBufferSize = 0;
    tjs_uint* _bufferSizeCache;
    int _bufferIdx = -1;
    tTJSCriticalSection _buffer_mtx;

    int queued = 0, processed = 0;

    SDL_AudioDeviceID sdl_audio_device = 0;
    SDL_AudioSpec spec;
    tjs_int BitsPerSample = 0;
    int _frame_size = 0; // bytes per frame (all channels)
    tjs_uint64 _sentBytes = 0; // bytes queued since the last Reset
    std::vector<tjs_uint8> _scaled;

    // SDL2 queues have no gain, so volume and pan (the AetherKrkr
    // RecalcVolume model: the opposite side fades linearly) scale the
    // samples as they are queued.
    template <typename T>
    static void ScaleSamples(T* data, size_t samples, int channels, float left, float right,
                             float other)
    {
        for (size_t i = 0; i < samples; ++i)
        {
            const int ch = channels < 2 ? 2 : (int)(i % channels);
            const float gain = ch == 0 ? left : ch == 1 ? right : other;
            data[i] = (T)std::lround(data[i] * (double)gain);
        }
    }
    const void* ApplyGain(const void* in, unsigned int len)
    {
        const float left = _volume * (_pan > 0 ? 1.0f - _pan : 1.0f);
        const float right = _volume * (_pan < 0 ? 1.0f + _pan : 1.0f);
        if (left == 1.0f && right == 1.0f && _volume == 1.0f)
            return in;
        _scaled.assign((const tjs_uint8*)in, (const tjs_uint8*)in + len);
        const int channels = spec.channels;
        if (BitsPerSample == 8)
            ScaleSamples((Sint8*)_scaled.data(), len, channels, left, right, _volume);
        else if (BitsPerSample == 16)
            ScaleSamples((Sint16*)_scaled.data(), len / 2, channels, left, right, _volume);
        else
            ScaleSamples((Sint32*)_scaled.data(), len / 4, channels, left, right, _volume);
        return _scaled.data();
    }

    void UpdateQueueData()
    {
        int dataVar = SDL_GetQueuedAudioSize(sdl_audio_device);
        int newSize = dataVar;
        int idxVar = _bufferIdx;
        if (idxVar < 0)
            return;
        // A resampled stream holds back a few frames (up to 7) until a
        // flush, so a tail that small counts as played.
        if (dataVar < _frame_size * 8)
        {
            queued = 0;
        }
        else
        {
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
        _bufferSizeCache = new tjs_uint[bufcount]();
        memset(&spec, 0, sizeof(spec));
        spec.freq = fmt.SamplesPerSec;
        spec.channels = fmt.Channels;
        BitsPerSample = fmt.BitsPerSample;
        switch (fmt.BitsPerSample)
        {
            case 8:
                spec.format = AUDIO_S8;
                break;
            case 16:
                spec.format = AUDIO_S16SYS;
                break;
            case 32:
                spec.format = AUDIO_S32SYS;
                break;
            default:
                spec.format = 0;
                break;
        }
        spec.samples = 4096;
        spec.callback = NULL;
    }

    virtual bool Init() override
    {
        if (spec.format == 0)
        {
            SDL_Log("Couldn't create audio stream: Unknow Format!");
            delete this;
            return false;
        }

        sdl_audio_device = SDL_OpenAudioDevice(NULL, 0, &spec, NULL, 0);
        if (sdl_audio_device == 0)
        {
            SDL_Log("Couldn't open audio device: %s", SDL_GetError());
            delete this;
            return false;
        }
        return true;
    }

    virtual ~tTVPSoundBufferSDL()
    {
        Stop();

        if (sdl_audio_device)
            SDL_CloseAudioDevice(sdl_audio_device);
        delete[] _bufferSizeCache;
    }

    virtual void Release() override { delete this; }
    virtual void Play() override
    {
        if (_playing)
            return;
        SDL_PauseAudioDevice(sdl_audio_device, 0);
        _playing = true;
    }
    virtual void Pause() override
    {
        if (!_playing)
            return;
        SDL_PauseAudioDevice(sdl_audio_device, 1);
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
        SDL_ClearQueuedAudio(sdl_audio_device);
        _sentBytes = 0;
        queued = 0;
        processed = 0;
    }
    virtual bool IsPlaying() override { return _playing; }
    virtual void SetVolume(float v) override
    {
        _volume = v;
    }
    virtual float GetVolume() override
    {
        return _volume;
    }
    virtual void SetPan(float v) override { _pan = std::clamp(v, -1.0f, 1.0f); }
    virtual float GetPan() override { return _pan; }
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

        SDL_QueueAudio(sdl_audio_device, ApplyGain(_inbuf, inlen), inlen);
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
        // Played frames: queued in total minus still waiting.
        tTJSCSH lk(_buffer_mtx);
        tjs_uint64 waiting = SDL_GetQueuedAudioSize(sdl_audio_device);
        if (waiting > _sentBytes)
            return 0;
        return (tjs_uint)((_sentBytes - waiting) / _frame_size);
    }
    virtual int GetRemainBuffers() override
    {
        tTJSCSH lk(_buffer_mtx);
        UpdateQueueData();
        return queued;
    }
    virtual tjs_uint GetLatencySamples() override { return 0; }
    virtual float GetLatencySeconds() override { return 0; }

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
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0)
        {
            int i, num_devices;
            num_devices = SDL_GetNumAudioDevices(0);
            for (i = 0; i < num_devices; ++i)
            {
                const char* name = SDL_GetAudioDeviceName(i, 0);
                SDL_Log("AudioDevice %d: %s", i, name ? name : "Unknown");
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
