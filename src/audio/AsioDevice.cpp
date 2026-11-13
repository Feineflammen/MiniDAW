#include "audio/AsioDevice.h"

#include "audio/AsioFormat.h"
#include "audio/AudioCallback.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <unknwn.h>

#include <algorithm>
#include <atomic>
#include <cstring>

// Schnittstelle der Treiber aus dem ASIO SDK von Steinberg (GPLv3,
// third_party/asiosdk). Die Header sind alt und nicht warnungsfrei.
#pragma warning(push, 0)
#include "asiosys.h"
#include "asio.h"
#include "iasiodrv.h"
#pragma warning(pop)

static_assert(sizeof(void*) == 8, "MiniDAW unterstuetzt ASIO nur unter 64-Bit-Windows");

// Die Formatwandlung (audio/AsioFormat.h) kommt ohne SDK aus und wird so auch
// unter Linux getestet; ihre Nummern muessen zu denen des SDK passen.
constexpr bool sameFormat(long ours, long sdk) { return ours == sdk; }
static_assert(sameFormat(asio_format::kInt16, ASIOSTInt16LSB) && sameFormat(asio_format::kInt24, ASIOSTInt24LSB) &&
              sameFormat(asio_format::kInt32, ASIOSTInt32LSB) && sameFormat(asio_format::kFloat32, ASIOSTFloat32LSB) &&
              sameFormat(asio_format::kFloat64, ASIOSTFloat64LSB) &&
              sameFormat(asio_format::kInt32in16, ASIOSTInt32LSB16) &&
              sameFormat(asio_format::kInt32in18, ASIOSTInt32LSB18) &&
              sameFormat(asio_format::kInt32in20, ASIOSTInt32LSB20) &&
              sameFormat(asio_format::kInt32in24, ASIOSTInt32LSB24));

namespace {

bool succeeded(ASIOError e) { return e == ASE_OK || e == ASE_SUCCESS; }

constexpr int kMaxInputs = 64;   // so viele Eingaenge wertet die Engine aus
constexpr int kMaxOutputs = 2;   // MiniDAW gibt Stereo aus

// Nur ASCII, damit die Meldung in jeder Kodierung lesbar bleibt.
std::string narrow(const std::wstring& text) {
    std::string out;
    for (const wchar_t c : text) out += (c < 128) ? static_cast<char>(c) : '?';
    return out;
}

std::wstring widen(const char* text) {
    wchar_t buffer[256] = {};
    MultiByteToWideChar(CP_ACP, 0, text, -1, buffer, 255);
    return buffer;
}

// Liest einen Textwert unter HKLM\SOFTWARE\ASIO\<driver>.
std::wstring registryText(HKEY root, const std::wstring& driver, const wchar_t* value) {
    wchar_t buffer[512] = {};
    DWORD size = sizeof(buffer);
    if (RegGetValueW(root, driver.c_str(), value, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) return {};
    return buffer;
}

}  // namespace

struct AsioDevice::Impl {
    IASIO* driver = nullptr;
    std::wstring name;
    std::wstring emptyName;
    double rate = 0.0;
    int inputs = 0;
    int outputs = 0;
    int bufferSize = 0;
    long inLatency = 0;
    long outLatency = 0;
    bool buffersCreated = false;
    bool running = false;
    bool useOutputReady = false;
    std::string inError;

    std::vector<ASIOBufferInfo> buffers;  // erst die Eingaenge, dann die Ausgaenge
    std::vector<long> types;
    std::vector<float> scratch;           // float-Puffer fuer alle Kanaele
    std::vector<const float*> inPointers;
    std::vector<float*> outPointers;
    AudioCallback* callback = nullptr;

    std::atomic<bool> processing{false};
    std::atomic<bool> reset{false};
    std::atomic<bool> latenciesChanged{false};
    std::atomic<unsigned> overloads{0};

    void process(long index) noexcept;
    void refreshLatencies();
    std::string driverError(const char* what);
};

namespace {

std::atomic<AsioDevice::Impl*> g_active{nullptr};

void onBufferSwitch(long index, ASIOBool /*direct*/) {
    if (AsioDevice::Impl* self = g_active.load(std::memory_order_acquire)) self->process(index);
}

ASIOTime* onBufferSwitchTimeInfo(ASIOTime* params, long index, ASIOBool direct) {
    onBufferSwitch(index, direct);
    return params;
}

void onSampleRateChange(ASIOSampleRate /*rate*/) {
    if (AsioDevice::Impl* self = g_active.load(std::memory_order_acquire)) self->reset.store(true);
}

long onMessage(long selector, long value, void* /*message*/, double* /*opt*/) {
    AsioDevice::Impl* self = g_active.load(std::memory_order_acquire);
    switch (selector) {
        case kAsioSelectorSupported:
            return (value == kAsioEngineVersion || value == kAsioResetRequest || value == kAsioBufferSizeChange ||
                    value == kAsioResyncRequest || value == kAsioLatenciesChanged || value == kAsioSupportsTimeInfo ||
                    value == kAsioOverload) ? 1 : 0;
        case kAsioEngineVersion:
            return 2;
        case kAsioResetRequest:
        case kAsioBufferSizeChange:
            // Neu oeffnen erledigt die UI (resetRequested), nicht der Treiber-Thread.
            if (self) self->reset.store(true);
            return 1;
        case kAsioResyncRequest:
            return 1;
        case kAsioLatenciesChanged:
            if (self) self->latenciesChanged.store(true);
            return 1;
        case kAsioSupportsTimeInfo:
            return 1;
        case kAsioSupportsTimeCode:
            return 0;
        case kAsioOverload:
            if (self) self->overloads.fetch_add(1, std::memory_order_relaxed);
            return 0;
        default:
            return 0;
    }
}

ASIOCallbacks g_callbacks{onBufferSwitch, onSampleRateChange, onMessage, onBufferSwitchTimeInfo};

}  // namespace

// --- Treiber-Thread ----------------------------------------------------------

void AsioDevice::Impl::process(long index) noexcept {
    const auto n = static_cast<std::size_t>(bufferSize);
    const int total = inputs + outputs;
    if (!processing.load(std::memory_order_acquire) || callback == nullptr || index < 0 || index > 1) {
        for (int c = inputs; c < total; ++c) {
            const ASIOBufferInfo& info = buffers[static_cast<std::size_t>(c)];
            std::memset(info.buffers[index & 1], 0,
                        n * static_cast<std::size_t>(asio_format::bytesPerSample(types[static_cast<std::size_t>(c)])));
        }
        return;
    }
    for (int c = 0; c < inputs; ++c) {
        const auto i = static_cast<std::size_t>(c);
        asio_format::toFloat(types[i], buffers[i].buffers[index], scratch.data() + i * n, bufferSize);
    }
    float* const outStart = scratch.data() + static_cast<std::size_t>(inputs) * n;
    std::fill(outStart, outStart + static_cast<std::size_t>(outputs) * n, 0.0f);

    callback->process(inPointers.data(), inputs, outPointers.data(), outputs, bufferSize);

    for (int c = 0; c < outputs; ++c) {
        const auto i = static_cast<std::size_t>(inputs + c);
        asio_format::fromFloat(types[i], outPointers[static_cast<std::size_t>(c)], buffers[i].buffers[index], bufferSize);
    }
    if (useOutputReady) driver->outputReady();
}

// --- UI-Thread ---------------------------------------------------------------

std::string AsioDevice::Impl::driverError(const char* what) {
    char text[256] = {};
    if (driver) driver->getErrorMessage(text);
    std::string message = what;
    if (text[0] != '\0') message += ": " + narrow(widen(text));
    return message;
}

void AsioDevice::Impl::refreshLatencies() {
    if (!driver || !latenciesChanged.exchange(false)) return;
    long in = 0, out = 0;
    if (succeeded(driver->getLatencies(&in, &out))) {
        inLatency = in;
        outLatency = out;
    }
}

AsioDevice::AsioDevice() : d(std::make_unique<Impl>()) {}

AsioDevice::~AsioDevice() { close(); }

std::vector<std::wstring> AsioDevice::drivers() {
    std::vector<std::wstring> names;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &key) != ERROR_SUCCESS) return names;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256] = {};
        DWORD length = 255;
        if (RegEnumKeyExW(key, i, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        if (!registryText(key, name, L"CLSID").empty()) names.emplace_back(name);
    }
    RegCloseKey(key);
    std::sort(names.begin(), names.end());
    return names;
}

bool AsioDevice::open(const std::wstring& driverName, void* window, std::string& error) {
    close();
    if (g_active.load() != nullptr) {
        error = "Es ist schon ein ASIO-Treiber offen.";
        return false;
    }

    // Klassenkennung des Treibers aus der Registry.
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &key) != ERROR_SUCCESS) {
        error = "Keine ASIO-Treiber installiert.";
        return false;
    }
    const std::wstring clsidText = registryText(key, driverName, L"CLSID");
    RegCloseKey(key);
    CLSID clsid{};
    if (clsidText.empty() || FAILED(CLSIDFromString(clsidText.c_str(), &clsid))) {
        error = "ASIO-Treiber nicht gefunden: " + narrow(driverName);
        return false;
    }

    // ASIO-Eigenheit: Die Schnittstellenkennung ist die Klassenkennung selbst.
    void* object = nullptr;
    const HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, &object);
    if (FAILED(hr) || object == nullptr) {
        error = hr == REGDB_E_CLASSNOTREG
                    ? "Der ASIO-Treiber ist nicht als 64-Bit-Version installiert."
                    : "Der ASIO-Treiber l\u00E4sst sich nicht laden (Fehler " + std::to_string(static_cast<long>(hr)) + ").";
        return false;
    }
    d->driver = static_cast<IASIO*>(object);
    d->name = driverName;

    if (!d->driver->init(window)) {
        error = d->driverError("Treiber meldet einen Fehler beim Start");
        close();
        return false;
    }

    long numIn = 0, numOut = 0;
    if (!succeeded(d->driver->getChannels(&numIn, &numOut)) || numOut < 1) {
        error = d->driverError("Der Treiber meldet keine Ausg\u00E4nge");
        close();
        return false;
    }
    d->inputs = std::min<int>(static_cast<int>(numIn), kMaxInputs);
    d->outputs = std::min<int>(static_cast<int>(numOut), kMaxOutputs);
    d->inError = d->inputs == 0 ? "Der Treiber hat keine Eing\u00E4nge" : "";

    // Samplerate: wie im Treiber eingestellt; meldet er keine, 48000 oder 44100 versuchen.
    ASIOSampleRate rate = 0.0;
    if (!succeeded(d->driver->getSampleRate(&rate)) || rate <= 0.0) {
        for (const double wanted : {48000.0, 44100.0}) {
            if (succeeded(d->driver->canSampleRate(wanted)) && succeeded(d->driver->setSampleRate(wanted))) {
                rate = wanted;
                break;
            }
        }
    }
    if (rate <= 0.0) {
        error = d->driverError("Der Treiber meldet keine Samplerate");
        close();
        return false;
    }
    d->rate = rate;

    long minSize = 0, maxSize = 0, preferred = 0, granularity = 0;
    if (!succeeded(d->driver->getBufferSize(&minSize, &maxSize, &preferred, &granularity)) || preferred <= 0) {
        error = d->driverError("Der Treiber meldet keine Puffergr\u00F6\u00DFe");
        close();
        return false;
    }
    d->bufferSize = static_cast<int>(preferred);

    // Puffer fuer alle Eingaenge und die ersten Ausgaenge.
    const int total = d->inputs + d->outputs;
    d->buffers.assign(static_cast<std::size_t>(total), ASIOBufferInfo{});
    d->types.assign(static_cast<std::size_t>(total), 0);
    for (int c = 0; c < total; ++c) {
        const bool isInput = c < d->inputs;
        ASIOBufferInfo& info = d->buffers[static_cast<std::size_t>(c)];
        info.isInput = isInput ? 1 : 0;
        info.channelNum = isInput ? c : c - d->inputs;

        ASIOChannelInfo channel{};
        channel.channel = info.channelNum;
        channel.isInput = info.isInput;
        if (!succeeded(d->driver->getChannelInfo(&channel))) {
            error = d->driverError("Kanalangaben des Treibers fehlen");
            close();
            return false;
        }
        if (!asio_format::supported(channel.type)) {
            error = "Der Treiber liefert ein Sampleformat, das MiniDAW nicht kennt (Typ " +
                    std::to_string(channel.type) + ").";
            close();
            return false;
        }
        d->types[static_cast<std::size_t>(c)] = channel.type;
    }

    g_active.store(d.get(), std::memory_order_release);
    if (!succeeded(d->driver->createBuffers(d->buffers.data(), total, d->bufferSize, &g_callbacks))) {
        error = d->driverError("Der Treiber kann die Puffer nicht anlegen");
        close();
        return false;
    }
    d->buffersCreated = true;

    if (!succeeded(d->driver->getLatencies(&d->inLatency, &d->outLatency))) {
        d->inLatency = d->outLatency = d->bufferSize;
    }
    d->useOutputReady = succeeded(d->driver->outputReady());

    const auto n = static_cast<std::size_t>(d->bufferSize);
    d->scratch.assign(static_cast<std::size_t>(total) * n, 0.0f);
    d->inPointers.resize(static_cast<std::size_t>(d->inputs));
    d->outPointers.resize(static_cast<std::size_t>(d->outputs));
    for (int c = 0; c < d->inputs; ++c)
        d->inPointers[static_cast<std::size_t>(c)] = d->scratch.data() + static_cast<std::size_t>(c) * n;
    for (int c = 0; c < d->outputs; ++c)
        d->outPointers[static_cast<std::size_t>(c)] = d->scratch.data() + static_cast<std::size_t>(d->inputs + c) * n;
    d->reset.store(false);
    d->latenciesChanged.store(false);
    return true;
}

bool AsioDevice::start(AudioCallback& callback, std::string& error) {
    if (!d->driver || !d->buffersCreated) {
        error = "Kein ASIO-Treiber offen.";
        return false;
    }
    stop();
    callback.prepare(d->rate, d->inputs, d->outputs, d->bufferSize);
    d->callback = &callback;
    d->processing.store(true, std::memory_order_release);
    if (!succeeded(d->driver->start())) {
        d->processing.store(false);
        d->callback = nullptr;
        error = d->driverError("Der Treiber startet nicht");
        return false;
    }
    d->running = true;
    return true;
}

void AsioDevice::stop() {
    d->processing.store(false, std::memory_order_release);
    if (d->driver && d->running) d->driver->stop();
    d->running = false;
    d->callback = nullptr;
}

void AsioDevice::close() {
    stop();
    if (d->driver) {
        if (d->buffersCreated) d->driver->disposeBuffers();
        d->driver->Release();
    }
    Impl* expected = d.get();
    g_active.compare_exchange_strong(expected, nullptr);
    d->driver = nullptr;
    d->buffersCreated = false;
    d->buffers.clear();
    d->types.clear();
    d->scratch.clear();
    d->inPointers.clear();
    d->outPointers.clear();
    d->rate = 0.0;
    d->inputs = d->outputs = d->bufferSize = 0;
    d->inLatency = d->outLatency = 0;
    d->reset.store(false);
}

double AsioDevice::sampleRate() const { return d->rate; }
int AsioDevice::numChannels() const { return d->outputs; }
int AsioDevice::bufferFrames() const { return d->bufferSize; }
int AsioDevice::inputChannels() const { return d->inputs; }
const std::string& AsioDevice::inputError() const { return d->inError; }

int AsioDevice::outputLatencyFrames() const {
    d->refreshLatencies();
    return static_cast<int>(d->outLatency);
}

int AsioDevice::inputLatencyFrames() const {
    d->refreshLatencies();
    return static_cast<int>(d->inLatency);
}

const std::wstring& AsioDevice::outputName() const { return d->driver ? d->name : d->emptyName; }
const std::wstring& AsioDevice::inputName() const { return d->driver ? d->name : d->emptyName; }
std::wstring AsioDevice::settingsKey() const { return L"asio:" + d->name; }

bool AsioDevice::resetRequested() const { return d->reset.load(); }

void AsioDevice::showControlPanel() {
    if (d->driver) d->driver->controlPanel();
}

unsigned AsioDevice::overloads() const { return d->overloads.load(std::memory_order_relaxed); }
