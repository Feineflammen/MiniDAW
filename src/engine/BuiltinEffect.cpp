#include "engine/BuiltinEffect.h"

#include "engine/Effects.h"
#include "engine/Mastering.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace fx {

float clampValue(const ParamInfo& p, float value) noexcept {
    if (!std::isfinite(value)) return p.def;
    if (p.toggle) return value >= 0.5f ? 1.0f : 0.0f;
    if (p.choiceCount > 0) return std::clamp(std::round(value), 0.0f, static_cast<float>(p.choiceCount - 1));
    return std::clamp(value, p.min, p.max);
}

float toPosition(const ParamInfo& p, float value) noexcept {
    value = clampValue(p, value);
    if (p.toggle) return value;
    if (p.choiceCount > 1) return value / static_cast<float>(p.choiceCount - 1);
    if (p.logScale) return static_cast<float>(std::log(value / p.min) / std::log(p.max / p.min));
    return (value - p.min) / (p.max - p.min);
}

float fromPosition(const ParamInfo& p, float position) noexcept {
    position = std::clamp(position, 0.0f, 1.0f);
    if (p.toggle) return position >= 0.5f ? 1.0f : 0.0f;
    if (p.choiceCount > 1) return std::round(position * static_cast<float>(p.choiceCount - 1));
    if (p.logScale) return static_cast<float>(p.min * std::pow(p.max / p.min, static_cast<double>(position)));
    return p.min + (p.max - p.min) * position;
}

BuiltinEffect::BuiltinEffect(const char* id, const wchar_t* name, const ParamInfo* params, int count,
                             double sampleRate)
    : sampleRate_(std::clamp(std::isfinite(sampleRate) ? sampleRate : 48000.0, 8000.0, 384000.0)),
      id_(id),
      name_(name),
      params_(params),
      count_(count),
      values_(new std::atomic<float>[static_cast<std::size_t>(count)]),
      current_(static_cast<std::size_t>(count)) {
    for (int i = 0; i < count; ++i) {
        values_[static_cast<std::size_t>(i)].store(params[i].def, std::memory_order_relaxed);
        current_[static_cast<std::size_t>(i)] = params[i].def;
    }
}

float BuiltinEffect::param(int param) const noexcept {
    if (param < 0 || param >= count_) return 0.0f;
    return values_[static_cast<std::size_t>(param)].load(std::memory_order_relaxed);
}

void BuiltinEffect::setParam(int param, float value) noexcept {
    if (param < 0 || param >= count_ || !std::isfinite(value)) return;
    values_[static_cast<std::size_t>(param)].store(clampValue(params_[param], value), std::memory_order_relaxed);
}

bool BuiltinEffect::pullParams() noexcept {
    for (int i = 0; i < count_; ++i)
        current_[static_cast<std::size_t>(i)] = values_[static_cast<std::size_t>(i)].load(std::memory_order_relaxed);
    const bool first = first_;
    first_ = false;
    return first;
}

std::vector<char> BuiltinEffect::saveState() const {
    std::string text = std::string(id_) + " 1\n";
    for (int i = 0; i < count_; ++i) {
        char line[96];
        std::snprintf(line, sizeof line, "%s=%.9g\n", params_[i].key, static_cast<double>(param(i)));
        text += line;
    }
    return std::vector<char>(text.begin(), text.end());
}

void BuiltinEffect::loadState(const std::vector<char>& state) {
    const std::string text(state.begin(), state.end());
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(start, end - start);
        start = end + 1;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;  // Kopfzeile oder Unsinn
        const std::string key = line.substr(0, eq);
        for (int i = 0; i < count_; ++i) {
            if (key != params_[i].key) continue;
            char* rest = nullptr;
            const double value = std::strtod(line.c_str() + eq + 1, &rest);
            if (rest != line.c_str() + eq + 1) setParam(i, static_cast<float>(value));
            break;
        }
    }
}

const std::vector<BuiltinType>& builtinTypes() {
    static const std::vector<BuiltinType> types{
        {"EQ", L"EQ"}, {"Kompressor", L"Kompressor"}, {"Hall", L"Hall"}, {"Echo", L"Echo"}, {"Gate", L"Gate"}, {"Chorus", L"Chorus"}, {"Mastering", L"Mastering"},
    };
    return types;
}

std::shared_ptr<BuiltinEffect> createBuiltin(const std::string& id, double sampleRate) {
    if (id == "EQ") return std::make_shared<EqEffect>(sampleRate);
    if (id == "Kompressor") return std::make_shared<CompressorEffect>(sampleRate);
    if (id == "Hall") return std::make_shared<ReverbEffect>(sampleRate);
    if (id == "Echo") return std::make_shared<DelayEffect>(sampleRate);
    if (id == "Gate") return std::make_shared<GateEffect>(sampleRate);
    if (id == "Chorus") return std::make_shared<ChorusEffect>(sampleRate);
    if (id == "Mastering") return std::make_shared<mastering::MasteringEffect>(sampleRate);
    return nullptr;
}

}  // namespace fx
