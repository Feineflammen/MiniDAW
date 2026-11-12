// Prueft den VST3-Host gegen ein kleines, hier im Test gebautes Plugin.
// Das ersetzt keinen Test mit echten Plugins, deckt aber Reihenfolge der
// Aufrufe, Referenzzaehlung, MIDI-Uebersetzung und Parameterwege ab.

#include "vst3/Vst3Host.h"

#include "pluginterfaces/vst/ivstattributes.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

int failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ++failures;                                                          \
            std::printf("FEHLER %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                        \
    } while (0)

const TUID kSynthClass = INLINE_UID(0x11111111, 0x22222222, 0x33333333, 0x44444444);
constexpr ParamID kVolumeParam = 7;
constexpr ParamID kSustainParam = 8;

int liveObjects = 0;

// Instrument aus einem Stueck: Verarbeitung und Bedienteil in einem Objekt.
// Gibt 0.25 * Lautstaerke aus, solange eine Note gehalten wird.
struct TestSynth final : IComponent, IAudioProcessor, IEditController, IMidiMapping {
    uint32 refs = 1;
    bool initialized = false, active = false, processing = false, setupDone = false, terminated = false;
    bool orderOk = true;
    IComponentHandler* handler = nullptr;
    int held = 0;
    double volume = 1.0;
    double sustain = 0.0;
    int firstNoteOffset = -1;
    double lastTempo = 0.0;
    bool sawPlaying = false;
    bool hostNameOk = false;

    TestSynth() { ++liveObjects; }
    ~TestSynth() { --liveObjects; }

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        *obj = nullptr;
        if (FUnknownPrivate::iidEqual(iid, IComponent::iid) || FUnknownPrivate::iidEqual(iid, IPluginBase::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid))
            *obj = static_cast<IComponent*>(this);
        else if (FUnknownPrivate::iidEqual(iid, IAudioProcessor::iid)) *obj = static_cast<IAudioProcessor*>(this);
        else if (FUnknownPrivate::iidEqual(iid, IEditController::iid)) *obj = static_cast<IEditController*>(this);
        else if (FUnknownPrivate::iidEqual(iid, IMidiMapping::iid)) *obj = static_cast<IMidiMapping*>(this);
        if (!*obj) return kNoInterface;
        ++refs;
        return kResultOk;
    }
    uint32 PLUGIN_API addRef() override { return ++refs; }
    uint32 PLUGIN_API release() override {
        const uint32 r = --refs;
        if (r == 0) delete this;
        return r;
    }

    // IPluginBase
    tresult PLUGIN_API initialize(FUnknown* context) override {
        if (initialized) orderOk = false;
        initialized = true;
        FUnknownPtr<IHostApplication> host(context);
        if (host) {
            String128 name{};
            host->getName(name);
            hostNameOk = name[0] == u'M';
            // Der Host muss Nachrichten und Attributlisten anlegen koennen.
            IMessage* message = nullptr;
            TUID messageId;
            IMessage::iid.toTUID(messageId);
            if (host->createInstance(messageId, messageId, reinterpret_cast<void**>(&message)) == kResultOk && message) {
                message->setMessageID("test");
                message->getAttributes()->setInt("zahl", 42);
                int64 value = 0;
                if (message->getAttributes()->getInt("zahl", value) != kResultOk || value != 42) orderOk = false;
                if (std::strcmp(message->getMessageID(), "test") != 0) orderOk = false;
                message->release();
            } else {
                orderOk = false;
            }
        }
        return kResultOk;
    }
    tresult PLUGIN_API terminate() override {
        if (!initialized || active || terminated) orderOk = false;
        terminated = true;
        return kResultOk;
    }

    // IComponent
    tresult PLUGIN_API getControllerClassId(TUID) override { return kNotImplemented; }
    tresult PLUGIN_API setIoMode(IoMode) override { return kResultOk; }
    int32 PLUGIN_API getBusCount(MediaType type, BusDirection dir) override {
        if (type == kAudio) return dir == kOutput ? 1 : 0;
        return dir == kInput ? 1 : 0;
    }
    tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo& bus) override {
        if (index != 0) return kInvalidArgument;
        bus = BusInfo{};
        bus.mediaType = type;
        bus.direction = dir;
        bus.channelCount = type == kAudio ? 2 : 16;
        bus.busType = kMain;
        bus.flags = BusInfo::kDefaultActive;
        return kResultOk;
    }
    tresult PLUGIN_API getRoutingInfo(RoutingInfo&, RoutingInfo&) override { return kNotImplemented; }
    tresult PLUGIN_API activateBus(MediaType, BusDirection, int32, TBool) override {
        if (!initialized || active) orderOk = false;
        return kResultOk;
    }
    tresult PLUGIN_API setActive(TBool state) override {
        if (!initialized || !setupDone) orderOk = false;
        if (!state && processing) orderOk = false;  // erst setProcessing(false)
        active = state != 0;
        return kResultOk;
    }
    tresult PLUGIN_API setState(IBStream*) override { return kResultOk; }
    tresult PLUGIN_API getState(IBStream* state) override {
        int32 magic = 1234;
        return state->write(&magic, sizeof magic, nullptr);
    }

    // IAudioProcessor
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement*, int32 numIns, SpeakerArrangement* outputs,
                                          int32 numOuts) override {
        return (numIns == 0 && numOuts == 1 && outputs[0] == 3) ? kResultOk : kResultFalse;
    }
    tresult PLUGIN_API getBusArrangement(BusDirection, int32, SpeakerArrangement& arr) override {
        arr = 3;
        return kResultOk;
    }
    tresult PLUGIN_API canProcessSampleSize(int32 size) override { return size == kSample32 ? kResultOk : kResultFalse; }
    uint32 PLUGIN_API getLatencySamples() override { return 32; }
    tresult PLUGIN_API setupProcessing(ProcessSetup& setup) override {
        if (active) orderOk = false;
        setupDone = setup.sampleRate == 48000.0 && setup.maxSamplesPerBlock == 256 &&
                    setup.symbolicSampleSize == kSample32;
        return setupDone ? kResultOk : kResultFalse;
    }
    tresult PLUGIN_API setProcessing(TBool state) override {
        if (state && !active) orderOk = false;
        processing = state != 0;
        return kResultOk;
    }
    tresult PLUGIN_API process(ProcessData& data) override {
        if (!active || !processing) orderOk = false;
        if (data.numInputs != 0 || data.numOutputs != 1 || !data.outputs || data.outputs[0].numChannels != 2)
            orderOk = false;
        if (data.processContext) {
            lastTempo = data.processContext->tempo;
            sawPlaying = (data.processContext->state & ProcessContext::kPlaying) != 0;
        }
        if (data.inputParameterChanges) {
            for (int32 i = 0; i < data.inputParameterChanges->getParameterCount(); ++i) {
                IParamValueQueue* queue = data.inputParameterChanges->getParameterData(i);
                int32 offset = 0;
                ParamValue value = 0.0;
                if (queue && queue->getPoint(queue->getPointCount() - 1, offset, value) == kResultOk) {
                    if (queue->getParameterId() == kVolumeParam) volume = value;
                    if (queue->getParameterId() == kSustainParam) sustain = value;
                }
            }
        }
        int32 eventIndex = 0;
        const int32 eventCount = data.inputEvents ? data.inputEvents->getEventCount() : 0;
        for (int32 i = 0; i < data.numSamples; ++i) {
            while (eventIndex < eventCount) {
                Event e{};
                data.inputEvents->getEvent(eventIndex, e);
                if (e.sampleOffset > i) break;
                if (e.type == Event::kNoteOnEvent) {
                    if (firstNoteOffset < 0) firstNoteOffset = e.sampleOffset;
                    ++held;
                } else if (e.type == Event::kNoteOffEvent && held > 0) {
                    --held;
                }
                ++eventIndex;
            }
            const float s = held > 0 ? static_cast<float>(0.25 * volume) : 0.0f;
            data.outputs[0].channelBuffers32[0][i] = s;
            data.outputs[0].channelBuffers32[1][i] = -s;
        }
        // Dem Host eine eigene Parameteraenderung melden.
        if (data.outputParameterChanges && held > 0) {
            int32 index = 0;
            if (IParamValueQueue* queue = data.outputParameterChanges->addParameterData(kVolumeParam, index))
                queue->addPoint(0, 0.5, index);
        }
        return kResultOk;
    }
    uint32 PLUGIN_API getTailSamples() override { return 0; }

    // IEditController
    double uiVolume = -1.0;
    bool gotComponentState = false;
    tresult PLUGIN_API setComponentState(IBStream* state) override {
        int32 magic = 0, read = 0;
        state->read(&magic, sizeof magic, &read);
        gotComponentState = magic == 1234 && read == 4;
        return kResultOk;
    }
    int32 PLUGIN_API getParameterCount() override { return 2; }
    tresult PLUGIN_API getParameterInfo(int32, ParameterInfo&) override { return kNotImplemented; }
    tresult PLUGIN_API getParamStringByValue(ParamID, ParamValue, String128) override { return kNotImplemented; }
    tresult PLUGIN_API getParamValueByString(ParamID, TChar*, ParamValue&) override { return kNotImplemented; }
    ParamValue PLUGIN_API normalizedParamToPlain(ParamID, ParamValue v) override { return v; }
    ParamValue PLUGIN_API plainParamToNormalized(ParamID, ParamValue v) override { return v; }
    ParamValue PLUGIN_API getParamNormalized(ParamID) override { return volume; }
    tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue value) override {
        if (id == kVolumeParam) uiVolume = value;
        return kResultOk;
    }
    tresult PLUGIN_API setComponentHandler(IComponentHandler* h) override {
        handler = h;
        return kResultOk;
    }
    IPlugView* PLUGIN_API createView(FIDString) override { return nullptr; }

    // IMidiMapping: Haltepedal auf einen Parameter legen.
    tresult PLUGIN_API getMidiControllerAssignment(int32, int16, CtrlNumber cc, ParamID& id) override {
        if (cc == kCtrlSustainOnOff) {
            id = kSustainParam;
            return kResultOk;
        }
        return kResultFalse;
    }
};

TestSynth* lastSynth = nullptr;

struct TestFactory final : IPluginFactory {
    tresult PLUGIN_API queryInterface(const TUID, void** obj) override { *obj = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    tresult PLUGIN_API getFactoryInfo(PFactoryInfo*) override { return kNotImplemented; }
    int32 PLUGIN_API countClasses() override { return 1; }
    tresult PLUGIN_API getClassInfo(int32, PClassInfo*) override { return kNotImplemented; }
    tresult PLUGIN_API createInstance(FIDString cid, FIDString iid, void** obj) override {
        *obj = nullptr;
        if (!FUnknownPrivate::iidEqual(cid, kSynthClass)) return kNoInterface;
        auto* synth = new TestSynth();
        lastSynth = synth;
        const tresult result = synth->queryInterface(iid, obj);
        synth->release();
        return result;
    }
};

void testPluginLifecycle() {
    TestFactory factory;
    std::string error;

    // Falsche Klasse und falsche Samplerate werden sauber abgelehnt, ohne Leck.
    const TUID kWrong = INLINE_UID(1, 2, 3, 4);
    CHECK(vst3::Vst3Plugin::create(&factory, kWrong, true, L"x", 48000.0, 256, error) == nullptr);
    CHECK(!error.empty());
    CHECK(vst3::Vst3Plugin::create(&factory, kSynthClass, true, L"x", 44100.0, 256, error) == nullptr);
    CHECK(liveObjects == 0);

    auto plugin = vst3::Vst3Plugin::create(&factory, kSynthClass, false, L"Testsynth", 48000.0, 256, error);
    CHECK(plugin != nullptr);
    if (!plugin) return;
    TestSynth* synth = lastSynth;
    CHECK(liveObjects == 1);
    CHECK(plugin->isInstrument());  // erkannt an "MIDI rein, kein Audio rein"
    CHECK(plugin->name() == L"Testsynth");
    CHECK(plugin->latencyFrames() == 32);
    CHECK(synth->initialized && synth->active && synth->processing);
    CHECK(synth->hostNameOk);
    CHECK(synth->gotComponentState);
    CHECK(synth->handler != nullptr);
    CHECK(plugin->createView() == nullptr);

    std::vector<float> l(256, 9.0f), r(256, 9.0f);
    PluginTransport transport{true, 48000, 90.0, 48000.0};

    // Ohne Noten: Stille (der alte Pufferinhalt wird ueberschrieben).
    plugin->process(l.data(), r.data(), 256, nullptr, 0, transport);
    CHECK(l[0] == 0.0f && r[255] == 0.0f);
    CHECK(synth->lastTempo == 90.0 && synth->sawPlaying);

    // Note an bei Offset 100: davor Stille, danach Ton, links und rechts wie vom Plugin geliefert.
    const PluginMidiEvent on[] = {{100, MidiMessage::noteOn(60, 100)}};
    plugin->process(l.data(), r.data(), 256, on, 1, transport);
    CHECK(synth->firstNoteOffset == 100);
    CHECK(l[99] == 0.0f && l[100] == 0.25f && r[100] == -0.25f);

    // Haltepedal (Controller 64) kommt ueber die Parameter-Zuordnung an.
    const PluginMidiEvent pedal[] = {{0, MidiMessage{0xB0, 64, 127}}};
    plugin->process(l.data(), r.data(), 256, pedal, 1, transport);
    CHECK(synth->sustain == 1.0);

    // Bedienung an der Oberflaeche: performEdit landet im naechsten Block beim Plugin.
    CHECK(synth->handler->performEdit(kVolumeParam, 0.5) == kResultOk);
    plugin->process(l.data(), r.data(), 256, nullptr, 0, transport);
    CHECK(synth->volume == 0.5);
    CHECK(l[10] == 0.125f);

    // Vom Plugin gemeldete Aenderung erreicht den Bedienteil ueber idle().
    CHECK(synth->uiVolume == -1.0);
    plugin->idle();
    CHECK(synth->uiVolume == 0.5);

    const PluginMidiEvent off[] = {{0, MidiMessage::noteOff(60)}};
    plugin->process(l.data(), r.data(), 256, off, 1, transport);
    CHECK(l[0] == 0.0f);

    // Sehr viele Ereignisse in einem Block duerfen nichts ueberlaufen lassen.
    std::vector<PluginMidiEvent> many;
    for (int i = 0; i < 2000; ++i) many.push_back({i % 256, MidiMessage::noteOn(60, 100)});
    plugin->process(l.data(), r.data(), 256, many.data(), static_cast<int>(many.size()), transport);

    CHECK(synth->orderOk);
    plugin.reset();
    CHECK(liveObjects == 0);  // sauber abgebaut, alle Referenzen freigegeben
}

void testStreamAndLists() {
    vst3::MemoryStream stream;
    int32 written = 0, readCount = 0;
    char text[] = "hallo welt";
    CHECK(stream.write(text, 10, &written) == kResultOk && written == 10);
    int64 pos = -1;
    stream.seek(6, IBStream::kIBSeekSet, &pos);
    CHECK(pos == 6);
    char out[8] = {};
    CHECK(stream.read(out, 8, &readCount) == kResultOk && readCount == 4);
    CHECK(std::strncmp(out, "welt", 4) == 0);
    stream.seek(-2, IBStream::kIBSeekEnd, &pos);
    CHECK(pos == 8);
    stream.tell(&pos);
    CHECK(pos == 8);

    vst3::ParamChanges changes;
    for (int i = 0; i < 300; ++i) changes.add(static_cast<ParamID>(i), 0, 0.5);
    CHECK(changes.getParameterCount() == 128);  // begrenzt statt uebergelaufen
    changes.clear();
    for (int i = 0; i < 40; ++i) changes.add(3, i, i / 40.0);
    CHECK(changes.getParameterCount() == 1);
    CHECK(changes.getParameterData(0)->getPointCount() == 16);
    CHECK(changes.getParameterData(1) == nullptr);

    vst3::EventList events;
    Event e{};
    for (int i = 0; i < 600; ++i) events.addEvent(e);
    CHECK(events.getEventCount() == 512);
    CHECK(events.getEvent(600, e) != kResultOk);
}

}  // namespace

int main() {
    testPluginLifecycle();
    testStreamAndLists();
    if (failures == 0) {
        std::printf("Alle VST3-Tests bestanden.\n");
        return 0;
    }
    std::printf("%d Pruefung(en) fehlgeschlagen.\n", failures);
    return 1;
}
