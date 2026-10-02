// Euclides - DPF wrapper
#include "DistrhoPlugin.hpp"
#include "Engine.hpp"

START_NAMESPACE_DISTRHO

using namespace euclides;

class EuclidesPlugin : public Plugin, private MidiSink {
public:
    EuclidesPlugin() : Plugin(kParamCount, 0, 0)
    {
        engine.init(getSampleRate());
    }

protected:
    const char* getLabel() const override       { return "Euclides"; }
    const char* getDescription() const override { return "Euclidean MIDI sequencer, 5 voices, host- or internal-clocked"; }
    const char* getMaker() const override       { return "Andrea"; }
    const char* getLicense() const override     { return "ISC"; }
    uint32_t getVersion() const override        { return d_version(0, 3, 0); }

    void initPortGroup(uint32_t groupId, PortGroup& portGroup) override
    {
        switch (groupId)
        {
        case kGroupGlobal:    portGroup.name = "Global";    portGroup.symbol = "global";    break;
        case kGroupSequencer: portGroup.name = "Sequencer"; portGroup.symbol = "sequencer"; break;
        default:              Plugin::initPortGroup(groupId, portGroup);                  break;
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

        if (d.enumLabels != nullptr && d.enumCount > 0) {
            ParameterEnumerationValue* const values = new ParameterEnumerationValue[d.enumCount];
            for (uint32_t i = 0; i < d.enumCount; ++i) {
                values[i].value = d.min + (float)i;
                values[i].label = d.enumLabels[i];
            }
            parameter.enumValues.count = (uint8_t)d.enumCount;
            parameter.enumValues.restrictedMode = true;
            parameter.enumValues.values = values;
        }
    }

    float getParameterValue(uint32_t index) const override { return index < kParamCount ? engine.params[index] : 0.f; }
    void setParameterValue(uint32_t index, float value) override { engine.setParam(index, value); }

    void sampleRateChanged(double newSampleRate) override { engine.init(newSampleRate); }

    void run(const float**, float**, uint32_t frames) override
    {
        const TimePosition& tp = getTimePosition();
        HostTransport host;
        host.valid    = true;
        host.playing  = tp.playing;
        host.framePos = (double)tp.frame;
        host.bbtValid = tp.bbt.valid;
        if (tp.bbt.valid) {
            host.bpm      = tp.bbt.beatsPerMinute;
            host.beatType = tp.bbt.beatType;
            const double tpb = tp.bbt.ticksPerBeat > 0.0 ? tp.bbt.ticksPerBeat : 1920.0;
            host.beatPos = (double)(tp.bbt.bar - 1) * tp.bbt.beatsPerBar
                         + (double)(tp.bbt.beat - 1)
                         + tp.bbt.tick / tpb;
        }

        engine.process(frames, host, *this);
    }

private:
    Engine engine;

    void midi(uint32_t frame, uint8_t b0, uint8_t b1, uint8_t b2) override
    {
        MidiEvent ev;
        ev.frame   = frame;
        ev.size    = 3;
        ev.data[0] = b0; ev.data[1] = b1; ev.data[2] = b2; ev.data[3] = 0;
        ev.dataExt = nullptr;
        writeMidiEvent(ev);
    }

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EuclidesPlugin)
};

Plugin* createPlugin() { return new EuclidesPlugin(); }

END_NAMESPACE_DISTRHO
