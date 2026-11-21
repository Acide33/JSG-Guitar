// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
#include "plugin.h"
#include "plugin_view.h"
#include "paths.h"

#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "public.sdk/source/vst/utility/stringconvert.h"

#include <cstring>

#ifdef _WIN32
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#else
 #include <dlfcn.h>
#endif

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace jsg::plugin
{

namespace
{
    //==========================================================================
    /** Un réglage de JSG Guitar vu par le logiciel hôte : même échelle et même affichage que l'interface. */
    class JsgParameter final : public Parameter
    {
    public:
        explicit JsgParameter (int index)
        {
            std::string name;
            p = paramInfo (index, name);
            info.id = (ParamID) index;
            StringConvert::convert (name, info.title);
            StringConvert::convert (name.size() > 20 ? name.substr (0, 20) : name, info.shortTitle);
            if (p.kind == Kind::ranged) StringConvert::convert (p.unit, info.units);
            info.stepCount = (p.kind == Kind::toggle || p.kind == Kind::choice) ? (int32) (p.max - p.min) : 0;
            info.defaultNormalizedValue = plugin::toNormalized (p, p.def);
            info.flags = ParameterInfo::kCanAutomate;
            if (p.kind == Kind::choice) info.flags |= ParameterInfo::kIsList;
            setNormalized (info.defaultNormalizedValue);
        }

        ParamValue toPlain (ParamValue n) const override       { return plugin::toPlain (p, n); }
        ParamValue toNormalized (ParamValue v) const override  { return plugin::toNormalized (p, v); }

        void toString (ParamValue n, String128 out) const override
        {
            StringConvert::convert (formatValue (p, toPlain (n)), out);
        }

        bool fromString (const TChar* text, ParamValue& n) const override
        {
            const std::string s = StringConvert::convert (text);
            if (p.kind == Kind::toggle) { n = (s == "ON" || s == "on" || s == "1") ? 1.0 : 0.0; return true; }
            if (p.kind == Kind::choice)
            {
                for (int i = 0; i < p.numChoices; ++i) if (s == p.choices[i]) { n = toNormalized (i); return true; }
                return false;
            }
            std::string t = s;
            for (auto& c : t) if (c == ',') c = '.';
            char* end = nullptr;
            double v = std::strtod (t.c_str(), &end);
            if (end == t.c_str()) return false;
            if (p.kind == Kind::position) v /= 100.0;
            n = toNormalized (v);
            return true;
        }

    private:
        ParamInfo p;
    };

    /** Dossier de l'interface : JSG Guitar.vst3/Contents/Resources/ui (ou « ui » à côté du module). */
    std::filesystem::path interfaceFolder()
    {
        std::filesystem::path module;
       #ifdef _WIN32
        HMODULE h = nullptr;
        GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR) (void*) &interfaceFolder, &h);
        wchar_t buffer[MAX_PATH * 4] {};
        GetModuleFileNameW (h, buffer, (DWORD) std::size (buffer));
        module = buffer;
       #else
        Dl_info info {};
        if (dladdr ((void*) &interfaceFolder, &info) != 0 && info.dli_fname != nullptr) module = fromUtf8 (info.dli_fname);
       #endif
        std::error_code ec;
        const auto bundled = module.parent_path().parent_path() / "Resources" / "ui";
        if (std::filesystem::exists (bundled / "index.html", ec)) return bundled;
        return module.parent_path() / "ui";
    }

    constexpr uint32 stateMagic = 0x5653474A;   // « JGSV »
}

//==============================================================================
/** Réveille le fil de l'interface du logiciel hôte pour lui passer les réglages touchés dans la fenêtre. */
class EditQueue
{
public:
    explicit EditQueue (Plugin& p) : plugin (p)
    {
       #ifdef _WIN32
        WNDCLASSW wc {};
        wc.lpfnWndProc = proc;
        wc.hInstance = GetModuleHandleW (nullptr);
        wc.lpszClassName = L"JSGGuitarPluginEdits";
        RegisterClassW (&wc);   // échoue sans conséquence si la classe existe déjà
        hwnd = CreateWindowExW (0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        SetWindowLongPtrW (hwnd, GWLP_USERDATA, (LONG_PTR) this);
        SetTimer (hwnd, 1, 60, nullptr);   // fin des gestes (potard lâché)
       #endif
    }

    ~EditQueue()
    {
       #ifdef _WIN32
        if (hwnd != nullptr) { KillTimer (hwnd, 1); DestroyWindow (hwnd); }
       #endif
    }

    void wake()
    {
       #ifdef _WIN32
        if (hwnd != nullptr) PostMessageW (hwnd, WM_APP, 0, 0);
       #else
        plugin.flushUserEdits();   // sans fenêtre (Linux, tests) : tout de suite
       #endif
    }

private:
   #ifdef _WIN32
    static LRESULT CALLBACK proc (HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_APP || msg == WM_TIMER)
            if (auto* self = (EditQueue*) GetWindowLongPtrW (h, GWLP_USERDATA)) self->plugin.flushUserEdits();
        return DefWindowProcW (h, msg, wp, lp);
    }
    HWND hwnd = nullptr;
   #endif
    Plugin& plugin;
};

//==============================================================================
Plugin::Plugin()
{
    core.setInterfaceFolder (interfaceFolder());

    core.onUserEdit = [this] (int index, float plain)
    {
        { const std::lock_guard<std::mutex> lock (editLock); pendingEdits.push_back ({ index, plain }); }
        if (queue != nullptr) queue->wake();
    };
    core.onStateChanged = [this]
    {
        { const std::lock_guard<std::mutex> lock (editLock); pendingDirty = true; }
        if (queue != nullptr) queue->wake();
    };
}

Plugin::~Plugin() = default;

tresult PLUGIN_API Plugin::initialize (FUnknown* context)
{
    const tresult result = SingleComponentEffect::initialize (context);
    if (result != kResultOk) return result;

    addAudioInput (STR16 ("Entrée"), SpeakerArr::kStereo);
    addAudioOutput (STR16 ("Sortie"), SpeakerArr::kStereo);

    for (int i = 0; i < numParams; ++i) parameters.addParameter (new JsgParameter (i));
    auto* bypassParam = new StringListParameter (STR16 ("Contourner"), bypassId, nullptr,
                                                 ParameterInfo::kCanAutomate | ParameterInfo::kIsBypass | ParameterInfo::kIsList);
    bypassParam->appendString (STR16 ("OFF"));
    bypassParam->appendString (STR16 ("ON"));
    parameters.addParameter (bypassParam);

    queue = std::make_unique<EditQueue> (*this);
    return kResultOk;
}

tresult PLUGIN_API Plugin::terminate()
{
    queue.reset();
    return SingleComponentEffect::terminate();
}

tresult PLUGIN_API Plugin::setBusArrangements (SpeakerArrangement* inputs, int32 numIns, SpeakerArrangement* outputs, int32 numOuts)
{
    // Entrée mono ou stéréo, sortie mono ou stéréo
    if (numIns != 1 || numOuts != 1) return kResultFalse;
    const auto ok = [] (SpeakerArrangement a) { return a == SpeakerArr::kMono || a == SpeakerArr::kStereo; };
    if (! ok (inputs[0]) || ! ok (outputs[0])) return kResultFalse;

    if (auto* bus = FCast<AudioBus> (audioInputs.at (0))) bus->setArrangement (inputs[0]);
    if (auto* bus = FCast<AudioBus> (audioOutputs.at (0))) bus->setArrangement (outputs[0]);
    return kResultTrue;
}

tresult PLUGIN_API Plugin::canProcessSampleSize (int32 size)
{
    return size == kSample32 ? kResultTrue : kResultFalse;
}

tresult PLUGIN_API Plugin::setupProcessing (ProcessSetup& setup)
{
    const tresult r = SingleComponentEffect::setupProcessing (setup);
    core.prepare (setup.sampleRate);
    return r;
}

tresult PLUGIN_API Plugin::setActive (TBool state)
{
    if (state) core.prepare (processSetup.sampleRate);   // repart d'un moteur propre (queues de réverbe effacées)
    return kResultOk;
}

tresult PLUGIN_API Plugin::process (ProcessData& data)
{
    // Réglages envoyés par le logiciel hôte (automation) : la dernière valeur du bloc
    if (auto* changes = data.inputParameterChanges)
    {
        const int32 count = changes->getParameterCount();
        for (int32 i = 0; i < count; ++i)
        {
            auto* q = changes->getParameterData (i);
            if (q == nullptr || q->getPointCount() <= 0) continue;
            int32 offset = 0;
            ParamValue value = 0;
            if (q->getPoint (q->getPointCount() - 1, offset, value) != kResultTrue) continue;
            const ParamID id = q->getParameterId();
            if (id == (ParamID) bypassId) core.setBypass (value > 0.5);
            else if (id < (ParamID) numParams)
            {
                std::string name;
                core.setFromHost ((int) id, (float) toPlain (paramInfo ((int) id, name), value));
            }
        }
    }

    if (data.numSamples <= 0 || data.numOutputs < 1 || data.outputs[0].numChannels < 1) return kResultOk;

    const float* const* in = nullptr;
    int numIn = 0;
    if (data.numInputs >= 1 && data.inputs[0].numChannels >= 1 && data.inputs[0].channelBuffers32 != nullptr)
    {
        in = data.inputs[0].channelBuffers32;
        numIn = data.inputs[0].numChannels;
    }
    core.process (in, numIn, data.outputs[0].channelBuffers32, data.outputs[0].numChannels, data.numSamples);
    data.outputs[0].silenceFlags = 0;
    return kResultOk;
}

//==============================================================================
// Projet : « JGSV », longueur, puis l'état en JSON
tresult PLUGIN_API Plugin::getState (IBStream* state)
{
    if (state == nullptr) return kInvalidArgument;
    const std::string json = core.getState();
    IBStreamer s (state, kLittleEndian);
    if (! s.writeInt32u (stateMagic) || ! s.writeInt32u ((uint32) json.size())) return kResultFalse;
    int32 written = 0;
    return state->write ((void*) json.data(), (int32) json.size(), &written) == kResultOk && written == (int32) json.size() ? kResultOk : kResultFalse;
}

tresult PLUGIN_API Plugin::setState (IBStream* state)
{
    if (state == nullptr) return kInvalidArgument;
    IBStreamer s (state, kLittleEndian);
    uint32 magic = 0, size = 0;
    if (! s.readInt32u (magic) || magic != stateMagic || ! s.readInt32u (size) || size > 64u * 1024u * 1024u) return kResultFalse;
    std::string json (size, '\0');
    int32 read = 0;
    if (size > 0 && (state->read (json.data(), (int32) size, &read) != kResultOk || read != (int32) size)) return kResultFalse;
    if (! core.setState (json)) return kResultFalse;
    syncParametersFromCore();
    return kResultOk;
}

/** Valeurs des réglages côté logiciel hôte = celles du cœur (après un rappel du projet). */
void Plugin::syncParametersFromCore()
{
    for (int i = 0; i < numParams; ++i)
        if (auto* p = parameters.getParameter ((ParamID) i))
            p->setNormalized (p->toNormalized (core.value (i)));
    if (auto* p = parameters.getParameter ((ParamID) bypassId)) p->setNormalized (core.isBypassed() ? 1.0 : 0.0);
    if (componentHandler != nullptr) componentHandler->restartComponent (kParamValuesChanged);
}

tresult PLUGIN_API Plugin::setParamNormalized (ParamID tag, ParamValue value)
{
    const tresult r = SingleComponentEffect::setParamNormalized (tag, value);
    if (tag < (ParamID) numParams)
    {
        std::string name;
        core.setFromHost ((int) tag, (float) toPlain (paramInfo ((int) tag, name), value));
    }
    else if (tag == (ParamID) bypassId) core.setBypass (value > 0.5);
    return r;
}

IPlugView* PLUGIN_API Plugin::createView (FIDString name)
{
    if (name == nullptr || std::strcmp (name, ViewType::kEditor) != 0) return nullptr;
    return createEditorView (*this);
}

//==============================================================================
/** Fil de l'interface du logiciel hôte : réglages touchés dans la fenêtre -> logiciel hôte (automation, annuler). */
void Plugin::flushUserEdits()
{
    std::vector<Edit> edits;
    bool dirty;
    {
        const std::lock_guard<std::mutex> lock (editLock);
        edits.swap (pendingEdits);
        dirty = pendingDirty;
        pendingDirty = false;
    }

    const auto now = std::chrono::steady_clock::now();
    for (const auto& e : edits)
    {
        auto* p = parameters.getParameter ((ParamID) e.index);
        if (p == nullptr) continue;
        const ParamValue n = p->toNormalized (e.plain);
        p->setNormalized (n);
        if (! editing[(size_t) e.index]) { beginEdit ((ParamID) e.index); editing[(size_t) e.index] = true; }
        performEdit ((ParamID) e.index, n);
        lastEdit[(size_t) e.index] = now;
    }

    // Geste fini (potard lâché depuis un quart de seconde) : une seule étape d'annulation par geste.
    // Sans minuterie (hors Windows), le geste se termine tout de suite.
   #ifdef _WIN32
    const auto gestureEnd = std::chrono::milliseconds (250);
   #else
    const auto gestureEnd = std::chrono::milliseconds (0);
   #endif
    for (int i = 0; i < numParams; ++i)
        if (editing[(size_t) i] && now - lastEdit[(size_t) i] >= gestureEnd)
        {
            endEdit ((ParamID) i);
            editing[(size_t) i] = false;
        }

    if (dirty && componentHandler != nullptr)
    {
        IComponentHandler2* handler2 = nullptr;
        if (componentHandler->queryInterface (IComponentHandler2::iid, (void**) &handler2) == kResultTrue && handler2 != nullptr)
        {
            handler2->setDirty (true);
            handler2->release();
        }
    }
}

} // namespace jsg::plugin
