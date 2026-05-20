#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "CurveFIR.h"

namespace
{
    constexpr int minimumPhaseTaps = 4096;
    constexpr int naturalPhaseTaps = 4096;
    constexpr int linearPhaseTaps = 8192;
    constexpr int naturalLatencySamples = 1024;
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
    currentSampleRate = sampleRate;

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32> (samplesPerBlock);
    spec.numChannels = static_cast<juce::uint32> (getTotalNumOutputChannels());

    convolution.prepare (spec);
    limiterGain = 1.0f;
    
    wetBuffer.setSize (getTotalNumOutputChannels(), samplesPerBlock);
    
    // 1 ms is enough for headphone crossfeed ITD at all normal sample rates.
    crossfeedDelay.setSize (2, juce::jmax (32, static_cast<int> (std::ceil (sampleRate * 0.001))));
    resetCrossfeed();

    dryDelayBuffer.setSize (getTotalNumOutputChannels(), 32768);
    dryDelayBuffer.clear();
    dryDelayWriteIndex = 0;
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
        updateWetAutoGain (correctionCurve, reader->sampleRate);

        const auto phaseMode = getPhaseMode();
        const auto taps = juce::jlimit (256, 16384, juce::jmax (getDefaultFirTapsForMode (phaseMode), impulse.getNumSamples()));
        auto monoImpulse = createImpulseForPhaseMode (correctionCurve, reader->sampleRate, taps);

        impulse.setSize (static_cast<int> (reader->numChannels), taps);
        for (int ch = 0; ch < static_cast<int> (reader->numChannels); ++ch)
            impulse.copyFrom (ch, 0, monoImpulse, 0, 0, taps);

        activeImpulsePeakSamples = findImpulseResponsePeak (impulse);
        applyActiveLatency (getLatencyForPhaseMode (phaseMode, impulse.getNumSamples()));

        convolution.loadImpulseResponse (std::move (impulse),
                                         reader->sampleRate,
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
            return CurveFIR::createMixedPhaseFIR (points, sampleRate, taps, 0.72f, juce::jlimit (0, taps - 1, naturalLatencySamples));

        case PhaseMode::linear:
        default:
            return CurveFIR::createLinearPhaseFIR (points, sampleRate, taps);
    }
}

int CalCurveAudioProcessor::getDefaultFirTapsForMode (PhaseMode mode) const
{
    switch (mode)
    {
        case PhaseMode::minimum: return minimumPhaseTaps;
        case PhaseMode::natural: return naturalPhaseTaps;
        case PhaseMode::linear:  return linearPhaseTaps;
    }

    return naturalPhaseTaps;
}

int CalCurveAudioProcessor::getLatencyForPhaseMode (PhaseMode mode, int taps) const
{
    switch (mode)
    {
        case PhaseMode::minimum: return 0;
        case PhaseMode::natural: return juce::jlimit (0, juce::jmax (0, taps - 1), naturalLatencySamples);
        case PhaseMode::linear:  return juce::jmax (0, taps / 2);
    }

    return 0;
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
}

void CalCurveAudioProcessor::applyCrossfeed (juce::AudioBuffer<float>& buffer, float amount)
{
    if (buffer.getNumChannels() < 2 || crossfeedDelay.getNumSamples() == 0 || amount <= 0.001f)
        return;

    auto* left = buffer.getWritePointer (0);
    auto* right = buffer.getWritePointer (1);
    auto* delayL = crossfeedDelay.getWritePointer (0);
    auto* delayR = crossfeedDelay.getWritePointer (1);
    const auto delaySize = crossfeedDelay.getNumSamples();

    const auto lpAlpha = static_cast<float> (1.0 - std::exp (-juce::MathConstants<double>::twoPi * 900.0 / currentSampleRate));
    const auto width = juce::jmap (amount, 1.0f, 0.42f);
    const auto crossGain = 0.16f * amount;
    auto delayOffset = static_cast<int> (std::round (0.00028 * currentSampleRate));
    delayOffset = juce::jlimit (1, delaySize - 1, delayOffset);

    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        const auto inL = left[i];
        const auto inR = right[i];

        const auto mid = 0.5f * (inL + inR);
        const auto side = 0.5f * (inL - inR) * width;

        lpL += lpAlpha * (inL - lpL);
        lpR += lpAlpha * (inR - lpR);

        const int readIdx = (crossfeedWrite - delayOffset + delaySize) % delaySize;

        const auto delayedLpL = delayL[readIdx];
        const auto delayedLpR = delayR[readIdx];

        left[i]  = mid + side + crossGain * delayedLpR;
        right[i] = mid - side + crossGain * delayedLpL;

        delayL[crossfeedWrite] = lpL;
        delayR[crossfeedWrite] = lpR;

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
