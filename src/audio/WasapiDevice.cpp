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
#include <propsys.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
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

// PKEY_Device_FriendlyName, hier selbst angelegt statt aus functiondiscoverykeys_devpkey.h.
const PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

std::wstring deviceId(IMMDevice* device) {
    std::wstring id;
    LPWSTR raw = nullptr;
    if (SUCCEEDED(device->GetId(&raw)) && raw) id = raw;
    if (raw) CoTaskMemFree(raw);
    return id;
}

std::wstring deviceName(IMMDevice* device) {
    std::wstring name;
    ComPtr<IPropertyStore> store;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store))) {
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(store->GetValue(kFriendlyName, &value)) && value.vt == VT_LPWSTR && value.pwszVal)
            name = value.pwszVal;
        PropVariantClear(&value);
    }
    return name.empty() ? std::wstring(L"Unbenanntes Ger\u00E4t") : name;
}

// Holt das Geraet mit dieser Kennung oder, bei leerer Kennung, das Standardgeraet.
HRESULT findDevice(IMMDeviceEnumerator* enumerator, EDataFlow flow, const std::wstring& id, ComPtr<IMMDevice>& device) {
    device.Reset();
    if (id.empty()) return enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device);
    return enumerator->GetDevice(id.c_str(), &device);
}

HRESULT activateClient(IMMDevice* device, ComPtr<IAudioClient>& client) {
    client.Reset();
    return device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                            reinterpret_cast<void**>(client.GetAddressOf()));
}

int toFrames(REFERENCE_TIME time, double sampleRate) {  // time in 100-ns-Einheiten
    return static_cast<int>(static_cast<double>(time) * sampleRate / 10'000'000.0 + 0.5);
}

constexpr std::size_t kInputRingFrames = std::size_t{1} << 15;
constexpr std::size_t kInputRingMask = kInputRingFrames - 1;

}  // namespace

struct WasapiDevice::Impl {
    // Ausgabe
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
    int outputLatency = 0;

    std::vector<std::vector<float>> planar;
    std::vector<float*> planarPtrs;

    // Eingang
    ComPtr<IAudioClient> inClient;
    ComPtr<IAudioCaptureClient> capture;
    HANDLE inEvent = nullptr;
    std::thread inThread;
    int inChannels = 0;
    int inPeriodFrames = 0;
    int inStreamLatency = 0;
    std::string inError;

    // Ringpuffer Aufnahme-Thread -> Wiedergabe-Thread, verschachtelt
    std::vector<float> ring;
    std::atomic<std::size_t> ringWrite{0};
    std::atomic<std::size_t> ringRead{0};
    bool primed = false;       // nur Wiedergabe-Thread
    float fillAverage = 0.0f;  // nur Wiedergabe-Thread
    std::atomic<int> ringLatency{0};
    std::atomic<unsigned> inGlitches{0};

    // Zeitstempel des zuletzt eingetroffenen Aufnahmepakets: Ringpuffer-Index
    // seines ersten Frames und der Zeitpunkt, zu dem das Geraet diesen Frame
    // aufgenommen hat (100-ns-Einheiten). Geschuetzt durch einen Zaehler
    // (ungerade = wird gerade geschrieben), damit der Leser nie wartet.
    std::atomic<std::uint64_t> stampSeq{0};
    std::atomic<std::uint64_t> stampIndex{0};
    std::atomic<std::int64_t> stampTime{0};
    std::atomic<bool> stampsUsed{false};  // der Eingang ist ueber Zeitstempel ausgerichtet
    int targetAgeFrames = 0;              // so alt ist ein Eingangs-Sample, wenn es gelesen wird
    int expectedPadding = 0;              // ueblicher Fuellstand der Ausgabe beim Rueckruf
    std::int64_t qpcFrequency = 1;

    std::vector<std::vector<float>> inPlanar;
    std::vector<const float*> inPtrs;

    void openInput(IMMDeviceEnumerator* enumerator, const std::wstring& id);

    std::wstring outId, outName, inId, inName;
    void closeInput();
    void renderThread();
    void captureThread();
    void pullInput(UINT32 frames, UINT32 padding) noexcept;
    bool alignFromTimestamp(std::size_t n, UINT32 padding, std::size_t& r) noexcept;
    std::int64_t now100ns() const noexcept;
};

WasapiDevice::WasapiDevice() : d(std::make_unique<Impl>()) {}

WasapiDevice::~WasapiDevice() { close(); }

std::vector<AudioEndpoint> WasapiDevice::endpoints(bool capture) {
    std::vector<AudioEndpoint> list;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
        return list;
    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(capture ? eCapture : eRender, DEVICE_STATE_ACTIVE, &collection)))
        return list;
    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (SUCCEEDED(collection->Item(i, &device))) list.push_back({deviceId(device.Get()), deviceName(device.Get())});
    }
    return list;
}

const std::wstring& WasapiDevice::outputId() const { return d->outId; }
const std::wstring& WasapiDevice::outputName() const { return d->outName; }
const std::wstring& WasapiDevice::inputId() const { return d->inId; }
const std::wstring& WasapiDevice::inputName() const { return d->inName; }

bool WasapiDevice::open(const std::wstring& outputId, const std::wstring& inputId, bool noInput,
                        std::string& error) {
    close();

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) { error = hrText("Geraeteliste nicht verfuegbar", hr); return false; }

    ComPtr<IMMDevice> device;
    hr = findDevice(enumerator.Get(), eRender, outputId, device);
    if (FAILED(hr)) {
        error = outputId.empty() ? hrText("Kein Standard-Wiedergabegeraet", hr)
                                 : hrText("Wiedergabegeraet nicht gefunden (abgesteckt?)", hr);
        return false;
    }
    d->outId = deviceId(device.Get());
    d->outName = deviceName(device.Get());

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
        REFERENCE_TIME defPeriod = 0;
        if (SUCCEEDED(d->client->GetDevicePeriod(&defPeriod, nullptr)))
            d->periodFrames = toFrames(defPeriod, d->sampleRate);
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

    // Ausgangslatenz: was das Geraet meldet, plus eine Periode im Puffer.
    REFERENCE_TIME streamLatency = 0;
    d->client->GetStreamLatency(&streamLatency);
    d->outputLatency = toFrames(streamLatency, d->sampleRate) + d->periodFrames;

    d->planar.assign(static_cast<size_t>(d->channels),
                     std::vector<float>(d->bufferFrames, 0.0f));
    d->planarPtrs.clear();
    for (auto& ch : d->planar) d->planarPtrs.push_back(ch.data());

    // Der Eingang ist optional: Ohne ihn laeuft alles ausser der Aufnahme.
    if (noInput) {
        d->inError = "Kein Eingang gewaehlt";
    } else {
        d->openInput(enumerator.Get(), inputId);
    }
    return true;
}

void WasapiDevice::Impl::openInput(IMMDeviceEnumerator* enumerator, const std::wstring& id) {
    inError.clear();

    ComPtr<IMMDevice> device;
    HRESULT hr = findDevice(enumerator, eCapture, id, device);
    if (FAILED(hr)) { inError = id.empty() ? "Kein Aufnahmegeraet gefunden" : "Aufnahmegeraet nicht gefunden (abgesteckt?)"; return; }
    inId = deviceId(device.Get());
    inName = deviceName(device.Get());

    hr = activateClient(device.Get(), inClient);
    if (FAILED(hr)) { inError = hrText("Aufnahmegeraet nicht aktivierbar", hr); closeInput(); return; }

    WAVEFORMATEX* mix = nullptr;
    hr = inClient->GetMixFormat(&mix);
    if (FAILED(hr)) { inError = hrText("Format des Aufnahmegeraets nicht lesbar", hr); closeInput(); return; }
    const int deviceChannels = static_cast<int>(mix->nChannels);
    CoTaskMemFree(mix);

    // Gewuenschtes Format: Float, Samplerate der Ausgabe, hoechstens Stereo.
    // Windows wandelt vom Geraeteformat dorthin um.
    inChannels = std::clamp(deviceChannels, 1, 2);
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = static_cast<WORD>(inChannels);
    format.nSamplesPerSec = static_cast<DWORD>(sampleRate + 0.5);
    format.wBitsPerSample = 32;
    format.nBlockAlign = static_cast<WORD>(inChannels * 4);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    format.cbSize = 0;

    hr = inClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                              AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                  AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                              0, 0, &format, nullptr);
    if (FAILED(hr)) {
        inError = hr == E_ACCESSDENIED
                      ? "Windows verweigert den Mikrofonzugriff (Datenschutz-Einstellungen pruefen)"
                      : hrText("Aufnahme-Stream nicht initialisierbar", hr);
        closeInput();
        return;
    }

    inEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!inEvent || FAILED(inClient->SetEventHandle(inEvent)) ||
        FAILED(inClient->GetService(IID_PPV_ARGS(&capture)))) {
        inError = "Aufnahme-Stream nicht einrichtbar";
        closeInput();
        return;
    }

    REFERENCE_TIME period = 0, latency = 0;
    inClient->GetDevicePeriod(&period, nullptr);
    inClient->GetStreamLatency(&latency);
    inPeriodFrames = std::max(1, toFrames(period, sampleRate));
    inStreamLatency = toFrames(latency, sampleRate);

    ring.assign(kInputRingFrames * static_cast<std::size_t>(inChannels), 0.0f);
    ringWrite.store(0);
    ringRead.store(0);
    primed = false;
    fillAverage = 0.0f;
    ringLatency.store(2 * inPeriodFrames);

    // Ein Eingangs-Sample wird gelesen, wenn es eine Ausgabeperiode plus zwei
    // Aufnahmeperioden alt ist. Das reicht, damit bei jedem Rueckruf genug
    // Daten da sind, und haengt nicht davon ab, wann die Pakete eintreffen.
    targetAgeFrames = periodFrames + 2 * inPeriodFrames;
    expectedPadding = static_cast<int>(bufferFrames) - periodFrames;
    stampSeq.store(0);
    stampsUsed.store(false);
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    qpcFrequency = frequency.QuadPart > 0 ? frequency.QuadPart : 1;

    inPlanar.assign(static_cast<std::size_t>(inChannels), std::vector<float>(bufferFrames, 0.0f));
    inPtrs.clear();
    for (auto& ch : inPlanar) inPtrs.push_back(ch.data());
}

void WasapiDevice::Impl::closeInput() {
    capture.Reset();
    inClient.Reset();
    if (inEvent) { CloseHandle(inEvent); inEvent = nullptr; }
    inChannels = 0;
    inPlanar.clear();
    inPtrs.clear();
    inId.clear();
    inName.clear();
}

bool WasapiDevice::start(AudioCallback& callback, std::string& error) {
    if (!d->client || !d->render) { error = "Geraet ist nicht geoeffnet"; return false; }
    if (d->running.load()) { error = "Geraet laeuft bereits"; return false; }

    callback.prepare(d->sampleRate, d->inChannels, d->channels, static_cast<int>(d->bufferFrames));
    d->callback = &callback;
    d->failed.store(false);
    d->running.store(true);
    if (d->inChannels > 0) d->inThread = std::thread([this] { d->captureThread(); });
    d->thread = std::thread([this] { d->renderThread(); });
    return true;
}

void WasapiDevice::stop() {
    d->running.store(false);
    if (d->event) SetEvent(d->event);  // Threads aus dem Warten holen
    if (d->inEvent) SetEvent(d->inEvent);
    if (d->thread.joinable()) d->thread.join();
    if (d->inThread.joinable()) d->inThread.join();
    d->callback = nullptr;
}

void WasapiDevice::close() {
    stop();
    d->closeInput();
    d->render.Reset();
    d->client.Reset();
    if (d->event) { CloseHandle(d->event); d->event = nullptr; }
    d->planar.clear();
    d->planarPtrs.clear();
    d->bufferFrames = 0;
    d->periodFrames = 0;
    d->channels = 0;
    d->sampleRate = 0.0;
    d->outId.clear();
    d->outName.clear();
}

double WasapiDevice::sampleRate() const { return d->sampleRate; }
int WasapiDevice::numChannels() const { return d->channels; }
int WasapiDevice::bufferFrames() const { return static_cast<int>(d->bufferFrames); }
int WasapiDevice::periodFrames() const { return d->periodFrames; }
bool WasapiDevice::failed() const { return d->failed.load(); }
int WasapiDevice::inputChannels() const { return d->inChannels; }
const std::string& WasapiDevice::inputError() const { return d->inError; }
int WasapiDevice::outputLatencyFrames() const { return d->outputLatency; }
unsigned WasapiDevice::inputGlitches() const { return d->inGlitches.load(std::memory_order_relaxed); }

int WasapiDevice::inputLatencyFrames() const {
    if (d->inChannels == 0) return 0;
    // Ueber Zeitstempel ausgerichtet: immer gleich, unabhaengig vom Programmstart.
    if (d->stampsUsed.load(std::memory_order_relaxed)) return d->targetAgeFrames;
    // Geraet + eine Aufnahmeperiode + Verweildauer im Ringpuffer.
    return d->inStreamLatency + d->inPeriodFrames + d->ringLatency.load(std::memory_order_relaxed);
}

std::int64_t WasapiDevice::Impl::now100ns() const noexcept {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    const std::int64_t c = counter.QuadPart;
    return (c / qpcFrequency) * 10'000'000 + (c % qpcFrequency) * 10'000'000 / qpcFrequency;
}

// Legt den Lesezeiger so, dass das naechste gelesene Sample genau
// targetAgeFrames alt ist, gemessen an den Zeitstempeln des Geraets. So ist
// der Abstand zwischen Ein- und Ausgang bei jedem Start gleich und eine
// gemessene Latenz bleibt gueltig. Liefert false, solange das nicht geht.
bool WasapiDevice::Impl::alignFromTimestamp(std::size_t n, UINT32 padding, std::size_t& r) noexcept {
    std::uint64_t index = 0;
    std::int64_t time = 0;
    bool consistent = false;
    for (int attempt = 0; attempt < 4 && !consistent; ++attempt) {
        const std::uint64_t before = stampSeq.load(std::memory_order_acquire);
        if (before == 0 || (before & 1)) continue;
        index = stampIndex.load(std::memory_order_relaxed);
        time = stampTime.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        consistent = stampSeq.load(std::memory_order_relaxed) == before;
    }
    if (!consistent) return false;

    // Index des Samples, das das Geraet gerade jetzt aufnimmt ...
    const double recordedSince = static_cast<double>(now100ns() - time) * sampleRate / 10'000'000.0;
    // ... minus das Zielalter. Kommt der Rueckruf frueher als sonst (mehr in der
    // Ausgabe vorgemerkt), wird das Gelesene entsprechend spaeter hoerbar.
    const double age = targetAgeFrames - (static_cast<double>(padding) - expectedPadding);
    const auto want = static_cast<std::int64_t>(std::llround(static_cast<double>(index) + recordedSince - age));

    const std::size_t w = ringWrite.load(std::memory_order_acquire);
    if (want < 0 || static_cast<std::size_t>(want) < r) return false;  // noch nicht weit genug
    if (static_cast<std::size_t>(want) + n > w) return false;          // Daten noch nicht da
    r = static_cast<std::size_t>(want);
    stampsUsed.store(true, std::memory_order_relaxed);
    return true;
}

// Holt frames Eingangs-Frames aus dem Ringpuffer nach inPlanar (Wiedergabe-Thread).
void WasapiDevice::Impl::pullInput(UINT32 frames, UINT32 padding) noexcept {
    const std::size_t n = frames;
    const std::size_t target = static_cast<std::size_t>(2 * inPeriodFrames);  // Polster gegen Schwankungen
    const auto ch = static_cast<std::size_t>(inChannels);

    std::size_t r = ringRead.load(std::memory_order_relaxed);
    std::size_t avail = ringWrite.load(std::memory_order_acquire) - r;

    // Zu viel angesammelt (die Uhren der Geraete laufen auseinander): aufholen.
    if (avail > target + n + static_cast<std::size_t>(4 * inPeriodFrames)) {
        r += avail - (target + n);
        avail = target + n;
        inGlitches.fetch_add(1, std::memory_order_relaxed);
    }

    if (!primed) {
        if (stampSeq.load(std::memory_order_acquire) != 0) {
            // Das Geraet liefert Zeitstempel: danach ausrichten.
            if (alignFromTimestamp(n, padding, r)) {
                primed = true;
                avail = ringWrite.load(std::memory_order_acquire) - r;
            }
        } else if (avail >= target + n) {
            primed = true;  // ohne Zeitstempel: nach Fuellstand
        }
    }

    if (primed && avail >= n) {
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t idx = ((r + i) & kInputRingMask) * ch;
            for (std::size_t c = 0; c < ch; ++c) inPlanar[c][i] = ring[idx + c];
        }
        r += n;
        fillAverage += 0.02f * (static_cast<float>(avail - n) - fillAverage);
        ringLatency.store(static_cast<int>(fillAverage), std::memory_order_relaxed);
    } else {
        // Noch nicht genug Vorrat (Start) oder leergelaufen: Stille, dann neu ansammeln.
        for (std::size_t c = 0; c < ch; ++c) std::memset(inPlanar[c].data(), 0, n * sizeof(float));
        if (primed) {
            primed = false;
            inGlitches.fetch_add(1, std::memory_order_relaxed);
        }
    }
    ringRead.store(r, std::memory_order_release);
}

void WasapiDevice::Impl::renderThread() {
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

        if (inChannels > 0) {
            pullInput(frames, padding);
            callback->process(inPtrs.data(), inChannels, planarPtrs.data(), ch, static_cast<int>(frames));
        } else {
            callback->process(nullptr, 0, planarPtrs.data(), ch, static_cast<int>(frames));
        }

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

// Liest Pakete vom Aufnahmegeraet und legt sie in den Ringpuffer.
void WasapiDevice::Impl::captureThread() {
    const HRESULT comInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD taskIndex = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    const auto ch = static_cast<std::size_t>(inChannels);
    bool ok = SUCCEEDED(inClient->Start());

    while (ok && running.load(std::memory_order_relaxed)) {
        WaitForSingleObject(inEvent, 2000);
        if (!running.load(std::memory_order_relaxed)) break;

        for (;;) {
            UINT32 packet = 0;
            if (FAILED(capture->GetNextPacketSize(&packet))) { ok = false; break; }
            if (packet == 0) break;

            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 devicePosition = 0, qpcPosition = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, &devicePosition, &qpcPosition))) { ok = false; break; }

            const std::size_t w = ringWrite.load(std::memory_order_relaxed);
            const std::size_t space = kInputRingFrames - (w - ringRead.load(std::memory_order_acquire));
            if (frames <= space) {
                const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr;
                const float* src = reinterpret_cast<const float*>(data);
                for (std::size_t i = 0; i < frames; ++i) {
                    const std::size_t idx = ((w + i) & kInputRingMask) * ch;
                    for (std::size_t c = 0; c < ch; ++c) ring[idx + c] = silent ? 0.0f : src[i * ch + c];
                }
                ringWrite.store(w + frames, std::memory_order_release);

                // Zeitstempel dieses Pakets fuer die Ausrichtung im Wiedergabe-Thread.
                if ((flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) == 0 && qpcPosition != 0) {
                    const std::uint64_t seq = stampSeq.load(std::memory_order_relaxed);
                    stampSeq.store(seq + 1, std::memory_order_relaxed);
                    std::atomic_thread_fence(std::memory_order_release);
                    stampIndex.store(w, std::memory_order_relaxed);
                    stampTime.store(static_cast<std::int64_t>(qpcPosition), std::memory_order_relaxed);
                    stampSeq.store(seq + 2, std::memory_order_release);
                }
            }  // sonst: Ringpuffer voll (die Wiedergabe steht), Paket verwerfen

            capture->ReleaseBuffer(frames);
        }
    }

    inClient->Stop();
    if (task) AvRevertMmThreadCharacteristics(task);
    if (SUCCEEDED(comInit)) CoUninitialize();
}
