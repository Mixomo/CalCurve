#pragma once

#include <JuceHeader.h>
#include "CurveFIR.h"

class CalCurveAudioProcessor : public juce::AudioProcessor,
                               private juce::AudioProcessorValueTreeState::Listener,
                               private juce::AsyncUpdater
{
public:
    CalCurveAudioProcessor();
    ~CalCurveAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "CalCurve"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    juce::AudioProcessorValueTreeState parameters;

    void loadImpulseResponse (const juce::File& file);
    void loadCorrectionCurve (const juce::File& file);
    bool savePresetToFile (const juce::File& file, const juce::String& presetName);
    bool loadPresetFromFile (const juce::File& file);
    juce::String getLoadedName() const;
    juce::String getPresetName() const;
    int getActiveLatencySamples() const noexcept { return activeLatencySamples; }
    int getActiveImpulsePeakSamples() const noexcept { return activeImpulsePeakSamples; }
    double getCurrentSampleRate() const noexcept { return currentSampleRate; }
    std::vector<CurvePoint> getCorrectionCurve() const;
    juce::File getPresetsDirectory() const
    {
        auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                       .getChildFile ("Mixomo")
                       .getChildFile ("CalCurve")
                       .getChildFile ("UserPresets");
        dir.createDirectory();
        return dir;
    }

private:
    enum class PhaseMode
    {
        minimum = 0,
        natural = 1,
        linear = 2
    };

    void parameterChanged (const juce::String& parameterID, float newValue) override;
    void handleAsyncUpdate() override;
    PhaseMode getPhaseMode() const;
    juce::AudioBuffer<float> createImpulseForPhaseMode (const std::vector<CurvePoint>& points,
                                                        double sampleRate,
                                                        int taps) const;
    void updateWetAutoGain (const std::vector<CurvePoint>& points, double sampleRate);
    void applyCrossfeed (juce::AudioBuffer<float>& buffer, float amount);
    void resetCrossfeed();
    void rebuildCurveFIR();
    int findImpulseResponsePeak (const juce::AudioBuffer<float>& impulse);
    int getLatencyForPhaseMode (PhaseMode mode, int taps) const;
    int getDefaultFirTapsForMode (PhaseMode mode) const;
    void applyActiveLatency (int latencySamples);

    juce::dsp::Convolution convolution;
    float limiterGain = 1.0f;
    juce::AudioBuffer<float> wetBuffer;
    juce::AudioBuffer<float> dryDelayBuffer;
    int dryDelayWriteIndex = 0;
    int activeLatencySamples = 0;
    int activeImpulsePeakSamples = 0;

    juce::String loadedName = "No FIR loaded";
    juce::String presetName = "Untitled";
    juce::File loadedCurveFile;
    std::vector<CurvePoint> correctionCurve;
    mutable juce::CriticalSection curveLock;
    double currentSampleRate = 48000.0;
    float wetAutoGain = 1.0f;
    float wetAutoGainDb = 0.0f;
    bool hasImpulse = false;

    // Transparent crossfeed components
    juce::AudioBuffer<float> crossfeedDelay;
    int crossfeedWrite = 0;
    float lpL = 0.0f;
    float lpR = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CalCurveAudioProcessor)
};
