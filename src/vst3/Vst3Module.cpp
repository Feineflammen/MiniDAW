#include "vst3/Vst3Module.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstring>
#include <map>

using namespace Steinberg;

namespace vst3 {

namespace {

struct Module {
    HMODULE handle = nullptr;
    IPluginFactory* factory = nullptr;
};

// Geladene Dateien, nach Pfad. Sie werden nie entladen: Ein Plugin kann noch
// Fenster oder Threads haben, und das Entladen bringt beim Beenden nichts.
std::map<std::wstring, Module>& modules() {
    static std::map<std::wstring, Module> instance;
    return instance;
}

// Ein Plugin ist entweder eine einzelne Datei "Name.vst3" oder ein Ordner
// "Name.vst3" mit der eigentlichen Datei unter Contents\x86_64-win.
std::filesystem::path resolveBinary(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec)) return path;
#if defined(_M_ARM64)
    const wchar_t* const arch = L"arm64-win";
#elif defined(_M_X64)
    const wchar_t* const arch = L"x86_64-win";
#else
    const wchar_t* const arch = L"x86-win";
#endif
    return path / L"Contents" / arch / path.filename();
}

void collect(const std::filesystem::path& folder, std::vector<PluginEntry>& out, int depth) {
    std::error_code ec;
    for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        const std::filesystem::path path = it->path();
        std::wstring extension = path.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
        if (extension == L".vst3") {
            // Datei oder Buendel-Ordner; in ein Buendel wird nicht hineingesucht.
            if (std::filesystem::exists(resolveBinary(path), ec)) out.push_back({path.stem().wstring(), path});
        } else if (it->is_directory(ec) && depth < 4) {
            collect(path, out, depth + 1);  // Hersteller-Unterordner
        }
    }
}

Module* loadModule(const std::filesystem::path& path, std::string& error) {
    const std::filesystem::path binary = resolveBinary(path);
    const std::wstring key = binary.wstring();
    auto& cache = modules();
    if (const auto it = cache.find(key); it != cache.end()) return &it->second;

    // Mit diesem Schalter findet Windows auch DLLs, die neben dem Plugin liegen.
    const HMODULE handle = LoadLibraryExW(key.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!handle) {
        const DWORD code = GetLastError();
        error = code == ERROR_BAD_EXE_FORMAT ? "Die Datei ist kein 64-Bit-Plugin fuer dieses System"
                : code == ERROR_MOD_NOT_FOUND ? "Die Datei oder eine von ihr benoetigte DLL fehlt"
                                              : "Die Datei laesst sich nicht laden (Fehler " + std::to_string(code) + ")";
        return nullptr;
    }

    using InitFunction = bool (*)();
    using FactoryFunction = IPluginFactory* (PLUGIN_API*)();
    if (const auto init = reinterpret_cast<InitFunction>(reinterpret_cast<void*>(GetProcAddress(handle, "InitDll")))) {
        if (!init()) {
            error = "Das Plugin meldet einen Fehler beim Start";
            FreeLibrary(handle);
            return nullptr;
        }
    }
    const auto getFactory =
        reinterpret_cast<FactoryFunction>(reinterpret_cast<void*>(GetProcAddress(handle, "GetPluginFactory")));
    IPluginFactory* factory = getFactory ? getFactory() : nullptr;
    if (!factory) {
        error = "Die Datei ist kein VST3-Plugin";
        FreeLibrary(handle);
        return nullptr;
    }

    Module& module = cache[key];
    module.handle = handle;
    module.factory = factory;
    return &module;
}

std::wstring widen(const char* text) {
    std::wstring out;
    for (; text && *text; ++text) out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*text)));
    return out;
}

}  // namespace

std::filesystem::path pluginFolder() {
    std::filesystem::path folder;
    PWSTR common = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFilesCommon, 0, nullptr, &common)) && common)
        folder = std::filesystem::path(common) / L"VST3";
    if (common) CoTaskMemFree(common);
    return folder;
}

std::vector<PluginEntry> scanPlugins() {
    std::vector<PluginEntry> found;
    const auto folder = pluginFolder();
    if (!folder.empty()) collect(folder, found, 0);
    std::sort(found.begin(), found.end(), [](const PluginEntry& a, const PluginEntry& b) {
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return found;
}

std::unique_ptr<Vst3Plugin> loadPlugin(const std::filesystem::path& path, double sampleRate, int maxFrames,
                                       std::string& error) {
    Module* module = loadModule(path, error);
    if (!module) return nullptr;

    // Die erste Klasse der Kategorie "Audio Module Class" ist das Plugin selbst.
    IPluginFactory* factory = module->factory;
    FUnknownPtr<IPluginFactory2> factory2(factory);
    const int32 count = factory->countClasses();
    for (int32 i = 0; i < count; ++i) {
        PClassInfo info{};
        if (factory->getClassInfo(i, &info) != kResultOk) continue;
        if (std::strncmp(info.category, kVstAudioEffectClass, PClassInfo::kCategorySize) != 0) continue;

        // Instrument oder Effekt? Steht in den Unterkategorien, z. B. "Instrument|Synth".
        bool instrument = false;
        if (factory2) {
            PClassInfo2 info2{};
            if (factory2->getClassInfo2(i, &info2) == kResultOk) {
                info2.subCategories[PClassInfo2::kSubCategoriesSize - 1] = 0;
                instrument = std::strstr(info2.subCategories, "Instrument") != nullptr;
            }
        }
        info.name[PClassInfo::kNameSize - 1] = 0;
        std::wstring name = widen(info.name);
        if (name.empty()) name = path.stem().wstring();
        return Vst3Plugin::create(factory, info.cid, instrument, std::move(name), sampleRate, maxFrames, error);
    }
    error = "Die Datei enthaelt kein Audio-Plugin";
    return nullptr;
}

}  // namespace vst3
