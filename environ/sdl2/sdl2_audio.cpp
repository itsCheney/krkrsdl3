#include "tjsCommHead.h"
#include "PlatformAudio.h"
#include "PlatformMutex.h"
#include "TVPSystem.h"
#include "TVPDebug.h"
#include <iomanip>
#include <unordered_set>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <vector>

#include <SDL.h>

class tTVPSoundBufferSDL : public iTVPSoundBuffer
{
public:
    bool _playing = false;
    std::atomic<float> _volume{1};
    std::atomic<float> _pan{0};

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

    // The device pulls from this queue in its callback, so volume and pan
    // apply to what is mixed now, not to what was queued seconds ago.
    std::vector<tjs_uint8> _ring;
    size_t _ringHead = 0, _ringSize = 0;
    std::vector<tjs_uint8> _mixIn; // callback scratch
    int _deviceChannels = 0;
    // Gains the callback last mixed with (left, right, other); a change
    // ramps from these across one callback, so fades don't click. Sound
    // started after a Reset takes the current gains at once.
    float _mixedGain[3] = {1, 1, 1};
    bool _gainReset = true;

    void RingPeek(void* out, size_t len) const
    {
        const size_t first = std::min(len, _ring.size() - _ringHead);
        memcpy(out, _ring.data() + _ringHead, first);
        memcpy((tjs_uint8*)out + first, _ring.data(), len - first);
    }
    void RingPush(const void* in, size_t len)
    {
        if (_ringSize + len > _ring.size())
        {
            std::vector<tjs_uint8> grown(std::max(_ring.size() * 2, _ringSize + len));
            if (_ringSize)
                RingPeek(grown.data(), _ringSize);
            _ring.swap(grown);
            _ringHead = 0;
        }
        const size_t tail = (_ringHead + _ringSize) % _ring.size();
        const size_t first = std::min(len, _ring.size() - tail);
        memcpy(_ring.data() + tail, in, first);
        memcpy(_ring.data(), (const tjs_uint8*)in + first, len - first);
        _ringSize += len;
    }
    void RingPop(void* out, size_t len)
    {
        if (!len)
            return;
        RingPeek(out, len);
        _ringHead = (_ringHead + len) % _ring.size();
        _ringSize -= len;
    }

    // Volume and pan (the AetherKrkr RecalcVolume model: the opposite side
    // fades linearly); a mono source is spread to both device channels.
    template <typename T>
    static void Mix(const T* in, T* out, int frames, int channels, int outChannels,
                    const float* from, const float* to)
    {
        for (int f = 0; f < frames; ++f)
        {
            const float t = (float)(f + 1) / frames;
            float gain[3];
            for (int i = 0; i < 3; ++i)
                gain[i] = from[i] + (to[i] - from[i]) * t;
            for (int ch = 0; ch < outChannels; ++ch)
            {
                const double sample = in[channels < 2 ? 0 : ch];
                const double scaled = sample * (ch == 0 ? gain[0] : ch == 1 ? gain[1] : gain[2]);
                out[ch] = (T)std::clamp<double>(std::round(scaled), std::numeric_limits<T>::min(),
                                                std::numeric_limits<T>::max());
            }
            in += channels;
            out += outChannels;
        }
    }
    static void SDLCALL FillAudio(void* userdata, Uint8* stream, int len)
    {
        static_cast<tTVPSoundBufferSDL*>(userdata)->Fill(stream, len);
    }
    void Fill(Uint8* stream, int len)
    {
        const float volume = _volume.load(), pan = _pan.load();
        const float gain[3] = {volume * (pan > 0 ? 1.0f - pan : 1.0f),
                               volume * (pan < 0 ? 1.0f + pan : 1.0f), volume};
        const int sampleBytes = BitsPerSample / 8;
        const int outFrame = sampleBytes * _deviceChannels;
        int frames = 0;
        {
            tTJSCSH lk(_buffer_mtx);
            frames = (int)std::min<size_t>(len / outFrame, _ringSize / _frame_size);
            _mixIn.resize((size_t)frames * _frame_size);
            RingPop(_mixIn.data(), _mixIn.size());
            if (_gainReset)
                std::copy(gain, gain + 3, _mixedGain);
            _gainReset = false;
        }
        if (frames > 0)
        {
            const int channels = spec.channels;
            if (BitsPerSample == 8)
                Mix((const Sint8*)_mixIn.data(), (Sint8*)stream, frames, channels,
                    _deviceChannels, _mixedGain, gain);
            else if (BitsPerSample == 16)
                Mix((const Sint16*)_mixIn.data(), (Sint16*)stream, frames, channels,
                    _deviceChannels, _mixedGain, gain);
            else
                Mix((const Sint32*)_mixIn.data(), (Sint32*)stream, frames, channels,
                    _deviceChannels, _mixedGain, gain);
        }
        // Signed formats: silence is zero.
        memset(stream + frames * outFrame, 0, len - frames * outFrame);
        std::copy(gain, gain + 3, _mixedGain);
    }

    void UpdateQueueData()
    {
        int dataVar = (int)_ringSize;
        int newSize = dataVar;
        int idxVar = _bufferIdx;
        if (idxVar < 0)
            return;
        // A tail under 8 frames counts as played, as in the SDL3 backend
        // whose resampler holds that much back until a flush.
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

        // Always at least stereo, so pan reaches mono sources too.
        SDL_AudioSpec want = spec;
        want.channels = std::max<Uint8>(spec.channels, 2);
        want.samples = 1024;
        want.callback = FillAudio;
        want.userdata = this;
        _deviceChannels = want.channels;
        sdl_audio_device = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
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
        // Waits for a running callback to return.
        if (sdl_audio_device)
            SDL_CloseAudioDevice(sdl_audio_device);
        Stop();
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
        _ringHead = _ringSize = 0;
        _gainReset = true;
        _sentBytes = 0;
        queued = 0;
        processed = 0;
    }
    virtual bool IsPlaying() override { return _playing; }
    virtual void SetVolume(float v) override
    {
        _volume.store(v);
    }
    virtual float GetVolume() override
    {
        return _volume.load();
    }
    virtual void SetPan(float v) override { _pan.store(std::clamp(v, -1.0f, 1.0f)); }
    virtual float GetPan() override { return _pan.load(); }
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

        RingPush(_inbuf, inlen);
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
        tjs_uint64 waiting = _ringSize;
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
