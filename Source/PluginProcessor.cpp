#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "CurveFIR.h"

namespace
{
    constexpr double referenceSampleRate = 44100.0;
    constexpr int minimumPhaseReferenceTaps = 4096;
    constexpr int naturalPhaseReferenceTaps = 4096;
    constexpr int linearPhaseReferenceTaps = 8192;
    constexpr int naturalReferenceLatencySamples = 1024;
    constexpr int maxFirTaps = 65536;
}

CalCurveAudioProcessor::CalCurveAudioProcessor()
    : AudioProcessor (BusesProperties()
        .withInput ("Input", juce::AudioChannelSet::stereo(), true)
        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "Parameters", createParameterLayout())
{
    parameters.addParameterListener ("phasemode", this);
}

CalCurveAudioProcessor::~CalCurveAudioProcessor()
{
    parameters.removeParameterListener ("phasemode", this);
}

juce::AudioProcessorValueTreeState::ParameterLayout CalCurveAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "drywet", "Dry/Wet", juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 1.0f,
        juce::String(),
        juce::AudioProcessorParameter::genericParameter,
        [] (float val, int) { return juce::String (juce::roundToInt (val * 100.0f)) + " %"; },
        [] (const juce::String& text) { return static_cast<float> (text.getDoubleValue() / 100.0); }));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "crossfeed", "Crossfeed", juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f,
        juce::String(),
        juce::AudioProcessorParameter::genericParameter,
        [] (float val, int) { return juce::String (juce::roundToInt (val * 100.0f)) + " %"; },
        [] (const juce::String& text) { return static_cast<float> (text.getDoubleValue() / 100.0); }));
    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        "crossfeedalgorithm", "Crossfeed Algorithm", juce::StringArray { "Natural", "BS2B" }, 0));
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "crossfeedcircumference", "Crossfeed Head Circumference",
        juce::NormalisableRange<float> (45.0f, 70.0f, 0.1f), 57.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "crossfeedheadwidth", "Crossfeed Head Width",
        juce::NormalisableRange<float> (10.0f, 22.0f, 0.1f), 15.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "crossfeedheadlength", "Crossfeed Head Length",
        juce::NormalisableRange<float> (14.0f, 25.0f, 0.1f), 19.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "crossfeedangle", "Crossfeed Speaker Angle",
        juce::NormalisableRange<float> (10.0f, 90.0f, 0.1f), 60.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "crossfeedcutoff", "Crossfeed Cutoff",
        juce::NormalisableRange<float> (200.0f, 2500.0f, 1.0f), 700.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "crossfeeddirect", "Crossfeed Direct",
        juce::NormalisableRange<float> (50.0f, 120.0f, 0.1f), 100.0f));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "gain", "Gain", juce::NormalisableRange<float> (-24.0f, 12.0f, 0.01f), 0.0f));

    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        "phasemode", "Phase Mode", juce::StringArray { "Minimum", "Natural", "Linear" }, 1));

    params.push_back (std::make_unique<juce::AudioParameterBool> (
        "limiter", "Limiter", false));

    params.push_back (std::make_unique<juce::AudioParameterBool> (
        "bypass", "Bypass", false));

    return { params.begin(), params.end() };
}

void CalCurveAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    const auto previousSampleRate = currentSampleRate;
    currentSampleRate = sampleRate;

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32> (samplesPerBlock);
    spec.numChannels = static_cast<juce::uint32> (getTotalNumOutputChannels());

    convolution.prepare (spec);
    limiterGain = 1.0f;
    
    wetBuffer.setSize (getTotalNumOutputChannels(), samplesPerBlock);
    
    // Extra delay headroom for geometry-based crossfeed at high sample rates.
    crossfeedDelay.setSize (2, juce::jmax (32, static_cast<int> (std::ceil (sampleRate * 0.003))));
    resetCrossfeed();

    dryDelayBuffer.setSize (getTotalNumOutputChannels(), 131072);
    dryDelayBuffer.clear();
    dryDelayWriteIndex = 0;

    if (! juce::approximatelyEqual (previousSampleRate, currentSampleRate))
        rebuildFromStoredCurve();
}

bool CalCurveAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainInputChannelSet() != layouts.getMainOutputChannelSet())
        return false;

    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    return true;
}

juce::AudioProcessorEditor* CalCurveAudioProcessor::createEditor()
{
    return new CalCurveAudioProcessorEditor (*this);
}

void CalCurveAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const auto totalInputChannels = getTotalNumInputChannels();
    const auto totalOutputChannels = getTotalNumOutputChannels();

    for (auto i = totalInputChannels; i < totalOutputChannels; ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    const auto dryWet = parameters.getRawParameterValue ("drywet")->load();
    const auto crossfeed = parameters.getRawParameterValue ("crossfeed")->load();
    const auto gain = juce::Decibels::decibelsToGain (parameters.getRawParameterValue ("gain")->load());
    const auto limiterEnabled = parameters.getRawParameterValue ("limiter")->load() > 0.5f;
    const auto bypassed = parameters.getRawParameterValue ("bypass")->load() > 0.5f;

    // If bypassed, we just pass the input delayed by activeLatencySamples to match latency
    if (bypassed)
    {
        if (activeLatencySamples > 0 && dryDelayBuffer.getNumSamples() > 0)
        {
            const auto numSamples = buffer.getNumSamples();
            const auto numChannels = buffer.getNumChannels();

            for (int channel = 0; channel < numChannels; ++channel)
            {
                auto* channelData = buffer.getWritePointer (channel);
                auto* delayData = dryDelayBuffer.getWritePointer (channel);

                int writeIdx = dryDelayWriteIndex;
                for (int sample = 0; sample < numSamples; ++sample)
                {
                    float drySample = channelData[sample];
                    delayData[writeIdx] = drySample;

                    const auto delaySize = dryDelayBuffer.getNumSamples();
                    int readIdx = (writeIdx - activeLatencySamples + delaySize) % delaySize;
                    channelData[sample] = delayData[readIdx];

                    writeIdx = (writeIdx + 1) % delaySize;
                }
            }
            dryDelayWriteIndex = (dryDelayWriteIndex + numSamples) % dryDelayBuffer.getNumSamples();
        }
        return;
    }

    const bool shouldConvolve = hasImpulse && dryWet > 0.0f;

    if (shouldConvolve)
    {
        wetBuffer.makeCopyOf (buffer, true);

        juce::dsp::AudioBlock<float> wetBlock (wetBuffer);
        juce::dsp::ProcessContextReplacing<float> context (wetBlock);
        convolution.process (context);
        wetBuffer.applyGain (wetAutoGain);
    }

    // Delay the input buffer (dry path) to match convolution group delay
    if (activeLatencySamples > 0 && dryDelayBuffer.getNumSamples() > 0)
    {
        const auto numSamples = buffer.getNumSamples();
        const auto numChannels = buffer.getNumChannels();

        for (int channel = 0; channel < numChannels; ++channel)
        {
            auto* channelData = buffer.getWritePointer (channel);
            auto* delayData = dryDelayBuffer.getWritePointer (channel);

            int writeIdx = dryDelayWriteIndex;
            for (int sample = 0; sample < numSamples; ++sample)
            {
                float drySample = channelData[sample];
                delayData[writeIdx] = drySample;

                const auto delaySize = dryDelayBuffer.getNumSamples();
                int readIdx = (writeIdx - activeLatencySamples + delaySize) % delaySize;
                channelData[sample] = delayData[readIdx];

                writeIdx = (writeIdx + 1) % delaySize;
            }
        }
        dryDelayWriteIndex = (dryDelayWriteIndex + numSamples) % dryDelayBuffer.getNumSamples();
    }

    // Mix dry and wet if convolution was run
    if (shouldConvolve)
    {
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            auto* dry = buffer.getWritePointer (channel);
            auto* wet = wetBuffer.getReadPointer (channel);

            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                dry[sample] = dry[sample] + dryWet * (wet[sample] - dry[sample]);
        }
    }

    if (crossfeed > 0.0f)
        applyCrossfeed (buffer, crossfeed);

    buffer.applyGain (gain);

    if (limiterEnabled)
    {
        float peak = 0.0f;
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            peak = juce::jmax (peak, buffer.getMagnitude (channel, 0, buffer.getNumSamples()));

        float targetLimiterGain = 1.0f;
        if (peak > 1.0f)
            targetLimiterGain = 1.0f / peak;

        // Smoothly adjust the limiter gain
        const double releaseTimeMs = 150.0;
        const double numSamples = buffer.getNumSamples();
        const double sr = currentSampleRate > 0.0 ? currentSampleRate : 48000.0;
        
        // Attack is instant (so we catch peaks immediately), release is smooth!
        if (targetLimiterGain < limiterGain)
        {
            limiterGain = targetLimiterGain;
        }
        else if (limiterGain < 1.0f)
        {
            const double releaseCoef = std::exp (-1.0 / (sr * (releaseTimeMs / 1000.0)));
            limiterGain = static_cast<float> (targetLimiterGain + (limiterGain - targetLimiterGain) * std::pow (releaseCoef, numSamples));
        }
        else
        {
            limiterGain = 1.0f;
        }

        // Apply the smoothed gain
        if (limiterGain < 1.0f)
            buffer.applyGain (limiterGain);

        // Finally, hard clip to guarantee absolute safety (last bastion!)
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            auto* channelData = buffer.getWritePointer (channel);
            for (int s = 0; s < buffer.getNumSamples(); ++s)
                channelData[s] = juce::jlimit (-1.0f, 1.0f, channelData[s]);
        }
    }
}

void CalCurveAudioProcessor::loadImpulseResponse (const juce::File& file)
{
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    if (auto reader = std::unique_ptr<juce::AudioFormatReader> (formatManager.createReaderFor (file)))
    {
        juce::AudioBuffer<float> impulse (reader->numChannels, static_cast<int> (reader->lengthInSamples));
        impulse.clear();
        reader->read (&impulse, 0, impulse.getNumSamples(), 0, true, true);

        // 1. Calculate and store correction curve
        {
            const juce::ScopedLock lock (curveLock);
            correctionCurve = CurveFIR::createMagnitudeCurveFromImpulse (impulse, reader->sampleRate);
        }
        const auto synthesisSampleRate = currentSampleRate > 0.0 ? currentSampleRate : reader->sampleRate;
        updateWetAutoGain (correctionCurve, synthesisSampleRate);

        const auto phaseMode = getPhaseMode();
        const auto taps = juce::jlimit (256, maxFirTaps, juce::jmax (getDefaultFirTapsForMode (phaseMode), scaleReferenceSampleCount (impulse.getNumSamples())));
        auto monoImpulse = createImpulseForPhaseMode (correctionCurve, synthesisSampleRate, taps);

        impulse.setSize (static_cast<int> (reader->numChannels), taps);
        for (int ch = 0; ch < static_cast<int> (reader->numChannels); ++ch)
            impulse.copyFrom (ch, 0, monoImpulse, 0, 0, taps);

        activeImpulsePeakSamples = findImpulseResponsePeak (impulse);
        applyActiveLatency (getLatencyForPhaseMode (phaseMode, impulse.getNumSamples()));

        convolution.loadImpulseResponse (std::move (impulse),
                                         synthesisSampleRate,
                                         juce::dsp::Convolution::Stereo::yes,
                                         juce::dsp::Convolution::Trim::no,
                                         juce::dsp::Convolution::Normalise::no);
    }

    loadedName = file.getFileName();
    loadedCurveFile = file; // Keep WAV file path to support runtime rebuilds!
    hasImpulse = true;
}

void CalCurveAudioProcessor::loadCorrectionCurve (const juce::File& file)
{
    auto points = CurveFIR::parseCurveFile (file);
    updateWetAutoGain (points, currentSampleRate);
    const auto phaseMode = getPhaseMode();
    const auto firTaps = getDefaultFirTapsForMode (phaseMode);
    auto impulse = createImpulseForPhaseMode (points, currentSampleRate, firTaps);

    activeImpulsePeakSamples = findImpulseResponsePeak (impulse);
    applyActiveLatency (getLatencyForPhaseMode (phaseMode, firTaps));

    convolution.loadImpulseResponse (std::move (impulse),
                                     currentSampleRate,
                                     juce::dsp::Convolution::Stereo::no,
                                     juce::dsp::Convolution::Trim::no,
                                     juce::dsp::Convolution::Normalise::no);
    loadedName = file.getFileName() + " (curve FIR)";
    loadedCurveFile = file;
    hasImpulse = true;

    const juce::ScopedLock lock (curveLock);
    correctionCurve = std::move (points);
}

bool CalCurveAudioProcessor::exportCurrentFirToFile (const juce::File& file)
{
    std::vector<CurvePoint> points;
    {
        const juce::ScopedLock lock (curveLock);
        points = correctionCurve;
    }

    if (points.empty())
        return false;

    const auto sampleRate = currentSampleRate > 0.0 ? currentSampleRate : referenceSampleRate;
    const auto phaseMode = getPhaseMode();
    const auto taps = getDefaultFirTapsForMode (phaseMode);
    auto impulse = createImpulseForPhaseMode (points, sampleRate, taps);

    if (impulse.getNumSamples() <= 0)
        return false;

    juce::WavAudioFormat wavFormat;

    if (file.existsAsFile() && ! file.deleteFile())
        return false;

    std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
    if (stream == nullptr || ! stream->openedOk())
        return false;

    std::unique_ptr<juce::AudioFormatWriter> writer (wavFormat.createWriterFor (stream.get(),
                                                                                 sampleRate,
                                                                                 1,
                                                                                 32,
                                                                                 {},
                                                                                 0));
    if (writer == nullptr)
        return false;

    stream.release();
    return writer->writeFromAudioSampleBuffer (impulse, 0, impulse.getNumSamples());
}

int CalCurveAudioProcessor::findImpulseResponsePeak (const juce::AudioBuffer<float>& impulse)
{
    if (impulse.getNumSamples() == 0)
        return 0;

    int peakIndex = 0;
    float maxVal = -1.0f;

    const auto* samples = impulse.getReadPointer (0);
    for (int i = 0; i < impulse.getNumSamples(); ++i)
    {
        float val = std::abs (samples[i]);
        if (val > maxVal)
        {
            maxVal = val;
            peakIndex = i;
        }
    }

    return peakIndex;
}

juce::String CalCurveAudioProcessor::getLoadedName() const
{
    return loadedName;
}

juce::String CalCurveAudioProcessor::getPresetName() const
{
    return presetName;
}

std::vector<CurvePoint> CalCurveAudioProcessor::getCorrectionCurve() const
{
    const juce::ScopedLock lock (curveLock);
    return correctionCurve;
}

bool CalCurveAudioProcessor::savePresetToFile (const juce::File& file, const juce::String& newPresetName)
{
    auto state = parameters.copyState();
    state.setProperty ("presetName", newPresetName, nullptr);
    state.setProperty ("loadedFilePath", loadedCurveFile.getFullPathName(), nullptr);
    state.setProperty ("loadedName", loadedName, nullptr);
    addStoredCurveToState (state);

    std::unique_ptr<juce::XmlElement> xml (state.createXml());

    if (xml == nullptr)
        return false;

    presetName = newPresetName;
    return xml->writeTo (file);
}

bool CalCurveAudioProcessor::loadPresetFromFile (const juce::File& file)
{
    std::unique_ptr<juce::XmlElement> xml (juce::XmlDocument::parse (file));

    if (xml == nullptr || ! xml->hasTagName (parameters.state.getType()))
        return false;

    auto state = juce::ValueTree::fromXml (*xml);
    const auto path = state.getProperty ("loadedFilePath").toString();
    presetName = state.getProperty ("presetName", file.getFileNameWithoutExtension()).toString();
    const auto storedLoadedName = state.getProperty ("loadedName", presetName).toString();

    parameters.replaceState (state);

    bool restoredFromFile = false;
    if (path.isNotEmpty())
    {
        const juce::File calibrationFile (path);

        if (calibrationFile.existsAsFile())
        {
            if (calibrationFile.hasFileExtension ("wav"))
                loadImpulseResponse (calibrationFile);
            else
                loadCorrectionCurve (calibrationFile);

            restoredFromFile = true;
        }
        else
            loadedCurveFile = calibrationFile;
    }

    if (! restoredFromFile && ! restoreStoredCurveFromState (state, storedLoadedName))
    {
        loadedName = path.isNotEmpty() ? "Missing: " + juce::File (path).getFileName() : storedLoadedName;
        hasImpulse = false;
    }

    return true;
}

CalCurveAudioProcessor::PhaseMode CalCurveAudioProcessor::getPhaseMode() const
{
    const auto value = parameters.getRawParameterValue ("phasemode")->load();
    const auto index = juce::roundToInt (value);

    if (index <= 0)
        return PhaseMode::minimum;

    if (index >= 2)
        return PhaseMode::linear;

    return PhaseMode::natural;
}

juce::AudioBuffer<float> CalCurveAudioProcessor::createImpulseForPhaseMode (const std::vector<CurvePoint>& points,
                                                                            double sampleRate,
                                                                            int taps) const
{
    switch (getPhaseMode())
    {
        case PhaseMode::minimum:
            return CurveFIR::createMinimumPhaseFIR (points, sampleRate, taps);

        case PhaseMode::natural:
            return CurveFIR::createMixedPhaseFIR (points, sampleRate, taps, 0.72f, juce::jlimit (0, taps - 1, scaleReferenceSampleCount (naturalReferenceLatencySamples)));

        case PhaseMode::linear:
        default:
            return CurveFIR::createLinearPhaseFIR (points, sampleRate, taps);
    }
}

int CalCurveAudioProcessor::getDefaultFirTapsForMode (PhaseMode mode) const
{
    switch (mode)
    {
        case PhaseMode::minimum: return scaleReferenceSampleCount (minimumPhaseReferenceTaps);
        case PhaseMode::natural: return scaleReferenceSampleCount (naturalPhaseReferenceTaps);
        case PhaseMode::linear:  return scaleReferenceSampleCount (linearPhaseReferenceTaps);
    }

    return scaleReferenceSampleCount (naturalPhaseReferenceTaps);
}

int CalCurveAudioProcessor::getLatencyForPhaseMode (PhaseMode mode, int taps) const
{
    switch (mode)
    {
        case PhaseMode::minimum: return 0;
        case PhaseMode::natural: return juce::jlimit (0, juce::jmax (0, taps - 1), scaleReferenceSampleCount (naturalReferenceLatencySamples));
        case PhaseMode::linear:  return juce::jmax (0, taps / 2);
    }

    return 0;
}

int CalCurveAudioProcessor::scaleReferenceSampleCount (int referenceSamples) const
{
    const auto safeSampleRate = currentSampleRate > 0.0 ? currentSampleRate : referenceSampleRate;
    auto scaled = static_cast<int> (std::round (static_cast<double> (referenceSamples) * safeSampleRate / referenceSampleRate));
    scaled = juce::jlimit (256, maxFirTaps, scaled);

    if ((scaled & 1) != 0)
        ++scaled;

    return juce::jmin (scaled, maxFirTaps);
}

void CalCurveAudioProcessor::applyActiveLatency (int latencySamples)
{
    activeLatencySamples = juce::jmax (0, latencySamples);
    setLatencySamples (activeLatencySamples);
    updateHostDisplay (juce::AudioProcessorListener::ChangeDetails()
                           .withLatencyChanged (true)
                           .withNonParameterStateChanged (true));

    if (dryDelayBuffer.getNumSamples() > 0)
        dryDelayBuffer.clear();

    dryDelayWriteIndex = 0;
}

void CalCurveAudioProcessor::updateWetAutoGain (const std::vector<CurvePoint>& points, double sampleRate)
{
    wetAutoGainDb = static_cast<float> (juce::jlimit (-18.0, 18.0, CurveFIR::calculateKWeightedGainOffset (points, sampleRate)));
    wetAutoGain = juce::Decibels::decibelsToGain (wetAutoGainDb);
}

void CalCurveAudioProcessor::parameterChanged (const juce::String& parameterID, float)
{
    if (parameterID == "phasemode")
        triggerAsyncUpdate();
}

void CalCurveAudioProcessor::handleAsyncUpdate()
{
    rebuildCurveFIR();
}

void CalCurveAudioProcessor::rebuildCurveFIR()
{
    if (loadedCurveFile.existsAsFile())
    {
        auto ext = loadedCurveFile.getFileExtension().toLowerCase();
        if (ext == ".wav")
            loadImpulseResponse (loadedCurveFile);
        else
            loadCorrectionCurve (loadedCurveFile);
    }
    else
    {
        rebuildFromStoredCurve();
    }
}

void CalCurveAudioProcessor::rebuildFromStoredCurve()
{
    std::vector<CurvePoint> points;
    {
        const juce::ScopedLock lock (curveLock);
        points = correctionCurve;
    }

    if (points.empty())
        return;

    updateWetAutoGain (points, currentSampleRate);
    const auto phaseMode = getPhaseMode();
    const auto firTaps = getDefaultFirTapsForMode (phaseMode);
    auto impulse = createImpulseForPhaseMode (points, currentSampleRate, firTaps);

    activeImpulsePeakSamples = findImpulseResponsePeak (impulse);
    applyActiveLatency (getLatencyForPhaseMode (phaseMode, firTaps));

    convolution.loadImpulseResponse (std::move (impulse),
                                     currentSampleRate,
                                     juce::dsp::Convolution::Stereo::no,
                                     juce::dsp::Convolution::Trim::no,
                                     juce::dsp::Convolution::Normalise::no);

    hasImpulse = true;
}

void CalCurveAudioProcessor::resetCrossfeed()
{
    crossfeedDelay.clear();
    crossfeedWrite = 0;
    lpL = 0.0f;
    lpR = 0.0f;
    bs2bA0Lo = 0.0;
    bs2bB1Lo = 0.0;
    bs2bA0Hi = 1.0;
    bs2bA1Hi = 0.0;
    bs2bB1Hi = 0.0;
    bs2bGain = 1.0;
    bs2bLo[0] = bs2bLo[1] = 0.0;
    bs2bHi[0] = bs2bHi[1] = 0.0;
    bs2bPrevInput[0] = bs2bPrevInput[1] = 0.0;
}

void CalCurveAudioProcessor::applyCrossfeed (juce::AudioBuffer<float>& buffer, float amount)
{
    if (buffer.getNumChannels() < 2 || crossfeedDelay.getNumSamples() == 0 || amount <= 0.001f)
        return;

    auto* left = buffer.getWritePointer (0);
    auto* right = buffer.getWritePointer (1);
    const auto algorithm = juce::roundToInt (parameters.getRawParameterValue ("crossfeedalgorithm")->load());
    const auto sampleRate = currentSampleRate > 0.0 ? currentSampleRate : 48000.0;
    const auto cutoffHz = juce::jlimit (200.0f, 2500.0f, parameters.getRawParameterValue ("crossfeedcutoff")->load());
    const auto directGain = juce::jlimit (0.5f, 1.2f, parameters.getRawParameterValue ("crossfeeddirect")->load() / 100.0f);

    if (algorithm == 1)
    {
        const auto feed = juce::jlimit (10.0, 150.0, 45.0 + static_cast<double> (amount) * 50.0) / 10.0;
        const auto fcut = juce::jlimit (300.0, 2000.0, static_cast<double> (cutoffHz));
        const auto gbLo = feed * -5.0 / 6.0 - 3.0;
        const auto gbHi = feed / 6.0 - 3.0;
        const auto gLo = juce::Decibels::decibelsToGain (gbLo);
        const auto gHi = 1.0 - juce::Decibels::decibelsToGain (gbHi);
        const auto fcHi = fcut * std::pow (2.0, (gbLo - 20.0 * std::log10 (juce::jmax (1.0e-9, gHi))) / 12.0);
        auto x = std::exp (-juce::MathConstants<double>::twoPi * fcut / sampleRate);
        bs2bB1Lo = x;
        bs2bA0Lo = gLo * (1.0 - x);
        x = std::exp (-juce::MathConstants<double>::twoPi * fcHi / sampleRate);
        bs2bB1Hi = x;
        bs2bA0Hi = 1.0 - gHi * (1.0 - x);
        bs2bA1Hi = -x;
        bs2bGain = 1.0 / (1.0 - gHi + gLo);

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            const auto inL = static_cast<double> (left[i]);
            const auto inR = static_cast<double> (right[i]);
            bs2bLo[0] = bs2bA0Lo * inL + bs2bB1Lo * bs2bLo[0];
            bs2bLo[1] = bs2bA0Lo * inR + bs2bB1Lo * bs2bLo[1];
            bs2bHi[0] = bs2bA0Hi * inL + bs2bA1Hi * bs2bPrevInput[0] + bs2bB1Hi * bs2bHi[0];
            bs2bHi[1] = bs2bA0Hi * inR + bs2bA1Hi * bs2bPrevInput[1] + bs2bB1Hi * bs2bHi[1];
            bs2bPrevInput[0] = inL;
            bs2bPrevInput[1] = inR;
            const auto wetL = (bs2bHi[0] + bs2bLo[1]) * bs2bGain * static_cast<double> (directGain);
            const auto wetR = (bs2bHi[1] + bs2bLo[0]) * bs2bGain * static_cast<double> (directGain);
            left[i] = static_cast<float> (inL + amount * (wetL - inL));
            right[i] = static_cast<float> (inR + amount * (wetR - inR));
        }
        return;
    }

    auto* delayL = crossfeedDelay.getWritePointer (0);
    auto* delayR = crossfeedDelay.getWritePointer (1);
    const auto delaySize = crossfeedDelay.getNumSamples();

    const auto circumferenceCm = juce::jlimit (45.0f, 70.0f, parameters.getRawParameterValue ("crossfeedcircumference")->load());
    const auto headWidthCm = juce::jlimit (10.0f, 22.0f, parameters.getRawParameterValue ("crossfeedheadwidth")->load());
    const auto headLengthCm = juce::jlimit (14.0f, 25.0f, parameters.getRawParameterValue ("crossfeedheadlength")->load());
    const auto angleDeg = juce::jlimit (10.0f, 90.0f, parameters.getRawParameterValue ("crossfeedangle")->load());
    const auto theta = static_cast<double> (angleDeg) * juce::MathConstants<double>::pi / 180.0;
    const auto halfHead = static_cast<double> (headWidthCm) / 200.0;
    const auto frontOffset = static_cast<double> (headLengthCm) / 200.0;
    const auto circumferenceRadius = static_cast<double> (circumferenceCm) / (2.0 * juce::MathConstants<double>::pi * 100.0);
    const auto speakerDistance = 1.0 + frontOffset;
    const auto dFar = std::sqrt (speakerDistance * speakerDistance + halfHead * halfHead
                               + 2.0 * speakerDistance * halfHead * std::sin (theta * 0.5));
    const auto dNear = std::sqrt (speakerDistance * speakerDistance + halfHead * halfHead
                                - 2.0 * speakerDistance * halfHead * std::sin (theta * 0.5));
    const auto pathDelay = (dFar - dNear) / 343.0;
    const auto headShadowDelay = circumferenceRadius * std::sin (theta * 0.5) / 343.0;
    const auto delaySeconds = juce::jmax (0.00005, 0.65 * pathDelay + 0.35 * headShadowDelay);
    const auto delaySamples = juce::jlimit (1, delaySize - 1, static_cast<int> (std::round (delaySeconds * sampleRate)));
    const auto lpAlpha = static_cast<float> (1.0 - std::exp (-juce::MathConstants<double>::twoPi * static_cast<double> (cutoffHz) / sampleRate));
    const auto width = 1.0f + amount * (0.42f - 1.0f);
    const auto feed = 0.16f * amount;

    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        const auto inL = left[i];
        const auto inR = right[i];
        delayL[crossfeedWrite] = inL;
        delayR[crossfeedWrite] = inR;
        const auto read = (crossfeedWrite - delaySamples + delaySize) % delaySize;
        lpL += lpAlpha * (delayL[read] - lpL);
        lpR += lpAlpha * (delayR[read] - lpR);
        const auto mid = 0.5f * (inL + inR);
        const auto side = 0.5f * (inL - inR) * width;
        left[i] = ((mid + side) + feed * lpR) * directGain;
        right[i] = ((mid - side) + feed * lpL) * directGain;
        crossfeedWrite = (crossfeedWrite + 1) % delaySize;
    }
}

void CalCurveAudioProcessor::addStoredCurveToState (juce::ValueTree& state) const
{
    state.removeChild (state.getChildWithName ("StoredCorrectionCurve"), nullptr);

    std::vector<CurvePoint> points;
    {
        const juce::ScopedLock lock (curveLock);
        points = correctionCurve;
    }

    if (points.empty())
        return;

    juce::ValueTree curve ("StoredCorrectionCurve");
    curve.setProperty ("format", "frequencyDbPoints", nullptr);
    curve.setProperty ("pointCount", static_cast<int> (points.size()), nullptr);

    for (const auto& point : points)
    {
        juce::ValueTree pointNode ("Point");
        pointNode.setProperty ("frequency", point.frequency, nullptr);
        pointNode.setProperty ("db", point.db, nullptr);
        curve.appendChild (pointNode, nullptr);
    }

    state.appendChild (curve, nullptr);
}

bool CalCurveAudioProcessor::restoreStoredCurveFromState (const juce::ValueTree& state, const juce::String& fallbackName)
{
    const auto curve = state.getChildWithName ("StoredCorrectionCurve");

    if (! curve.isValid())
        return false;

    std::vector<CurvePoint> points;
    points.reserve (static_cast<size_t> (curve.getNumChildren()));

    for (int i = 0; i < curve.getNumChildren(); ++i)
    {
        const auto pointNode = curve.getChild (i);
        const auto frequency = static_cast<double> (pointNode.getProperty ("frequency", 0.0));
        const auto db = static_cast<double> (pointNode.getProperty ("db", 0.0));

        if (frequency > 0.0 && std::isfinite (frequency) && std::isfinite (db))
            points.push_back ({ frequency, db });
    }

    if (points.empty())
        return false;

    {
        const juce::ScopedLock lock (curveLock);
        correctionCurve = std::move (points);
    }

    loadedName = fallbackName.isNotEmpty() ? fallbackName + " (embedded preset curve)" : "Embedded preset curve";
    rebuildFromStoredCurve();
    return hasImpulse;
}

void CalCurveAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    state.setProperty ("presetName", presetName, nullptr);
    state.setProperty ("loadedFilePath", loadedCurveFile.getFullPathName(), nullptr);
    state.setProperty ("loadedName", loadedName, nullptr);
    addStoredCurveToState (state);
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void CalCurveAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));

    if (xmlState != nullptr && xmlState->hasTagName (parameters.state.getType()))
    {
        const auto xmlText = xmlState->toString();
        const auto hasLegacyMinimumPhase = xmlText.containsIgnoreCase ("minimumphase")
                                        && (xmlText.containsIgnoreCase ("minimumphase=\"1")
                                         || xmlText.containsIgnoreCase ("minimumphase=\"true")
                                         || xmlText.containsIgnoreCase ("id=\"minimumphase\" value=\"1")
                                         || xmlText.containsIgnoreCase ("id=\"minimumphase\" value=\"true"));

        auto state = juce::ValueTree::fromXml (*xmlState);
        presetName = state.getProperty ("presetName", presetName).toString();
        const auto path = state.getProperty ("loadedFilePath").toString();
        const auto storedLoadedName = state.getProperty ("loadedName", loadedName).toString();
        parameters.replaceState (state);

        if (hasLegacyMinimumPhase)
        {
            if (auto* phaseMode = parameters.getParameter ("phasemode"))
                phaseMode->setValueNotifyingHost (0.0f);
        }

        bool restoredFromFile = false;
        if (path.isNotEmpty())
        {
            const juce::File calibrationFile (path);

            if (calibrationFile.existsAsFile())
            {
                if (calibrationFile.hasFileExtension ("wav"))
                    loadImpulseResponse (calibrationFile);
                else
                    loadCorrectionCurve (calibrationFile);

                restoredFromFile = true;
            }
            else
            {
                loadedCurveFile = calibrationFile;
            }
        }
        
        if (! restoredFromFile && ! restoreStoredCurveFromState (state, storedLoadedName))
        {
            if (path.isNotEmpty())
                loadedName = "Missing: " + juce::File (path).getFileName();
            else if (storedLoadedName.isNotEmpty())
                loadedName = storedLoadedName;

            hasImpulse = false;
        }
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new CalCurveAudioProcessor();
}
