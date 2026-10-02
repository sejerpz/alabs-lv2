// BlipMachine - DPF wrapper
#include "DistrhoPlugin.hpp"
#include "Engine.hpp"

START_NAMESPACE_DISTRHO

using namespace blipmachine;

class BlipMachinePlugin : public Plugin {
public:
    BlipMachinePlugin() : Plugin(kParamCount, 0, 0)
    {
        engine.init(getSampleRate());
    }

protected:
    const char* getLabel() const override       { return "BlipMachine"; }
    const char* getDescription() const override { return "5-voice synthesized percussion module, triggered by MIDI note"; }
    const char* getMaker() const override       { return "Andrea"; }
    const char* getLicense() const override     { return "ISC"; }
    uint32_t getVersion() const override        { return d_version(0, 1, 0); }

    void initAudioPort(bool input, uint32_t index, AudioPort& port) override
    {
        port.groupId = kPortGroupStereo;
        Plugin::initAudioPort(input, index, port);
        port.name   = index == 0 ? "Out L" : "Out R";
        port.symbol = index == 0 ? "out_l" : "out_r";
    }

    void initPortGroup(uint32_t groupId, PortGroup& portGroup) override
    {
        switch (groupId)
        {
        case kGroupGlobal: portGroup.name = "Global"; portGroup.symbol = "global"; break;
        case kGroupVoice:  portGroup.name = "Voice";  portGroup.symbol = "voice";  break;
        default:           Plugin::initPortGroup(groupId, portGroup);            break;
        }
    }

    void initParameter(uint32_t index, Parameter& parameter) override
    {
        char sym[32], name[32];
        ParamDef d;
        getParamDef(index, d, sym, name);

        parameter.hints = kParameterIsAutomatable;
        if (d.flags & kPFInteger) parameter.hints |= kParameterIsInteger;
        if (d.flags & kPFBoolean) parameter.hints |= kParameterIsBoolean;
        if (d.flags & kPFLog)     parameter.hints |= kParameterIsLogarithmic;

        parameter.symbol     = d.symbol;
        parameter.name       = d.name;
        parameter.shortName  = d.name;
        parameter.unit       = d.unit;
        parameter.ranges.min = d.min;
        parameter.ranges.max = d.max;
        parameter.ranges.def = d.def;
        parameter.groupId    = d.group;
    }

    float getParameterValue(uint32_t index) const override { return index < kParamCount ? engine.params[index] : 0.f; }
    void setParameterValue(uint32_t index, float value) override { engine.setParam(index, value); }

    void sampleRateChanged(double newSampleRate) override { engine.init(newSampleRate); }

    void run(const float**, float** outputs, uint32_t frames, const MidiEvent* midiEvents, uint32_t midiEventCount) override
    {
        float* outL = outputs[0];
        float* outR = outputs[1];
        uint32_t done = 0;

        for (uint32_t e = 0; e < midiEventCount; ++e) {
            const MidiEvent& ev = midiEvents[e];
            if (ev.frame > done) {
                engine.process(outL + done, outR + done, ev.frame - done);
                done = ev.frame;
            }
            if (ev.size < 2) continue;
            const uint8_t status = ev.data[0] & 0xF0;
            const uint8_t note   = ev.data[1];
            const uint8_t vel    = ev.size >= 3 ? ev.data[2] : 0;
            if (status == 0x90 && vel > 0) engine.noteOn(note, vel);
            else if (status == 0x80 || status == 0x90) engine.noteOff(note);
        }
        if (done < frames) engine.process(outL + done, outR + done, frames - done);
    }

private:
    Engine engine;

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BlipMachinePlugin)
};

Plugin* createPlugin() { return new BlipMachinePlugin(); }

END_NAMESPACE_DISTRHO
