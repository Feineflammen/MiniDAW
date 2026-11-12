#include "audio/WasapiDevice.h"
#include "audio/AudioCallback.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

std::string hrText(const char* what, HRESULT hr) {
    char buf[200];
    std::snprintf(buf, sizeof buf, "%s (HRESULT 0x%08lX)", what, static_cast<unsigned long>(hr));
    return buf;
}

bool isFloat32(const WAVEFORMATEX* f) {
    if (f->wBitsPerSample != 32) return false;
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22) {
        // Data1 der SubFormat-GUID ist das klassische Format-Tag.
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
        return ext->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT;
    }
    return false;
}

HRESULT activateClient(IMMDevice* device, ComPtr<IAudioClient>& client) {
    client.Reset();
    return device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                            reinterpret_cast<void**>(client.GetAddressOf()));
}

}  // namespace

struct WasapiDevice::Impl {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    HANDLE event = nullptr;

    std::thread thread;
    std::atomic<bool> running{false};
    std::atomic<bool> failed{false};
    AudioCallback* callback = nullptr;

    double sampleRate = 0.0;
    int channels = 0;
    UINT32 bufferFrames = 0;
    int periodFrames = 0;

    std::vector<std::vector<float>> planar;
    std::vector<float*> planarPtrs;

    void threadMain();
};

WasapiDevice::WasapiDevice() : d(std::make_unique<Impl>()) {}

WasapiDevice::~WasapiDevice() { close(); }

bool WasapiDevice::open(std::string& error) {
    close();

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) { error = hrText("Geraeteliste nicht verfuegbar", hr); return false; }

    ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) { error = hrText("Kein Standard-Wiedergabegeraet", hr); return false; }

    hr = activateClient(device.Get(), d->client);
    if (FAILED(hr)) { error = hrText("Audio-Client nicht aktivierbar", hr); return false; }

    WAVEFORMATEX* mix = nullptr;
    hr = d->client->GetMixFormat(&mix);
    if (FAILED(hr)) { error = hrText("Mix-Format nicht lesbar", hr); close(); return false; }

    if (!isFloat32(mix)) {
        CoTaskMemFree(mix);
        error = "Mix-Format des Geraets ist nicht 32-Bit-Float";
        close();
        return false;
    }

    d->sampleRate = static_cast<double>(mix->nSamplesPerSec);
    d->channels = static_cast<int>(mix->nChannels);

    // Erst die kleinste Periode versuchen (IAudioClient3, ab Windows 10).
    bool initialized = false;
    {
        ComPtr<IAudioClient3> client3;
        if (SUCCEEDED(d->client.As(&client3))) {
            UINT32 defFrames = 0, fundamental = 0, minFrames = 0, maxFrames = 0;
            hr = client3->GetSharedModeEnginePeriod(mix, &defFrames, &fundamental,
                                                    &minFrames, &maxFrames);
            if (SUCCEEDED(hr) && minFrames > 0) {
                hr = client3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                                          minFrames, mix, nullptr);
                if (SUCCEEDED(hr)) {
                    initialized = true;
                    d->periodFrames = static_cast<int>(minFrames);
                }
            }
        }
    }

    // Sonst normaler Shared Mode mit der Standardperiode (meist 10 ms).
    if (!initialized) {
        // Nach einem fehlgeschlagenen Versuch einen frischen Client holen.
        hr = activateClient(device.Get(), d->client);
        if (SUCCEEDED(hr)) {
            hr = d->client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                       AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, mix, nullptr);
        }
        if (FAILED(hr)) {
            CoTaskMemFree(mix);
            error = hrText("Audio-Stream nicht initialisierbar", hr);
            close();
            return false;
        }
        REFERENCE_TIME defPeriod = 0;  // in 100-ns-Einheiten
        if (SUCCEEDED(d->client->GetDevicePeriod(&defPeriod, nullptr))) {
            d->periodFrames = static_cast<int>(
                static_cast<double>(defPeriod) * d->sampleRate / 10'000'000.0 + 0.5);
        }
    }
    CoTaskMemFree(mix);

    hr = d->client->GetBufferSize(&d->bufferFrames);
    if (FAILED(hr)) { error = hrText("Puffergroesse nicht lesbar", hr); close(); return false; }

    d->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!d->event) { error = "Event nicht anlegbar"; close(); return false; }

    hr = d->client->SetEventHandle(d->event);
    if (FAILED(hr)) { error = hrText("Event nicht setzbar", hr); close(); return false; }

    hr = d->client->GetService(IID_PPV_ARGS(&d->render));
    if (FAILED(hr)) { error = hrText("Render-Client nicht verfuegbar", hr); close(); return false; }

    d->planar.assign(static_cast<size_t>(d->channels),
                     std::vector<float>(d->bufferFrames, 0.0f));
    d->planarPtrs.clear();
    for (auto& ch : d->planar) d->planarPtrs.push_back(ch.data());

    return true;
}

bool WasapiDevice::start(AudioCallback& callback, std::string& error) {
    if (!d->client || !d->render) { error = "Geraet ist nicht geoeffnet"; return false; }
    if (d->running.load()) { error = "Geraet laeuft bereits"; return false; }

    callback.prepare(d->sampleRate, d->channels, static_cast<int>(d->bufferFrames));
    d->callback = &callback;
    d->failed.store(false);
    d->running.store(true);
    d->thread = std::thread([this] { d->threadMain(); });
    return true;
}

void WasapiDevice::stop() {
    d->running.store(false);
    if (d->event) SetEvent(d->event);  // Thread aus dem Warten holen
    if (d->thread.joinable()) d->thread.join();
    d->callback = nullptr;
}

void WasapiDevice::close() {
    stop();
    d->render.Reset();
    d->client.Reset();
    if (d->event) { CloseHandle(d->event); d->event = nullptr; }
    d->planar.clear();
    d->planarPtrs.clear();
    d->bufferFrames = 0;
    d->periodFrames = 0;
    d->channels = 0;
    d->sampleRate = 0.0;
}

double WasapiDevice::sampleRate() const { return d->sampleRate; }
int WasapiDevice::numChannels() const { return d->channels; }
int WasapiDevice::bufferFrames() const { return static_cast<int>(d->bufferFrames); }
int WasapiDevice::periodFrames() const { return d->periodFrames; }
bool WasapiDevice::failed() const { return d->failed.load(); }

void WasapiDevice::Impl::threadMain() {
    const HRESULT comInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // Meldet den Thread beim Multimedia-Scheduler an (hohe Prioritaet).
    DWORD taskIndex = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    bool ok = SUCCEEDED(client->Start());

    while (ok && running.load(std::memory_order_relaxed)) {
        const DWORD wait = WaitForSingleObject(event, 2000);
        if (!running.load(std::memory_order_relaxed)) break;
        if (wait != WAIT_OBJECT_0) { ok = false; break; }  // Geraet liefert keine Events mehr

        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) { ok = false; break; }

        const UINT32 frames = bufferFrames - padding;
        if (frames == 0) continue;

        BYTE* data = nullptr;
        if (FAILED(render->GetBuffer(frames, &data))) { ok = false; break; }

        const int ch = static_cast<int>(planarPtrs.size());
        for (int c = 0; c < ch; ++c)
            std::memset(planarPtrs[static_cast<size_t>(c)], 0, frames * sizeof(float));

        callback->process(planarPtrs.data(), ch, static_cast<int>(frames));

        // Kanalpuffer in das verschachtelte Geraeteformat kopieren.
        float* dst = reinterpret_cast<float*>(data);
        for (UINT32 i = 0; i < frames; ++i)
            for (int c = 0; c < ch; ++c)
                *dst++ = planarPtrs[static_cast<size_t>(c)][i];

        if (FAILED(render->ReleaseBuffer(frames, 0))) { ok = false; break; }
    }

    client->Stop();
    if (!ok) failed.store(true);

    if (task) AvRevertMmThreadCharacteristics(task);
    if (SUCCEEDED(comInit)) CoUninitialize();
}
