#include "vst3/Vst3Host.h"

#include "pluginterfaces/vst/ivstattributes.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/vstspeaker.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <utility>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace vst3 {

namespace {

bool same(const TUID a, const TUID b) { return FUnknownPrivate::iidEqual(a, b); }

// --- Objekte, die der Host dem Plugin zur Verfuegung stellt -------------------

// Attributliste fuer Nachrichten zwischen den beiden Haelften eines Plugins.
class HostAttributeList final : public IAttributeList {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (same(iid, IAttributeList::iid) || same(iid, FUnknown::iid)) {
            addRef();
            *obj = static_cast<IAttributeList*>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refs_; }
    uint32 PLUGIN_API release() override {
        const uint32 r = --refs_;
        if (r == 0) delete this;
        return r;
    }

    tresult PLUGIN_API setInt(AttrID id, int64 value) override {
        if (!id) return kInvalidArgument;
        Entry& e = entries_[id];
        e = Entry{};
        e.type = Type::Int;
        e.intValue = value;
        return kResultOk;
    }
    tresult PLUGIN_API getInt(AttrID id, int64& value) override {
        const Entry* e = find(id, Type::Int);
        if (!e) return kResultFalse;
        value = e->intValue;
        return kResultOk;
    }
    tresult PLUGIN_API setFloat(AttrID id, double value) override {
        if (!id) return kInvalidArgument;
        Entry& e = entries_[id];
        e = Entry{};
        e.type = Type::Float;
        e.floatValue = value;
        return kResultOk;
    }
    tresult PLUGIN_API getFloat(AttrID id, double& value) override {
        const Entry* e = find(id, Type::Float);
        if (!e) return kResultFalse;
        value = e->floatValue;
        return kResultOk;
    }
    tresult PLUGIN_API setString(AttrID id, const TChar* string) override {
        if (!id || !string) return kInvalidArgument;
        std::size_t length = 0;
        while (string[length] != 0) ++length;
        Entry& e = entries_[id];
        e = Entry{};
        e.type = Type::String;
        e.bytes.resize((length + 1) * sizeof(TChar));
        std::memcpy(e.bytes.data(), string, e.bytes.size());
        return kResultOk;
    }
    tresult PLUGIN_API getString(AttrID id, TChar* string, uint32 sizeInBytes) override {
        const Entry* e = find(id, Type::String);
        if (!e || !string || sizeInBytes < sizeof(TChar)) return kResultFalse;
        const std::size_t n = std::min<std::size_t>(sizeInBytes, e->bytes.size());
        std::memcpy(string, e->bytes.data(), n);
        string[sizeInBytes / sizeof(TChar) - 1] = 0;  // immer abgeschlossen
        return kResultOk;
    }
    tresult PLUGIN_API setBinary(AttrID id, const void* data, uint32 sizeInBytes) override {
        if (!id || (!data && sizeInBytes > 0)) return kInvalidArgument;
        Entry& e = entries_[id];
        e = Entry{};
        e.type = Type::Binary;
        e.bytes.resize(sizeInBytes);
        if (sizeInBytes > 0) std::memcpy(e.bytes.data(), data, sizeInBytes);
        return kResultOk;
    }
    tresult PLUGIN_API getBinary(AttrID id, const void*& data, uint32& sizeInBytes) override {
        const Entry* e = find(id, Type::Binary);
        if (!e) return kResultFalse;
        data = e->bytes.data();
        sizeInBytes = static_cast<uint32>(e->bytes.size());
        return kResultOk;
    }

private:
    enum class Type { Int, Float, String, Binary };
    struct Entry {
        Type type = Type::Int;
        int64 intValue = 0;
        double floatValue = 0.0;
        std::vector<char> bytes;
    };
    const Entry* find(AttrID id, Type type) const {
        if (!id) return nullptr;
        const auto it = entries_.find(id);
        return (it != entries_.end() && it->second.type == type) ? &it->second : nullptr;
    }

    std::atomic<uint32> refs_{1};
    std::map<std::string, Entry> entries_;
};

class HostMessage final : public IMessage {
public:
    HostMessage() : attributes_(new HostAttributeList()) {}

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (same(iid, IMessage::iid) || same(iid, FUnknown::iid)) {
            addRef();
            *obj = static_cast<IMessage*>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refs_; }
    uint32 PLUGIN_API release() override {
        const uint32 r = --refs_;
        if (r == 0) {
            attributes_->release();
            delete this;
        }
        return r;
    }

    FIDString PLUGIN_API getMessageID() override { return id_.c_str(); }
    void PLUGIN_API setMessageID(FIDString id) override { id_ = id ? id : ""; }
    IAttributeList* PLUGIN_API getAttributes() override { return attributes_; }

private:
    std::atomic<uint32> refs_{1};
    std::string id_;
    HostAttributeList* attributes_;
};

// Der Host selbst, wie ihn das Plugin sieht.
class HostApplication final : public IHostApplication {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (same(iid, IHostApplication::iid) || same(iid, FUnknown::iid)) {
            *obj = static_cast<IHostApplication*>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }

    tresult PLUGIN_API getName(String128 name) override {
        static const char16_t kName[] = u"MiniDAW";
        std::memset(name, 0, sizeof(String128));
        std::memcpy(name, kName, sizeof kName);
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(TUID cid, TUID iid, void** obj) override {
        if (!obj) return kInvalidArgument;
        if (same(cid, IMessage::iid) && same(iid, IMessage::iid)) {
            *obj = static_cast<IMessage*>(new HostMessage());
            return kResultOk;
        }
        if (same(cid, IAttributeList::iid) && same(iid, IAttributeList::iid)) {
            *obj = static_cast<IAttributeList*>(new HostAttributeList());
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
};

HostApplication& hostApplication() {
    static HostApplication instance;
    return instance;
}

}  // namespace

// --- MemoryStream ------------------------------------------------------------

tresult PLUGIN_API MemoryStream::queryInterface(const TUID iid, void** obj) {
    if (same(iid, IBStream::iid) || same(iid, FUnknown::iid)) {
        *obj = static_cast<IBStream*>(this);
        return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
}

tresult PLUGIN_API MemoryStream::read(void* buffer, int32 numBytes, int32* numBytesRead) {
    if (!buffer || numBytes < 0) return kInvalidArgument;
    const std::size_t available = data_.size() - std::min(position_, data_.size());
    const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(numBytes), available);
    if (n > 0) std::memcpy(buffer, data_.data() + position_, n);
    position_ += n;
    if (numBytesRead) *numBytesRead = static_cast<int32>(n);
    return kResultOk;
}

tresult PLUGIN_API MemoryStream::write(void* buffer, int32 numBytes, int32* numBytesWritten) {
    if (!buffer || numBytes < 0) return kInvalidArgument;
    const auto n = static_cast<std::size_t>(numBytes);
    if (position_ + n > data_.size()) data_.resize(position_ + n);
    if (n > 0) std::memcpy(data_.data() + position_, buffer, n);
    position_ += n;
    if (numBytesWritten) *numBytesWritten = numBytes;
    return kResultOk;
}

tresult PLUGIN_API MemoryStream::seek(int64 pos, int32 mode, int64* result) {
    int64 target = pos;
    if (mode == kIBSeekCur) target = static_cast<int64>(position_) + pos;
    else if (mode == kIBSeekEnd) target = static_cast<int64>(data_.size()) + pos;
    if (target < 0) target = 0;
    position_ = static_cast<std::size_t>(target);
    if (result) *result = target;
    return kResultOk;
}

tresult PLUGIN_API MemoryStream::tell(int64* pos) {
    if (!pos) return kInvalidArgument;
    *pos = static_cast<int64>(position_);
    return kResultOk;
}

// --- EventList, ParamQueue, ParamChanges -------------------------------------

tresult PLUGIN_API EventList::queryInterface(const TUID iid, void** obj) {
    if (same(iid, IEventList::iid) || same(iid, FUnknown::iid)) {
        *obj = static_cast<IEventList*>(this);
        return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
}

tresult PLUGIN_API EventList::getEvent(int32 index, Event& e) {
    if (index < 0 || index >= count_) return kInvalidArgument;
    e = events_[static_cast<std::size_t>(index)];
    return kResultOk;
}

tresult PLUGIN_API EventList::addEvent(Event& e) {
    if (count_ >= static_cast<int32>(events_.size())) return kResultFalse;
    events_[static_cast<std::size_t>(count_++)] = e;
    return kResultOk;
}

tresult PLUGIN_API ParamQueue::queryInterface(const TUID iid, void** obj) {
    if (same(iid, IParamValueQueue::iid) || same(iid, FUnknown::iid)) {
        *obj = static_cast<IParamValueQueue*>(this);
        return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
}

tresult PLUGIN_API ParamQueue::getPoint(int32 index, int32& sampleOffset, ParamValue& value) {
    if (index < 0 || index >= count_) return kInvalidArgument;
    sampleOffset = points_[static_cast<std::size_t>(index)].offset;
    value = points_[static_cast<std::size_t>(index)].value;
    return kResultOk;
}

tresult PLUGIN_API ParamQueue::addPoint(int32 sampleOffset, ParamValue value, int32& index) {
    if (count_ >= static_cast<int32>(points_.size())) {
        // Voll: den letzten Punkt ueberschreiben, damit der juengste Wert gilt.
        index = count_ - 1;
        points_[static_cast<std::size_t>(index)] = {sampleOffset, value};
        return kResultOk;
    }
    index = count_;
    points_[static_cast<std::size_t>(count_++)] = {sampleOffset, value};
    return kResultOk;
}

tresult PLUGIN_API ParamChanges::queryInterface(const TUID iid, void** obj) {
    if (same(iid, IParameterChanges::iid) || same(iid, FUnknown::iid)) {
        *obj = static_cast<IParameterChanges*>(this);
        return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
}

IParamValueQueue* PLUGIN_API ParamChanges::getParameterData(int32 index) {
    if (index < 0 || index >= count_) return nullptr;
    return &queues_[static_cast<std::size_t>(index)];
}

IParamValueQueue* PLUGIN_API ParamChanges::addParameterData(const ParamID& id, int32& index) {
    for (int32 i = 0; i < count_; ++i) {
        if (queues_[static_cast<std::size_t>(i)].getParameterId() == id) {
            index = i;
            return &queues_[static_cast<std::size_t>(i)];
        }
    }
    if (count_ >= static_cast<int32>(queues_.size())) return nullptr;
    index = count_;
    queues_[static_cast<std::size_t>(count_)].reset(id);
    return &queues_[static_cast<std::size_t>(count_++)];
}

void ParamChanges::add(ParamID id, int32 offset, ParamValue value) noexcept {
    int32 index = 0;
    if (IParamValueQueue* queue = addParameterData(id, index)) queue->addPoint(offset, value, index);
}

// --- Vst3Plugin: Anlegen und Abbauen (UI-Thread) -----------------------------

std::unique_ptr<Vst3Plugin> Vst3Plugin::create(IPluginFactory* factory, const TUID classId,
                                               bool instrumentHint, std::wstring name, double sampleRate,
                                               int maxFrames, std::string& error) {
    std::unique_ptr<Vst3Plugin> plugin(new Vst3Plugin());
    plugin->name_ = std::move(name);
    if (!plugin->setup(factory, classId, instrumentHint, sampleRate, maxFrames, error)) return nullptr;
    return plugin;
}

bool Vst3Plugin::setup(IPluginFactory* factory, const TUID classId, bool instrumentHint, double sampleRate,
                       int maxFrames, std::string& error) {
    if (!factory) { error = "Plugin hat keine Fabrik"; return false; }

    // 1. Verarbeitungsteil anlegen.
    IComponent* rawComponent = nullptr;
    if (factory->createInstance(classId, IComponent::iid, reinterpret_cast<void**>(&rawComponent)) != kResultOk ||
        !rawComponent) {
        error = "Plugin-Klasse laesst sich nicht anlegen";
        return false;
    }
    component_ = owned(rawComponent);
    if (component_->initialize(&hostApplication()) != kResultOk) {
        error = "Plugin laesst sich nicht initialisieren";
        return false;
    }
    componentInitialized_ = true;

    processor_ = FUnknownPtr<IAudioProcessor>(component_);
    if (!processor_) { error = "Plugin kann kein Audio verarbeiten"; return false; }

    // 2. Bedienteil: entweder dasselbe Objekt oder eine eigene Klasse.
    controller_ = FUnknownPtr<IEditController>(component_);
    if (!controller_) {
        TUID controllerId{};
        if (component_->getControllerClassId(controllerId) == kResultOk) {
            IEditController* rawController = nullptr;
            if (factory->createInstance(controllerId, IEditController::iid,
                                        reinterpret_cast<void**>(&rawController)) == kResultOk && rawController) {
                controller_ = owned(rawController);
                separateController_ = true;
                if (controller_->initialize(&hostApplication()) == kResultOk) controllerInitialized_ = true;
                else controller_ = nullptr;
            }
        }
    }

    if (controller_) {
        controller_->setComponentHandler(this);

        // Getrennte Haelften miteinander verbinden, damit sie sich Nachrichten schicken koennen.
        if (separateController_) {
            componentConnection_ = FUnknownPtr<IConnectionPoint>(component_);
            controllerConnection_ = FUnknownPtr<IConnectionPoint>(controller_);
            if (componentConnection_ && controllerConnection_) {
                componentConnection_->connect(controllerConnection_);
                controllerConnection_->connect(componentConnection_);
            }
        }

        // Dem Bedienteil den Anfangszustand des Verarbeitungsteils mitteilen.
        MemoryStream state;
        if (component_->getState(&state) == kResultOk) {
            state.rewind();
            controller_->setComponentState(&state);
        }

        // MIDI-Controller (Haltepedal, Modulationsrad, Pitchbend ...) laufen in
        // VST3 ueber Parameter. Die Zuordnung einmal abfragen.
        if (FUnknownPtr<IMidiMapping> mapping{controller_}) {
            midiMap_.assign(static_cast<std::size_t>(16 * kControllers), kNoParamId);
            for (int channel = 0; channel < 16; ++channel) {
                for (int cc = 0; cc < kControllers; ++cc) {
                    ParamID id = kNoParamId;
                    if (mapping->getMidiControllerAssignment(0, static_cast<int16>(channel),
                                                             static_cast<CtrlNumber>(cc), id) == kResultOk)
                        midiMap_[static_cast<std::size_t>(channel * kControllers + cc)] = id;
                }
            }
        }
    }

    // 3. Busse: Hauptbus in Stereo, alle weiteren bleiben aus.
    const int32 numIn = component_->getBusCount(kAudio, kInput);
    const int32 numOut = component_->getBusCount(kAudio, kOutput);
    {
        std::vector<SpeakerArrangement> in(static_cast<std::size_t>(numIn), 0), out(static_cast<std::size_t>(numOut), 0);
        for (int32 i = 0; i < numIn; ++i) processor_->getBusArrangement(kInput, i, in[static_cast<std::size_t>(i)]);
        for (int32 i = 0; i < numOut; ++i) processor_->getBusArrangement(kOutput, i, out[static_cast<std::size_t>(i)]);
        if (numIn > 0) in[0] = SpeakerArr::kStereo;
        if (numOut > 0) out[0] = SpeakerArr::kStereo;
        // Lehnt das Plugin Stereo ab, behaelt es seine Anordnung; die Kanalzahl wird unten abgefragt.
        processor_->setBusArrangements(in.data(), numIn, out.data(), numOut);
    }
    for (int32 i = 0; i < numIn; ++i) component_->activateBus(kAudio, kInput, i, i == 0);
    for (int32 i = 0; i < numOut; ++i) component_->activateBus(kAudio, kOutput, i, i == 0);
    const int32 numEventIn = component_->getBusCount(kEvent, kInput);
    for (int32 i = 0; i < numEventIn; ++i) component_->activateBus(kEvent, kInput, i, i == 0);
    const int32 numEventOut = component_->getBusCount(kEvent, kOutput);
    for (int32 i = 0; i < numEventOut; ++i) component_->activateBus(kEvent, kOutput, i, false);

    instrument_ = instrumentHint || (numEventIn > 0 && numIn == 0);

    // 4. Verarbeitung einrichten und einschalten.
    if (processor_->canProcessSampleSize(kSample32) != kResultOk) {
        error = "Plugin unterstuetzt kein 32-Bit-Float";
        return false;
    }
    ProcessSetup setup{};
    setup.processMode = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = maxFrames;
    setup.sampleRate = sampleRate;
    if (processor_->setupProcessing(setup) != kResultOk) {
        error = "Plugin lehnt Samplerate oder Blockgroesse ab";
        return false;
    }

    allocateBuses(inputs_, inputBuffers_, kInput, maxFrames);
    allocateBuses(outputs_, outputBuffers_, kOutput, maxFrames);

    if (component_->setActive(true) != kResultOk) {
        error = "Plugin laesst sich nicht aktivieren";
        return false;
    }
    active_ = true;
    latency_ = static_cast<int>(processor_->getLatencySamples());
    processor_->setProcessing(true);
    return true;
}

void Vst3Plugin::allocateBuses(std::vector<Bus>& buses, std::vector<AudioBusBuffers>& buffers,
                               BusDirection direction, int maxFrames) {
    const int32 count = component_->getBusCount(kAudio, direction);
    buses.assign(static_cast<std::size_t>(count), Bus{});
    buffers.assign(static_cast<std::size_t>(count), AudioBusBuffers{});
    for (int32 i = 0; i < count; ++i) {
        BusInfo info{};
        component_->getBusInfo(kAudio, direction, i, info);
        Bus& bus = buses[static_cast<std::size_t>(i)];
        bus.channels = std::clamp(static_cast<int>(info.channelCount), 0, 64);
        // Auch abgeschaltete Busse bekommen gueltige Puffer; manche Plugins greifen trotzdem zu.
        bus.storage.assign(static_cast<std::size_t>(bus.channels),
                           std::vector<float>(static_cast<std::size_t>(std::max(1, maxFrames)), 0.0f));
        for (auto& channel : bus.storage) bus.pointers.push_back(channel.data());
        buffers[static_cast<std::size_t>(i)].numChannels = bus.channels;
        buffers[static_cast<std::size_t>(i)].silenceFlags = 0;
        buffers[static_cast<std::size_t>(i)].channelBuffers32 = bus.pointers.empty() ? nullptr : bus.pointers.data();
    }
}

Vst3Plugin::~Vst3Plugin() {
    if (processor_ && active_) processor_->setProcessing(false);
    if (component_ && active_) component_->setActive(false);
    if (controller_) controller_->setComponentHandler(nullptr);
    if (componentConnection_ && controllerConnection_) {
        componentConnection_->disconnect(controllerConnection_);
        controllerConnection_->disconnect(componentConnection_);
    }
    componentConnection_ = nullptr;
    controllerConnection_ = nullptr;
    if (controller_ && separateController_ && controllerInitialized_) controller_->terminate();
    controller_ = nullptr;
    processor_ = nullptr;
    if (component_ && componentInitialized_) component_->terminate();
    component_ = nullptr;
}

IPlugView* Vst3Plugin::createView() {
    return controller_ ? controller_->createView(ViewType::kEditor) : nullptr;
}

void Vst3Plugin::idle() {
    ParamEdit edit;
    while (editsToUi_.pop(edit))
        if (controller_) controller_->setParamNormalized(edit.id, edit.value);
}

// --- Vst3Plugin: IComponentHandler (UI-Thread) -------------------------------

tresult PLUGIN_API Vst3Plugin::queryInterface(const TUID iid, void** obj) {
    if (same(iid, IComponentHandler::iid) || same(iid, FUnknown::iid)) {
        *obj = static_cast<IComponentHandler*>(this);
        return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
}

tresult PLUGIN_API Vst3Plugin::beginEdit(ParamID) { return kResultOk; }

tresult PLUGIN_API Vst3Plugin::performEdit(ParamID id, ParamValue valueNormalized) {
    // Der Benutzer dreht an der Oberflaeche: an den Audio-Thread weiterreichen.
    editsToAudio_.push({id, valueNormalized});
    return kResultOk;
}

tresult PLUGIN_API Vst3Plugin::endEdit(ParamID) { return kResultOk; }

tresult PLUGIN_API Vst3Plugin::restartComponent(int32) { return kResultOk; }

// --- Vst3Plugin: Rechnen (Audio-Thread) --------------------------------------

void Vst3Plugin::process(float* left, float* right, int numFrames, const PluginMidiEvent* events,
                         int numEvents, const PluginTransport& transport) noexcept {
    const std::size_t bytes = static_cast<std::size_t>(numFrames) * sizeof(float);

    // Eingang auf den Hauptbus legen.
    if (!inputs_.empty()) {
        Bus& in = inputs_[0];
        if (in.channels >= 2) {
            std::memcpy(in.pointers[0], left, bytes);
            std::memcpy(in.pointers[1], right, bytes);
        } else if (in.channels == 1) {
            for (int i = 0; i < numFrames; ++i) in.pointers[0][i] = 0.5f * (left[i] + right[i]);
        }
        inputBuffers_[0].silenceFlags = 0;
    }

    // Parameter: Bedienung an der Oberflaeche und MIDI-Controller.
    inputParams_.clear();
    outputParams_.clear();
    ParamEdit edit;
    while (editsToAudio_.pop(edit)) inputParams_.add(edit.id, 0, edit.value);

    // MIDI in VST3-Ereignisse uebersetzen.
    inputEvents_.clear();
    outputEvents_.clear();
    for (int i = 0; i < numEvents; ++i) {
        const MidiMessage m = events[i].message;
        const int offset = std::clamp(events[i].offset, 0, numFrames - 1);
        const auto channel = static_cast<int16>(m.status & 0x0F);
        Event e{};
        e.busIndex = 0;
        e.sampleOffset = offset;
        e.flags = Event::kIsLive;
        if (m.isNoteOn()) {
            e.type = Event::kNoteOnEvent;
            e.noteOn.channel = channel;
            e.noteOn.pitch = m.data1;
            e.noteOn.velocity = static_cast<float>(m.data2) / 127.0f;
            e.noteOn.noteId = -1;
            inputEvents_.addEvent(e);
        } else if (m.isNoteOff()) {
            e.type = Event::kNoteOffEvent;
            e.noteOff.channel = channel;
            e.noteOff.pitch = m.data1;
            e.noteOff.velocity = 0.0f;
            e.noteOff.noteId = -1;
            inputEvents_.addEvent(e);
        } else if (!midiMap_.empty()) {
            int controller = -1;
            double value = 0.0;
            if (m.type() == 0xB0) {
                controller = m.data1;
                value = static_cast<double>(m.data2) / 127.0;
            } else if (m.type() == 0xE0) {
                controller = kPitchBend;
                value = static_cast<double>((m.data2 << 7) | m.data1) / 16383.0;
            } else if (m.type() == 0xD0) {
                controller = kAfterTouch;
                value = static_cast<double>(m.data1) / 127.0;
            }
            if (controller >= 0 && controller < kControllers) {
                const ParamID id = midiMap_[static_cast<std::size_t>(channel * kControllers + controller)];
                if (id != kNoParamId) inputParams_.add(id, offset, value);
            }
        }
    }

    // Transport.
    const double quarters = static_cast<double>(transport.position) / transport.sampleRate * transport.tempo / 60.0;
    context_.state = (transport.playing ? static_cast<uint32>(ProcessContext::kPlaying) : 0u) | ProcessContext::kTempoValid |
                     ProcessContext::kTimeSigValid | ProcessContext::kProjectTimeMusicValid |
                     ProcessContext::kBarPositionValid | ProcessContext::kContTimeValid;
    context_.sampleRate = transport.sampleRate;
    context_.projectTimeSamples = transport.position;
    context_.continousTimeSamples = continuousSamples_;
    context_.projectTimeMusic = quarters;
    context_.barPositionMusic = std::floor(quarters / 4.0) * 4.0;
    context_.tempo = transport.tempo;
    context_.timeSigNumerator = 4;
    context_.timeSigDenominator = 4;
    continuousSamples_ += numFrames;

    for (auto& out : outputBuffers_) out.silenceFlags = 0;

    ProcessData data{};
    data.processMode = kRealtime;
    data.symbolicSampleSize = kSample32;
    data.numSamples = numFrames;
    data.numInputs = static_cast<int32>(inputBuffers_.size());
    data.numOutputs = static_cast<int32>(outputBuffers_.size());
    data.inputs = inputBuffers_.empty() ? nullptr : inputBuffers_.data();
    data.outputs = outputBuffers_.empty() ? nullptr : outputBuffers_.data();
    data.inputParameterChanges = &inputParams_;
    data.outputParameterChanges = &outputParams_;
    data.inputEvents = &inputEvents_;
    data.outputEvents = &outputEvents_;
    data.processContext = &context_;

    const tresult result = processor_->process(data);

    // Ausgang vom Hauptbus holen.
    if (result != kResultOk || outputs_.empty() || outputs_[0].channels == 0) {
        std::memset(left, 0, bytes);
        std::memset(right, 0, bytes);
    } else {
        const Bus& out = outputs_[0];
        std::memcpy(left, out.pointers[0], bytes);
        std::memcpy(right, out.pointers[out.channels >= 2 ? 1 : 0], bytes);
    }
    // Schutz fuer Ohren und Lautsprecher: kaputte Werte nicht durchlassen.
    for (int i = 0; i < numFrames; ++i) {
        if (!std::isfinite(left[i])) left[i] = 0.0f;
        if (!std::isfinite(right[i])) right[i] = 0.0f;
    }

    // Vom Plugin gemeldete Parameteraenderungen an die Oberflaeche weiterreichen.
    const int32 changed = outputParams_.getParameterCount();
    for (int32 i = 0; i < changed; ++i) {
        IParamValueQueue* queue = outputParams_.getParameterData(i);
        const int32 points = queue ? queue->getPointCount() : 0;
        if (points <= 0) continue;
        int32 offset = 0;
        ParamValue value = 0.0;
        if (queue->getPoint(points - 1, offset, value) == kResultOk)
            editsToUi_.push({queue->getParameterId(), value});
    }
}

}  // namespace vst3
