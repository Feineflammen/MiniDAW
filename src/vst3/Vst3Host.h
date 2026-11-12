#pragma once

#include "engine/AudioPlugin.h"
#include "util/SpscQueue.h"

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Host-Seite fuer VST3-Plugins. Dieser Teil ist plattformunabhaengig: Er
// bekommt die Fabrik eines bereits geladenen Moduls und kuemmert sich um
// Anlegen, Einrichten, Rechnen und Abbauen eines Plugins. Das Laden der
// Datei und das Editor-Fenster liegen in Vst3Module und PluginWindow.
namespace vst3 {

// Speicherstrom fuer Plugin-Zustaende.
class MemoryStream final : public Steinberg::IBStream {
public:
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override;
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }

    Steinberg::tresult PLUGIN_API read(void* buffer, Steinberg::int32 numBytes,
                                       Steinberg::int32* numBytesRead) override;
    Steinberg::tresult PLUGIN_API write(void* buffer, Steinberg::int32 numBytes,
                                        Steinberg::int32* numBytesWritten) override;
    Steinberg::tresult PLUGIN_API seek(Steinberg::int64 pos, Steinberg::int32 mode,
                                       Steinberg::int64* result) override;
    Steinberg::tresult PLUGIN_API tell(Steinberg::int64* pos) override;

    std::vector<char>& data() { return data_; }
    void rewind() { position_ = 0; }

private:
    std::vector<char> data_;
    std::size_t position_ = 0;
};

// Ereignisliste fester Groesse fuer einen Block.
class EventList final : public Steinberg::Vst::IEventList {
public:
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override;
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }

    Steinberg::int32 PLUGIN_API getEventCount() override { return count_; }
    Steinberg::tresult PLUGIN_API getEvent(Steinberg::int32 index, Steinberg::Vst::Event& e) override;
    Steinberg::tresult PLUGIN_API addEvent(Steinberg::Vst::Event& e) override;

    void clear() noexcept { count_ = 0; }

private:
    std::array<Steinberg::Vst::Event, 512> events_{};
    Steinberg::int32 count_ = 0;
};

// Wertverlauf eines Parameters innerhalb eines Blocks.
class ParamQueue final : public Steinberg::Vst::IParamValueQueue {
public:
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override;
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }

    Steinberg::Vst::ParamID PLUGIN_API getParameterId() override { return id_; }
    Steinberg::int32 PLUGIN_API getPointCount() override { return count_; }
    Steinberg::tresult PLUGIN_API getPoint(Steinberg::int32 index, Steinberg::int32& sampleOffset,
                                           Steinberg::Vst::ParamValue& value) override;
    Steinberg::tresult PLUGIN_API addPoint(Steinberg::int32 sampleOffset, Steinberg::Vst::ParamValue value,
                                           Steinberg::int32& index) override;

    void reset(Steinberg::Vst::ParamID id) noexcept { id_ = id; count_ = 0; }

private:
    struct Point {
        Steinberg::int32 offset = 0;
        Steinberg::Vst::ParamValue value = 0.0;
    };
    Steinberg::Vst::ParamID id_ = 0;
    std::array<Point, 16> points_{};
    Steinberg::int32 count_ = 0;
};

// Alle Parameteraenderungen eines Blocks, feste Groesse.
class ParamChanges final : public Steinberg::Vst::IParameterChanges {
public:
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override;
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }

    Steinberg::int32 PLUGIN_API getParameterCount() override { return count_; }
    Steinberg::Vst::IParamValueQueue* PLUGIN_API getParameterData(Steinberg::int32 index) override;
    Steinberg::Vst::IParamValueQueue* PLUGIN_API addParameterData(const Steinberg::Vst::ParamID& id,
                                                                  Steinberg::int32& index) override;

    void clear() noexcept { count_ = 0; }
    void add(Steinberg::Vst::ParamID id, Steinberg::int32 offset, Steinberg::Vst::ParamValue value) noexcept;

private:
    std::array<ParamQueue, 128> queues_{};
    Steinberg::int32 count_ = 0;
};

// Ein geladenes VST3-Plugin.
//
// Angelegt und zerstoert wird es im UI-Thread. process() laeuft im
// Audio-Thread; alles andere gehoert dem UI-Thread.
class Vst3Plugin final : public AudioPlugin, public Steinberg::Vst::IComponentHandler {
public:
    // Legt die Klasse classId aus der Fabrik an und richtet sie fertig ein
    // (Stereo, 32-Bit-Float). Bei einem Fehler nullptr und Grund in error.
    static std::unique_ptr<Vst3Plugin> create(Steinberg::IPluginFactory* factory,
                                              const Steinberg::TUID classId, bool instrumentHint,
                                              std::wstring name, double sampleRate, int maxFrames,
                                              std::string& error);
    ~Vst3Plugin() override;

    const std::wstring& name() const noexcept { return name_; }
    int latencyFrames() const noexcept { return latency_; }

    // Bedienoberflaeche des Plugins oder nullptr. Der Aufrufer gibt sie mit release() frei.
    Steinberg::IPlugView* createView();

    // Regelmaessig im UI-Thread rufen: reicht Parameteraenderungen, die beim
    // Rechnen entstanden sind, an die Bedienoberflaeche weiter.
    void idle();

    // --- AudioPlugin (Audio-Thread) ---
    bool isInstrument() const noexcept override { return instrument_; }
    void process(float* left, float* right, int numFrames, const PluginMidiEvent* events, int numEvents,
                 const PluginTransport& transport) noexcept override;

    // --- IComponentHandler (UI-Thread, gerufen vom Plugin) ---
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override;
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }
    Steinberg::tresult PLUGIN_API beginEdit(Steinberg::Vst::ParamID id) override;
    Steinberg::tresult PLUGIN_API performEdit(Steinberg::Vst::ParamID id,
                                              Steinberg::Vst::ParamValue valueNormalized) override;
    Steinberg::tresult PLUGIN_API endEdit(Steinberg::Vst::ParamID id) override;
    Steinberg::tresult PLUGIN_API restartComponent(Steinberg::int32 flags) override;

private:
    Vst3Plugin() = default;
    bool setup(Steinberg::IPluginFactory* factory, const Steinberg::TUID classId, bool instrumentHint,
               double sampleRate, int maxFrames, std::string& error);

    // Die Kanalpuffer eines Audio-Busses.
    struct Bus {
        int channels = 0;
        std::vector<std::vector<float>> storage;
        std::vector<float*> pointers;
    };
    void allocateBuses(std::vector<Bus>& buses, std::vector<Steinberg::Vst::AudioBusBuffers>& buffers,
                       Steinberg::Vst::BusDirection direction, int maxFrames);

    struct ParamEdit {
        Steinberg::Vst::ParamID id = 0;
        Steinberg::Vst::ParamValue value = 0.0;
    };

    std::wstring name_;
    bool instrument_ = false;
    bool separateController_ = false;
    bool componentInitialized_ = false;
    bool controllerInitialized_ = false;
    bool active_ = false;
    int latency_ = 0;

    Steinberg::IPtr<Steinberg::Vst::IComponent> component_;
    Steinberg::IPtr<Steinberg::Vst::IAudioProcessor> processor_;
    Steinberg::IPtr<Steinberg::Vst::IEditController> controller_;
    Steinberg::IPtr<Steinberg::Vst::IConnectionPoint> componentConnection_;
    Steinberg::IPtr<Steinberg::Vst::IConnectionPoint> controllerConnection_;

    // Zuordnung MIDI-Controller -> Parameter, je Kanal (VST3 kennt keine Controller-Ereignisse).
    static constexpr int kControllers = 130;
    std::vector<Steinberg::Vst::ParamID> midiMap_;  // 16 * kControllers Eintraege oder leer

    // Audio-Thread
    std::vector<Bus> inputs_, outputs_;
    std::vector<Steinberg::Vst::AudioBusBuffers> inputBuffers_, outputBuffers_;
    EventList inputEvents_, outputEvents_;
    ParamChanges inputParams_, outputParams_;
    Steinberg::Vst::ProcessContext context_{};
    std::int64_t continuousSamples_ = 0;

    SpscQueue<ParamEdit, 1024> editsToAudio_;  // UI -> Audio: Bedienung an der Oberflaeche
    SpscQueue<ParamEdit, 1024> editsToUi_;     // Audio -> UI: vom Plugin gemeldete Aenderungen
};

}  // namespace vst3
