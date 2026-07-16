#include "FlexCurveProcessor.h"
#include "FlexCurveEditor.h"

#include <array>
#include <complex>
#include <cmath>

namespace
{
    constexpr double referenceSampleRate = 44100.0;
    constexpr int minimumPhaseReferenceTaps = 4096;
    constexpr int naturalPhaseReferenceTaps = 4096;
    constexpr int linearPhaseReferenceTaps = 8192;
    constexpr int naturalReferenceLatencySamples = 1024;
    constexpr int maxFirTaps = 65536;
    constexpr auto flexCurveGainMetadataPrefix = "FlexCurveLayerGainDb=";

    juce::Colour layerColourForIndex (int index)
    {
        static constexpr juce::uint32 colours[] =
        {
            0xff27d7a4, 0xff5ea1ff, 0xffffc857, 0xffff6b8a,
            0xffb388ff, 0xff4dd0e1, 0xffff8a5b, 0xff92d050,
            0xfff06bd8, 0xff00b8d9, 0xffffd166, 0xffa8dadc,
            0xffe76f51, 0xff8ecae6, 0xffcdb4db, 0xff80ed99,
            0xffff9f1c, 0xff9b8afb
        };
        return juce::Colour (colours[static_cast<size_t> (index) % std::size (colours)]);
    }

    juce::Colour nextLayerColour (const std::vector<FlexCurveLayer>& layers)
    {
        for (int i = 0; i < 18; ++i)
        {
            const auto colour = layerColourForIndex (i);
            const auto used = std::any_of (layers.begin(), layers.end(), [colour] (const auto& layer)
            {
                return layer.colour.getARGB() == colour.getARGB();
            });
            if (! used)
                return colour;
        }

        return layerColourForIndex (static_cast<int> (layers.size()));
    }

    double smoothStep (double edge0, double edge1, double x)
    {
        const auto t = juce::jlimit (0.0, 1.0, (x - edge0) / (edge1 - edge0));
        return t * t * (3.0 - 2.0 * t);
    }

    std::vector<CurvePoint> makeDefaultGrid()
    {
        std::vector<CurvePoint> points;
        points.reserve (512);

        for (int i = 0; i < 512; ++i)
        {
            const auto alpha = static_cast<double> (i) / 511.0;
            points.push_back ({ 20.0 * std::pow (1000.0, alpha), 0.0 });
        }

        return points;
    }

    void applyRegionTrims (std::vector<CurvePoint>& points, float bassGainDb, float midGainDb, float trebleGainDb,
                           float lowMidCrossoverHz, float midHighCrossoverHz)
    {
        for (auto& point : points)
        {
            const auto lowBlend = smoothStep (lowMidCrossoverHz * 0.55, lowMidCrossoverHz * 1.45, point.frequency);
            const auto highBlend = smoothStep (midHighCrossoverHz * 0.55, midHighCrossoverHz * 1.45, point.frequency);
            point.db += bassGainDb * (1.0 - lowBlend)
                      + midGainDb * lowBlend * (1.0 - highBlend)
                      + trebleGainDb * highBlend;
        }
    }

    void smoothCurveGeometry (std::vector<CurvePoint>& points)
    {
        if (points.size() < 5)
            return;

        auto smoothed = points;
        constexpr int radius = 5;
        constexpr double weights[] = { 1.0, 0.86, 0.62, 0.38, 0.20, 0.09 };

        for (int i = 0; i < static_cast<int> (points.size()); ++i)
        {
            double sum = 0.0;
            double weightSum = 0.0;
            for (int offset = -radius; offset <= radius; ++offset)
            {
                const auto index = juce::jlimit (0, static_cast<int> (points.size()) - 1, i + offset);
                const auto weight = weights[std::abs (offset)];
                sum += points[static_cast<size_t> (index)].db * weight;
                weightSum += weight;
            }
            smoothed[static_cast<size_t> (i)].db = sum / weightSum;
        }

        points.swap (smoothed);
    }

    bool parseFlexCurveGainMetadata (const juce::StringPairArray& metadata, double& gainDb)
    {
        for (int i = 0; i < metadata.size(); ++i)
        {
            const auto key = metadata.getAllKeys()[i];
            const auto value = metadata.getAllValues()[i];

            if (key.equalsIgnoreCase ("FlexCurveLayerGainDb"))
            {
                gainDb = value.getDoubleValue();
                return std::isfinite (gainDb);
            }

            const auto markerIndex = value.indexOfIgnoreCase (flexCurveGainMetadataPrefix);
            if (markerIndex >= 0)
            {
                const auto encoded = value.substring (markerIndex + juce::String (flexCurveGainMetadataPrefix).length())
                                          .upToFirstOccurrenceOf (";", false, false)
                                          .trim();
                gainDb = encoded.getDoubleValue();
                return std::isfinite (gainDb);
            }
        }

        return false;
    }

    juce::StringPairArray makeFlexCurveFirMetadata (double gainDb)
    {
        juce::StringPairArray metadata;
        metadata.set (juce::WavAudioFormat::riffInfoSoftware, "FlexCurve");
        metadata.set (juce::WavAudioFormat::riffInfoComment,
                      juce::String (flexCurveGainMetadataPrefix) + juce::String (gainDb, 8));
        return metadata;
    }

    float expandedRangeForImportedGain (float currentRange, double gainDb)
    {
        const auto magnitude = static_cast<float> (std::abs (gainDb));
        if (magnitude <= currentRange)
            return currentRange;

        for (const auto range : { 12.0f, 24.0f, 36.0f, 48.0f, 72.0f, 96.0f })
            if (magnitude <= range)
                return range;

        return 96.0f;
    }

    juce::String shortLayerNameForFile (const juce::File& file)
    {
        auto name = file.getFileNameWithoutExtension().replaceCharacter ('_', ' ').trim();
        while (name.contains ("  "))
            name = name.replace ("  ", " ");
        if (name.length() > 42)
            name = name.substring (0, 42).trimEnd();
        return name.isNotEmpty() ? name : "Imported Curve";
    }

    float channelBalanceOffsetDb (float balanceDb, FlexChannelSelection channel)
    {
        const auto limited = juce::jlimit (-24.0f, 24.0f, balanceDb);
        if (channel == FlexChannelSelection::right)
            return limited * 0.5f;
        return -limited * 0.5f;
    }

    double dbToGain (double db)
    {
        return std::pow (10.0, db / 20.0);
    }

    struct BufferPower
    {
        float peak = 0.0f;
        double meanSquare = 0.0;
        std::array<float, 3> channelPeak {};
        std::array<double, 3> channelMeanSquare {};
    };

    BufferPower measureBufferPower (const juce::AudioBuffer<float>& buffer)
    {
        BufferPower result;
        double sum = 0.0;
        juce::int64 count = 0;
        std::array<double, 2> channelSum {};
        std::array<juce::int64, 2> channelCount {};
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            const auto* data = buffer.getReadPointer (channel);
            const auto meterChannel = juce::jlimit (0, 1, channel);
            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            {
                const auto value = data[sample];
                const auto absValue = std::abs (value);
                result.peak = juce::jmax (result.peak, absValue);
                result.channelPeak[static_cast<size_t> (meterChannel)] = juce::jmax (
                    result.channelPeak[static_cast<size_t> (meterChannel)], absValue);
                sum += static_cast<double> (value) * value;
                channelSum[static_cast<size_t> (meterChannel)] += static_cast<double> (value) * value;
                ++channelCount[static_cast<size_t> (meterChannel)];
                ++count;
            }
        }
        result.meanSquare = count > 0 ? sum / static_cast<double> (count) : 0.0;
        for (size_t channel = 0; channel < 2; ++channel)
            result.channelMeanSquare[channel] = channelCount[channel] > 0
                ? channelSum[channel] / static_cast<double> (channelCount[channel]) : 0.0;
        result.channelPeak[2] = result.peak;
        result.channelMeanSquare[2] = result.meanSquare;
        return result;
    }

    float approximateLufsFromRmsDb (float rmsDb)
    {
        return rmsDb <= -99.0f ? -100.0f : rmsDb - 0.7f;
    }
}

FlexCurveAudioProcessor::FlexCurveAudioProcessor()
    : AudioProcessor (BusesProperties()
        .withInput ("Input", juce::AudioChannelSet::stereo(), true)
        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "Parameters", createParameterLayout())
{
    parameters.addParameterListener ("phasemode", this);
    parameters.addParameterListener ("autogain", this);
    parameters.addParameterListener ("loudnessmatchmode", this);
    resetMeters();
}

FlexCurveAudioProcessor::~FlexCurveAudioProcessor()
{
    cancelPendingUpdate();
    parameters.removeParameterListener ("phasemode", this);
    parameters.removeParameterListener ("autogain", this);
    parameters.removeParameterListener ("loudnessmatchmode", this);
}

juce::AudioProcessorValueTreeState::ParameterLayout FlexCurveAudioProcessor::createParameterLayout()
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
        "gain", "Gain", juce::NormalisableRange<float> (-96.0f, 96.0f, 0.01f), 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "globalbalance", "Global Balance",
        juce::NormalisableRange<float> (-24.0f, 24.0f, 0.01f), 0.0f,
        " dB"));
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "inputgain", "Input Gain", juce::NormalisableRange<float> (-36.0f, 36.0f, 0.01f), 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "outputgain", "Output Gain", juce::NormalisableRange<float> (-36.0f, 36.0f, 0.01f), 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterBool> ("autogain", "Auto Gain", true));
    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        "loudnessmatchmode", "Loudness Match Mode",
        juce::StringArray { "Match OUT to IN", "Match IN to OUT" }, 0));
    params.push_back (std::make_unique<juce::AudioParameterBool> (
        "includeoutputgainfir", "Include Output Gain in FIR Export", false));
    params.push_back (std::make_unique<juce::AudioParameterBool> (
        "includeautogainfir", "Include Auto Gain in FIR Export", false));
    params.push_back (std::make_unique<juce::AudioParameterBool> (
        "independentlrpreamp", "Use Independent L/R AutoEQ Preamp", false));

    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        "phasemode", "Phase Mode", juce::StringArray { "Minimum", "Natural", "Linear" }, 0));

    params.push_back (std::make_unique<juce::AudioParameterBool> ("limiter", "Limiter", false));
    params.push_back (std::make_unique<juce::AudioParameterBool> ("bypass", "Bypass", false));

    return { params.begin(), params.end() };
}

void FlexCurveAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    const auto previousSampleRate = currentSampleRate;
    currentSampleRate = sampleRate;

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32> (samplesPerBlock);
    spec.numChannels = static_cast<juce::uint32> (getTotalNumOutputChannels());
    convolution.prepare (spec);
    previewConvolution.prepare (spec);

    wetBuffer.setSize (getTotalNumOutputChannels(), samplesPerBlock);
    dryDelayBuffer.setSize (getTotalNumOutputChannels(), 131072);
    dryDelayBuffer.clear();
    dryDelayWriteIndex = 0;

    crossfeedDelay.setSize (2, juce::jmax (32, static_cast<int> (std::ceil (sampleRate * 0.003))));
    resetCrossfeed();
    limiterGain = 1.0f;
    previewFiltersDirty = true;
    previewFirReady = false;
    resetMeters();
    resetAutoGainState();
    triggerAsyncUpdate();

    if (hasRenderedFir && ! juce::approximatelyEqual (previousSampleRate, currentSampleRate))
    {
        if (! firOutdated.load())
            rebuildConvolutionFromRenderedCurve();
        else
            triggerAsyncUpdate();
    }
}

bool FlexCurveAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainInputChannelSet() != layouts.getMainOutputChannelSet())
        return false;

    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::mono()
        || layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

juce::AudioProcessorEditor* FlexCurveAudioProcessor::createEditor()
{
    return new FlexCurveAudioProcessorEditor (*this);
}

float FlexCurveAudioProcessor::Biquad::process (float x) noexcept
{
    const auto y = b0 * x + z1;
    if (! std::isfinite (y))
    {
        reset();
        return x;
    }
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return static_cast<float> (y);
}

void FlexCurveAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (auto i = getTotalNumInputChannels(); i < getTotalNumOutputChannels(); ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    const auto dryWet = parameters.getRawParameterValue ("drywet")->load();
    const auto crossfeed = parameters.getRawParameterValue ("crossfeed")->load();
    const auto globalBalanceDb = parameters.getRawParameterValue ("globalbalance")->load();
    const auto inputGain = juce::Decibels::decibelsToGain (parameters.getRawParameterValue ("inputgain")->load());
    const auto outputGain = juce::Decibels::decibelsToGain (parameters.getRawParameterValue ("outputgain")->load());
    const auto globalGain = juce::Decibels::decibelsToGain (parameters.getRawParameterValue ("gain")->load());
    const auto autoGainEnabled = parameters.getRawParameterValue ("autogain")->load() > 0.5f;
    const auto limiterEnabled = parameters.getRawParameterValue ("limiter")->load() > 0.5f;
    const auto bypassed = parameters.getRawParameterValue ("bypass")->load() > 0.5f;

    buffer.applyGain (inputGain);
    const auto inputStats = measureBufferPower (buffer);
    updateMeters (buffer, true);

    if (bypassed)
    {
        if (activeLatencySamples > 0 && dryDelayBuffer.getNumSamples() > 0)
        {
            const auto numSamples = buffer.getNumSamples();
            for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            {
                auto* channelData = buffer.getWritePointer (channel);
                auto* delayData = dryDelayBuffer.getWritePointer (channel);
                int writeIdx = dryDelayWriteIndex;
                for (int sample = 0; sample < numSamples; ++sample)
                {
                    delayData[writeIdx] = channelData[sample];
                    const auto delaySize = dryDelayBuffer.getNumSamples();
                    channelData[sample] = delayData[(writeIdx - activeLatencySamples + delaySize) % delaySize];
                    writeIdx = (writeIdx + 1) % delaySize;
                }
            }
            dryDelayWriteIndex = (dryDelayWriteIndex + numSamples) % dryDelayBuffer.getNumSamples();
        }

        applyGlobalBalance (buffer, globalBalanceDb);
        buffer.applyGain (outputGain * globalGain);
        if (limiterEnabled)
        {
            const auto peak = measureBufferPower (buffer).peak;
            if (peak > 1.0f)
                buffer.applyGain (1.0f / peak);
        }
        updateMeters (buffer, false);
        return;
    }

    const bool useAshPreview = ashPreviewActive.load();
    const bool useFir = ! useAshPreview && hasRenderedFir.load() && ! firOutdated.load() && dryWet > 0.0f;

    if (useFir)
    {
        wetBuffer.makeCopyOf (buffer, true);
        juce::dsp::AudioBlock<float> wetBlock (wetBuffer);
        juce::dsp::ProcessContextReplacing<float> context (wetBlock);
        convolution.process (context);
    }

    if (activeLatencySamples > 0 && dryDelayBuffer.getNumSamples() > 0)
    {
        const auto numSamples = buffer.getNumSamples();
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            auto* channelData = buffer.getWritePointer (channel);
            auto* delayData = dryDelayBuffer.getWritePointer (channel);
            int writeIdx = dryDelayWriteIndex;
            for (int sample = 0; sample < numSamples; ++sample)
            {
                delayData[writeIdx] = channelData[sample];
                const auto delaySize = dryDelayBuffer.getNumSamples();
                channelData[sample] = delayData[(writeIdx - activeLatencySamples + delaySize) % delaySize];
                writeIdx = (writeIdx + 1) % delaySize;
            }
        }
        dryDelayWriteIndex = (dryDelayWriteIndex + numSamples) % dryDelayBuffer.getNumSamples();
    }

    if (useFir)
    {
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            auto* dry = buffer.getWritePointer (channel);
            const auto* wet = wetBuffer.getReadPointer (channel);
            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                dry[sample] = dry[sample] + dryWet * (wet[sample] - dry[sample]);
        }
    }
    else if (dryWet > 0.0f)
    {
        wetBuffer.makeCopyOf (buffer, true);
        applyPreviewFilters (wetBuffer);
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            auto* dry = buffer.getWritePointer (channel);
            const auto* wet = wetBuffer.getReadPointer (channel);
            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                dry[sample] += dryWet * (wet[sample] - dry[sample]);
        }
    }

    if (crossfeed > 0.0f)
        applyCrossfeed (buffer, crossfeed);

    applyGlobalBalance (buffer, globalBalanceDb);

    const auto preAutoStats = measureBufferPower (buffer);
    const auto downstreamGain = outputGain * globalGain;
    const auto preAutoPeak = preAutoStats.peak * downstreamGain;
    const auto preAutoPeakValueDb = juce::Decibels::gainToDecibels (preAutoPeak, -100.0f);
    const auto preAutoRmsValueDb = juce::Decibels::gainToDecibels (
        static_cast<float> (std::sqrt (juce::jmax (0.0, preAutoStats.meanSquare))) * downstreamGain, -100.0f);
    auto updatePreAutoBallistic = [] (std::atomic<float>& target, float next)
    {
        const auto previous = target.load();
        target.store (next >= previous ? next : previous * 0.88f + next * 0.12f);
    };
    updatePreAutoBallistic (preAutoPeakDb, preAutoPeakValueDb);
    updatePreAutoBallistic (preAutoRmsDb, preAutoRmsValueDb);
    for (size_t channel = 0; channel < 3; ++channel)
    {
        const auto channelPeakDb = juce::Decibels::gainToDecibels (
            preAutoStats.channelPeak[channel] * downstreamGain, -100.0f);
        const auto channelRmsDb = juce::Decibels::gainToDecibels (
            static_cast<float> (std::sqrt (juce::jmax (0.0, preAutoStats.channelMeanSquare[channel]))) * downstreamGain, -100.0f);
        const auto channelLufs = approximateLufsFromRmsDb (channelRmsDb);
        updatePreAutoBallistic (preAutoChannelPeakDb[channel], channelPeakDb);
        updatePreAutoBallistic (preAutoChannelRmsDb[channel], channelRmsDb);
        updatePreAutoBallistic (preAutoChannelLufsMomentary[channel], channelLufs);
        preAutoChannelLufsShortTerm[channel].store (
            preAutoChannelLufsShortTerm[channel].load() * 0.94f + channelLufs * 0.06f);
        preAutoChannelLufsIntegrated[channel].store (
            preAutoChannelLufsIntegrated[channel].load() * 0.985f + channelLufs * 0.015f);
    }
    preAutoClip.store (preAutoPeak > 1.0f);
    preAutoClipOverDb.store (preAutoPeak > 1.0f ? juce::Decibels::gainToDecibels (preAutoPeak, 0.0f) : 0.0f);

    if (autoGainEnabled)
        buffer.applyGain (juce::Decibels::decibelsToGain (runtimeAutoGainDb.load()));

    buffer.applyGain (downstreamGain);

    if (limiterEnabled)
    {
        float peak = 0.0f;
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            peak = juce::jmax (peak, buffer.getMagnitude (channel, 0, buffer.getNumSamples()));

        float targetLimiterGain = peak > 1.0f ? 1.0f / peak : 1.0f;
        if (targetLimiterGain < limiterGain)
            limiterGain = targetLimiterGain;
        else if (limiterGain < 1.0f)
        {
            const auto releaseCoef = std::exp (-1.0 / ((currentSampleRate > 0.0 ? currentSampleRate : 48000.0) * 0.150));
            limiterGain = static_cast<float> (targetLimiterGain + (limiterGain - targetLimiterGain) * std::pow (releaseCoef, buffer.getNumSamples()));
        }
        else
            limiterGain = 1.0f;

        if (limiterGain < 1.0f)
            buffer.applyGain (limiterGain);

        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            auto* data = buffer.getWritePointer (channel);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                data[i] = juce::jlimit (-1.0f, 1.0f, data[i]);
        }
    }

    updateMeters (buffer, false);
}

ParsedCurveData FlexCurveAudioProcessor::loadCurveData (const juce::File& file) const
{
    ParsedCurveData result;
    if (file.hasFileExtension ("wav"))
    {
        juce::AudioFormatManager formatManager;
        formatManager.registerBasicFormats();
        if (auto reader = std::unique_ptr<juce::AudioFormatReader> (formatManager.createReaderFor (file)))
        {
            juce::AudioBuffer<float> impulse (static_cast<int> (reader->numChannels), static_cast<int> (reader->lengthInSamples));
            impulse.clear();
            reader->read (&impulse, 0, impulse.getNumSamples(), 0, true, true);
            juce::AudioBuffer<float> leftImpulse (1, impulse.getNumSamples());
            leftImpulse.copyFrom (0, 0, impulse, 0, 0, impulse.getNumSamples());
            result.points = CurveFIR::createMagnitudeCurveFromImpulse (leftImpulse, reader->sampleRate);
            if (impulse.getNumChannels() > 1)
            {
                juce::AudioBuffer<float> rightImpulse (1, impulse.getNumSamples());
                rightImpulse.copyFrom (0, 0, impulse, 1, 0, impulse.getNumSamples());
                result.rightPoints = CurveFIR::createMagnitudeCurveFromImpulse (rightImpulse, reader->sampleRate);
                result.hasIndependentRightChannel = true;
            }
            else
            {
                result.rightPoints = result.points;
            }
            result.hasExplicitGain = parseFlexCurveGainMetadata (reader->metadataValues, result.gainDb);
            result.rightGainDb = result.gainDb;
            if (result.hasExplicitGain)
            {
                for (auto& point : result.points)
                    point.db -= result.gainDb;
                for (auto& point : result.rightPoints)
                    point.db -= result.rightGainDb;
            }
        }
    }
    else
    {
        result = CurveFIR::parseCurveFileWithGain (file);
    }
    return result;
}

void FlexCurveAudioProcessor::copyLeftChannelToRight (FlexCurveLayer& layer)
{
    layer.right.autoEqMethod = layer.autoEqMethod;
    layer.right.autoEqSourcesOutdated = layer.autoEqSourcesOutdated;
    layer.right.autoEqReferenceOffsetDb = layer.autoEqReferenceOffsetDb;
    layer.right.autoEqReferenceDisplayEnabled = layer.autoEqReferenceDisplayEnabled;
    layer.right.points = layer.points;
    layer.right.inverted = layer.inverted;
    layer.right.gainDb = layer.gainDb;
    layer.right.normalizationOffsetDb = layer.normalizationOffsetDb;
    layer.right.smoothSourceCurve = layer.smoothSourceCurve;
    layer.right.blend = layer.blend;
    layer.right.graphicEnabled = layer.graphicEnabled;
    layer.right.graphicMode = layer.graphicMode;
    layer.right.preserveVariableShapeAcrossModes = layer.preserveVariableShapeAcrossModes;
    layer.right.smoothGraphicCurve = layer.smoothGraphicCurve;
    layer.right.graphic15Gains = layer.graphic15Gains;
    layer.right.graphic31Gains = layer.graphic31Gains;
    layer.right.paramBands = layer.paramBands;
    layer.right.freeformPoints = layer.freeformPoints;
}

void FlexCurveAudioProcessor::copyRightChannelToLeft (FlexCurveLayer& layer)
{
    layer.autoEqMethod = layer.right.autoEqMethod;
    layer.autoEqSourcesOutdated = layer.right.autoEqSourcesOutdated;
    layer.autoEqReferenceOffsetDb = layer.right.autoEqReferenceOffsetDb;
    layer.autoEqReferenceDisplayEnabled = layer.right.autoEqReferenceDisplayEnabled;
    layer.points = layer.right.points;
    layer.inverted = layer.right.inverted;
    layer.gainDb = layer.right.gainDb;
    layer.normalizationOffsetDb = layer.right.normalizationOffsetDb;
    layer.smoothSourceCurve = layer.right.smoothSourceCurve;
    layer.blend = layer.right.blend;
    layer.graphicEnabled = layer.right.graphicEnabled;
    layer.graphicMode = layer.right.graphicMode;
    layer.preserveVariableShapeAcrossModes = layer.right.preserveVariableShapeAcrossModes;
    layer.smoothGraphicCurve = layer.right.smoothGraphicCurve;
    layer.graphic15Gains = layer.right.graphic15Gains;
    layer.graphic31Gains = layer.right.graphic31Gains;
    layer.paramBands = layer.right.paramBands;
    layer.freeformPoints = layer.right.freeformPoints;
}

void FlexCurveAudioProcessor::loadRightChannelIntoLayerView (const FlexCurveLayer& source,
                                                              FlexCurveLayer& destination)
{
    destination = source;
    copyRightChannelToLeft (destination);
}

void FlexCurveAudioProcessor::saveLayerViewIntoRightChannel (const FlexCurveLayer& source,
                                                              FlexCurveLayer& destination)
{
    auto copy = source;
    copyLeftChannelToRight (copy);
    destination.right = std::move (copy.right);
}

void FlexCurveAudioProcessor::applyToSelectedChannelsLocked (
    FlexCurveLayer& layer, const std::function<void(FlexCurveLayer&)>& operation)
{
    if (layer.selectedChannel == FlexChannelSelection::stereo)
    {
        operation (layer);
        if (layer.channelsLinked)
            copyLeftChannelToRight (layer);
        else
        {
            auto rightView = layer;
            loadRightChannelIntoLayerView (layer, rightView);
            operation (rightView);
            saveLayerViewIntoRightChannel (rightView, layer);
        }
        return;
    }

    if (layer.channelsLinked)
    {
        copyLeftChannelToRight (layer);
        layer.channelsLinked = false;
    }

    if (layer.selectedChannel == FlexChannelSelection::left)
    {
        operation (layer);
        return;
    }

    auto rightView = layer;
    loadRightChannelIntoLayerView (layer, rightView);
    operation (rightView);
    saveLayerViewIntoRightChannel (rightView, layer);
}

FlexChannelSelection FlexCurveAudioProcessor::getDisplayChannelLocked (const FlexCurveLayer& layer) const
{
    return layer.selectedChannel == FlexChannelSelection::right
        ? FlexChannelSelection::right : FlexChannelSelection::left;
}

bool FlexCurveAudioProcessor::addCurveFile (const juce::File& file)
{
    if (editsBlocked() || ! canAddUserLayer())
        return false;

    auto imported = loadCurveData (file);

    if (imported.points.empty())
        return false;

    recordUndoState ("import-layer");
    imported.gainDb = juce::jlimit (-96.0, 96.0, imported.gainDb);
    globalDbRange.store (expandedRangeForImportedGain (globalDbRange.load(), imported.gainDb));
    {
        const juce::ScopedLock lock (projectLock);
        FlexCurveLayer layer;
        layer.id = nextLayerId++;
        layer.type = FlexCurveLayerType::eq;
        layer.name = shortLayerNameForFile (file);
        layer.sourceFile = file;
        layer.points = std::move (imported.points);
        layer.gainDb = static_cast<float> (imported.gainDb);
        layer.colour = nextLayerColour (layers);
        layer.paramBands.resize (8);
        copyLeftChannelToRight (layer);
        if (imported.hasIndependentRightChannel && ! imported.rightPoints.empty())
        {
            layer.right.points = std::move (imported.rightPoints);
            layer.right.gainDb = static_cast<float> (
                imported.hasExplicitRightGain ? imported.rightGainDb : imported.gainDb);
            layer.channelsLinked = false;
        }
        layers.push_back (std::move (layer));
        if (activeLayerId == 0)
            activeLayerId = layers.back().id;
        finalCurve = calculateFinalCurveLocked();
    }

    markPreviewDirty();
    return true;
}

bool FlexCurveAudioProcessor::addCurvePoints (const juce::String& name, std::vector<CurvePoint> points)
{
    if (editsBlocked() || ! canAddUserLayer() || points.empty())
        return false;

    recordUndoState ("add-curve-points");
    {
        const juce::ScopedLock lock (projectLock);
        FlexCurveLayer layer;
        layer.id = nextLayerId++;
        layer.type = FlexCurveLayerType::eq;
        layer.name = name.isNotEmpty() ? name : "ASH Curve";
        layer.points = std::move (points);
        layer.colour = nextLayerColour (layers);
        layer.paramBands.resize (8);
        copyLeftChannelToRight (layer);
        layers.push_back (std::move (layer));
        activeLayerId = layers.back().id;
        ashPreviewCurve.clear();
        ashPreviewLabel.clear();
        ashPreviewActive.store (false);
        finalCurve = calculateFinalCurveLocked();
    }

    markPreviewDirty();
    return true;
}

bool FlexCurveAudioProcessor::addReferenceCurveFile (const juce::File& file, FlexCurveLayerType type)
{
    if (type == FlexCurveLayerType::eq)
        return addCurveFile (file);
    if (editsBlocked() || ! canAddLayerType (type))
        return false;

    auto imported = loadCurveData (file);
    if (imported.points.empty())
        return false;

    recordUndoState (type == FlexCurveLayerType::target ? "import-target" : "import-raw");
    imported.gainDb = juce::jlimit (-96.0, 96.0, imported.gainDb);
    globalDbRange.store (expandedRangeForImportedGain (globalDbRange.load(), imported.gainDb));
    {
        const juce::ScopedLock lock (projectLock);
        FlexCurveLayer layer;
        layer.id = nextLayerId++;
        layer.type = type;
        layer.name = shortLayerNameForFile (file);
        layer.sourceFile = file;
        layer.points = std::move (imported.points);
        layer.gainDb = static_cast<float> (imported.gainDb);
        layer.colour = nextLayerColour (layers);
        layer.paramBands.resize (8);
        copyLeftChannelToRight (layer);
        if (imported.hasIndependentRightChannel && ! imported.rightPoints.empty())
        {
            layer.right.points = std::move (imported.rightPoints);
            layer.right.gainDb = static_cast<float> (
                imported.hasExplicitRightGain ? imported.rightGainDb : imported.gainDb);
            layer.channelsLinked = false;
        }
        layers.push_back (std::move (layer));
        activeLayerId = layers.back().id;
        finalCurve = calculateFinalCurveLocked();
    }
    triggerAsyncUpdate();
    return true;
}

bool FlexCurveAudioProcessor::addFlatCurve()
{
    if (editsBlocked())
        return false;

    if (! canAddUserLayer())
        return false;

    recordUndoState ("add-flat-layer");
    {
        const juce::ScopedLock lock (projectLock);
        FlexCurveLayer layer;
        layer.id = nextLayerId++;
        layer.name = "Curve " + juce::String (layers.size() + 1);
        layer.points = makeDefaultGrid();
        layer.colour = nextLayerColour (layers);
        layer.enabled = true;
        layer.muted = false;
        layer.solo = false;
        layer.gainDb = 0.0f;
        layer.balanceDb = 0.0f;
        layer.opacity = 1.0f;
        layer.paramBands.resize (8);
        copyLeftChannelToRight (layer);
        layers.push_back (std::move (layer));
        if (activeLayerId == 0)
            activeLayerId = layers.back().id;
        finalCurve = calculateFinalCurveLocked();
    }
    markPreviewDirty();
    return true;
}

bool FlexCurveAudioProcessor::cloneLayer (int id)
{
    if (editsBlocked())
        return false;

    recordUndoState ("clone-layer-" + juce::String (id));
    {
        const juce::ScopedLock lock (projectLock);
        const auto* source = findLayer (id);
        if (source == nullptr
            || std::count_if (layers.begin(), layers.end(), [source] (const auto& layer)
               {
                   return layer.type == source->type;
               }) >= maxUserLayers)
            return false;

        auto clone = *source;
        clone.id = nextLayerId++;
        clone.name = source->name + " Copy";
        clone.colour = nextLayerColour (layers);
        layers.push_back (std::move (clone));
        activeLayerId = layers.back().id;
        finalCurve = calculateFinalCurveLocked();
    }
    markPreviewDirty();
    return true;
}

bool FlexCurveAudioProcessor::canAddUserLayer() const
{
    return canAddLayerType (FlexCurveLayerType::eq);
}

bool FlexCurveAudioProcessor::canAddLayerType (FlexCurveLayerType type) const
{
    const juce::ScopedLock lock (projectLock);
    return std::count_if (layers.begin(), layers.end(), [type] (const auto& layer)
    {
        return layer.type == type;
    }) < maxUserLayers;
}

void FlexCurveAudioProcessor::setLayerTypeVisible (FlexCurveLayerType type, bool visible)
{
    layerTypeVisible[static_cast<size_t> (type)].store (visible);
    triggerAsyncUpdate();
}

bool FlexCurveAudioProcessor::isLayerTypeVisible (FlexCurveLayerType type) const noexcept
{
    return layerTypeVisible[static_cast<size_t> (type)].load();
}

void FlexCurveAudioProcessor::setCorrectedMeasurementsVisible (bool visible)
{
    correctedMeasurementsVisible.store (visible);
    triggerAsyncUpdate();
}

bool FlexCurveAudioProcessor::areCorrectedMeasurementsVisible() const noexcept
{
    return correctedMeasurementsVisible.load();
}

void FlexCurveAudioProcessor::removeLayer (int id)
{
    if (editsBlocked())
        return;

    recordUndoState ("remove-layer");
    {
        const juce::ScopedLock lock (projectLock);
        for (auto& layer : layers)
        {
            if (layer.autoEqRawLayerId == id)
            {
                layer.autoEqRawLayerId = 0;
                layer.autoEqSourcesOutdated = true;
            }
            if (layer.autoEqTargetLayerId == id)
            {
                layer.autoEqTargetLayerId = 0;
                layer.autoEqSourcesOutdated = true;
            }
        }
        layers.erase (std::remove_if (layers.begin(), layers.end(), [id] (const auto& l) { return l.id == id; }), layers.end());
        if (activeLayerId == id)
            activeLayerId = layers.empty() ? 0 : layers.front().id;
        finalCurve = calculateFinalCurveLocked();
    }
    markPreviewDirty();
}

void FlexCurveAudioProcessor::setLayerEnabled (int id, bool enabled)
{
    if (editsBlocked())
        return;

    recordUndoState ("layer-enabled-" + juce::String (id));
    {
        const juce::ScopedLock lock (projectLock);
        for (auto& layer : layers)
            if (layer.id == id)
            {
                layer.enabled = enabled;
                layer.muted = ! enabled;
            }
        finalCurve = calculateFinalCurveLocked();
    }
    markPreviewDirty();
}

void FlexCurveAudioProcessor::setLayerMuted (int id, bool muted)
{
    if (editsBlocked())
        return;

    recordUndoState ("layer-muted-" + juce::String (id));
    {
        const juce::ScopedLock lock (projectLock);
        for (auto& layer : layers)
            if (layer.id == id)
            {
                layer.muted = muted;
                layer.enabled = ! muted;
            }
        finalCurve = calculateFinalCurveLocked();
    }
    markPreviewDirty();
}

void FlexCurveAudioProcessor::setLayerVisible (int id, bool visible)
{
    if (! editLocked.load())
        recordUndoState ("layer-visible-" + juce::String (id));
    const juce::ScopedLock lock (projectLock);
    for (auto& layer : layers)
        if (layer.id == id)
            layer.visible = visible;
}

void FlexCurveAudioProcessor::setLayerSolo (int id, bool solo)
{
    if (editsBlocked())
        return;

    recordUndoState ("layer-solo-" + juce::String (id));
    {
        const juce::ScopedLock lock (projectLock);
        for (auto& layer : layers)
            if (layer.id == id)
                layer.solo = solo;
        finalCurve = calculateFinalCurveLocked();
    }
    markPreviewDirty();
}

void FlexCurveAudioProcessor::setLayerGain (int id, float gainDb)
{
    if (editsBlocked())
        return;

    recordUndoState ("layer-gain-" + juce::String (id));
    gainDb = juce::jlimit (-globalDbRange.load(), globalDbRange.load(), gainDb);
    {
        const juce::ScopedLock lock (projectLock);
        for (auto& layer : layers)
            if (layer.id == id)
            {
                applyToSelectedChannelsLocked (layer, [gainDb] (auto& channel)
                {
                    channel.gainDb = gainDb;
                });
                if (layer.type != FlexCurveLayerType::eq)
                    markLinkedAutoEqLayersOutdatedLocked (layer.id);
            }
        finalCurve = calculateFinalCurveLocked();
        finalCurveRight = calculateAverageCurveLocked (FlexChannelSelection::right);
    }
    markPreviewDirty();
}

void FlexCurveAudioProcessor::setLayerBalance (int id, float balanceDb)
{
    if (editsBlocked())
        return;

    recordUndoState ("layer-balance-" + juce::String (id));
    balanceDb = juce::jlimit (-24.0f, 24.0f, balanceDb);
    {
        const juce::ScopedLock lock (projectLock);
        if (auto* layer = findLayer (id))
        {
            layer->balanceDb = balanceDb;
            finalCurve = calculateFinalCurveLocked();
            finalCurveRight = calculateAverageCurveLocked (FlexChannelSelection::right);
        }
    }
    markPreviewDirty();
}

void FlexCurveAudioProcessor::setLayerChannelSelection (int id, FlexChannelSelection channel)
{
    channel = static_cast<FlexChannelSelection> (
        juce::jlimit (0, 2, static_cast<int> (channel)));
    if (! editLocked.load())
        recordUndoState ("layer-channel-" + juce::String (id));
    const juce::ScopedLock lock (projectLock);
    if (auto* layer = findLayer (id))
        layer->selectedChannel = channel;
}

FlexChannelSelection FlexCurveAudioProcessor::getLayerChannelSelection (int id) const
{
    const juce::ScopedLock lock (projectLock);
    if (const auto* layer = findLayer (id))
        return layer->selectedChannel;
    return FlexChannelSelection::stereo;
}

bool FlexCurveAudioProcessor::areLayerChannelsLinked (int id) const
{
    const juce::ScopedLock lock (projectLock);
    if (const auto* layer = findLayer (id))
        return layer->channelsLinked;
    return true;
}

void FlexCurveAudioProcessor::linkLayerChannels (int id)
{
    if (editsBlocked())
        return;

    recordUndoState ("link-layer-channels-" + juce::String (id));
    {
        const juce::ScopedLock lock (projectLock);
        if (auto* layer = findLayer (id))
        {
            if (layer->selectedChannel == FlexChannelSelection::right)
                copyRightChannelToLeft (*layer);
            copyLeftChannelToRight (*layer);
            layer->channelsLinked = true;
            layer->selectedChannel = FlexChannelSelection::stereo;
            finalCurve = calculateFinalCurveLocked();
            finalCurveRight = finalCurve;
        }
    }
    markPreviewDirty();
}

void FlexCurveAudioProcessor::setLayerSourceSmoothing (int id, bool enabled)
{
    if (editsBlocked())
        return;

    recordUndoState ("source-smoothing-" + juce::String (id));
    {
        const juce::ScopedLock lock (projectLock);
        if (auto* layer = findLayer (id))
        {
            applyToSelectedChannelsLocked (*layer, [enabled] (auto& channel)
            {
                channel.smoothSourceCurve = enabled;
            });
            if (layer->type != FlexCurveLayerType::eq)
                markLinkedAutoEqLayersOutdatedLocked (layer->id);
        }
        finalCurve = calculateFinalCurveLocked();
    }
    markPreviewDirty();
}

void FlexCurveAudioProcessor::setGlobalDbRange (float rangeDb)
{
    if (editsBlocked())
        return;

    recordUndoState ("global-db-range");
    const auto next = juce::jlimit (1.0f, 96.0f, std::abs (rangeDb));
    globalDbRange.store (next);

    {
        const juce::ScopedLock lock (projectLock);
        for (auto& layer : layers)
        {
            const auto selection = layer.selectedChannel;
            layer.selectedChannel = FlexChannelSelection::stereo;
            applyToSelectedChannelsLocked (layer, [next] (auto& channel)
            {
                channel.gainDb = juce::jlimit (-next, next, channel.gainDb);
                channel.blend.bassGainDb = juce::jlimit (-next, next, channel.blend.bassGainDb);
                channel.blend.midGainDb = juce::jlimit (-next, next, channel.blend.midGainDb);
                channel.blend.trebleGainDb = juce::jlimit (-next, next, channel.blend.trebleGainDb);
                for (auto& gain : channel.graphic15Gains)
                    gain = juce::jlimit (-next, next, gain);
                for (auto& gain : channel.graphic31Gains)
                    gain = juce::jlimit (-next, next, gain);
                for (auto& band : channel.paramBands)
                    band.gainDb = juce::jlimit (-next, next, band.gainDb);
                for (auto& point : channel.freeformPoints)
                    point.db = juce::jlimit (static_cast<double> (-next), static_cast<double> (next), point.db);
            });
            layer.selectedChannel = selection;
        }
        globalBlend.bassGainDb = juce::jlimit (-next, next, globalBlend.bassGainDb);
        globalBlend.midGainDb = juce::jlimit (-next, next, globalBlend.midGainDb);
        globalBlend.trebleGainDb = juce::jlimit (-next, next, globalBlend.trebleGainDb);
        finalCurve = calculateFinalCurveLocked();
    }

    if (auto* gain = dynamic_cast<juce::RangedAudioParameter*> (parameters.getParameter ("gain")))
    {
        const auto actual = juce::jlimit (-next, next, parameters.getRawParameterValue ("gain")->load());
        gain->setValueNotifyingHost (gain->convertTo0to1 (actual));
    }
    markPreviewDirty();
}

void FlexCurveAudioProcessor::toggleLayerInverted (int id)
{
    if (editsBlocked())
        return;

    recordUndoState ("layer-inverted-" + juce::String (id));
    {
        const juce::ScopedLock lock (projectLock);
        if (auto* layer = findLayer (id))
            applyToSelectedChannelsLocked (*layer, [] (auto& channel)
            {
                channel.inverted = ! channel.inverted;
            });
        finalCurve = calculateFinalCurveLocked();
    }
    markPreviewDirty();
}

void FlexCurveAudioProcessor::setLayerOpacity (int id, float opacity)
{
    if (editsBlocked())
        return;

    recordUndoState ("layer-opacity-" + juce::String (id));
    {
        const juce::ScopedLock lock (projectLock);
        for (auto& layer : layers)
            if (layer.id == id)
                layer.opacity = juce::jlimit (0.0f, 1.0f, opacity);
        finalCurve = calculateFinalCurveLocked();
    }
    markPreviewDirty();
}

void FlexCurveAudioProcessor::renameLayer (int id, const juce::String& name)
{
    if (editsBlocked())
        return;

    recordUndoState ("rename-layer-" + juce::String (id));
    const juce::ScopedLock lock (projectLock);
    for (auto& layer : layers)
        if (layer.id == id)
            layer.name = name.trim().isNotEmpty() ? name.trim() : ("Curve " + juce::String (layer.id));
}

void FlexCurveAudioProcessor::resetLayerEdits (int id)
{
    if (editsBlocked())
        return;

    recordUndoState ("reset-layer-" + juce::String (id));
    {
        const juce::ScopedLock lock (projectLock);
        if (auto* layer = findLayer (id))
        {
            layer->muted = false;
            layer->enabled = true;
            layer->solo = false;
            layer->opacity = 1.0f;
            layer->balanceDb = 0.0f;
            applyToSelectedChannelsLocked (*layer, [] (auto& channel)
            {
                channel.gainDb = 0.0f;
                channel.normalizationOffsetDb = 0.0f;
                channel.inverted = false;
                channel.blend = {};
                channel.graphicEnabled = false;
                channel.graphicMode = 31;
                channel.preserveVariableShapeAcrossModes = false;
                channel.smoothGraphicCurve = false;
                channel.smoothSourceCurve = false;
                channel.graphic15Gains.fill (0.0f);
                channel.graphic31Gains.fill (0.0f);
                channel.paramBands.assign (8, {});
                channel.freeformPoints.clear();
            });
            if (layer->type != FlexCurveLayerType::eq)
                markLinkedAutoEqLayersOutdatedLocked (layer->id);
            finalCurve = calculateFinalCurveLocked();
        }
    }
    markPreviewDirty();
}

void FlexCurveAudioProcessor::resetLayerToFlat (int id)
{
    if (editsBlocked())
        return;

    recordUndoState ("flat-layer-" + juce::String (id));
    {
        const juce::ScopedLock lock (projectLock);
        if (auto* layer = findLayer (id))
        {
            layer->balanceDb = 0.0f;
            applyToSelectedChannelsLocked (*layer, [] (auto& channel)
            {
                channel.points = makeDefaultGrid();
                channel.gainDb = 0.0f;
                channel.normalizationOffsetDb = 0.0f;
                channel.inverted = false;
                channel.blend = {};
                channel.graphic15Gains.fill (0.0f);
                channel.graphic31Gains.fill (0.0f);
                channel.freeformPoints.clear();
                channel.paramBands.assign (8, {});
                channel.graphicEnabled = false;
                channel.preserveVariableShapeAcrossModes = false;
                channel.smoothGraphicCurve = false;
                channel.smoothSourceCurve = false;
            });
            if (layer->type != FlexCurveLayerType::eq)
                markLinkedAutoEqLayersOutdatedLocked (layer->id);
            finalCurve = calculateFinalCurveLocked();
        }
    }
    markPreviewDirty();
}

void FlexCurveAudioProcessor::resetAllLayerCurvesToFlat()
{
    if (editsBlocked())
        return;

    recordUndoState ("flat-all-layers");
    {
        const juce::ScopedLock lock (projectLock);
        for (auto& layer : layers)
        {
            const auto selection = layer.selectedChannel;
            layer.selectedChannel = FlexChannelSelection::stereo;
            applyToSelectedChannelsLocked (layer, [] (auto& channel)
            {
                channel.points = makeDefaultGrid();
                channel.gainDb = 0.0f;
                channel.normalizationOffsetDb = 0.0f;
                channel.inverted = false;
                channel.blend = {};
                channel.graphic15Gains.fill (0.0f);
                channel.graphic31Gains.fill (0.0f);
                channel.freeformPoints.clear();
                channel.paramBands.assign (8, {});
                channel.graphicEnabled = false;
                channel.preserveVariableShapeAcrossModes = false;
                channel.smoothGraphicCurve = false;
                channel.smoothSourceCurve = false;
            });
            layer.selectedChannel = selection;
        }
        for (auto& layer : layers)
            if (layer.type == FlexCurveLayerType::eq && layer.autoEqRawLayerId > 0)
                layer.autoEqSourcesOutdated = true;
        globalBlend = {};
        finalCurve = calculateFinalCurveLocked();
    }
    markPreviewDirty();
}

void FlexCurveAudioProcessor::resetAll()
{
    if (editsBlocked())
        return;

    recordUndoState ("reset-all");
    {
        const juce::ScopedLock lock (projectLock);
        layers.clear();
        finalCurve.clear();
        finalCurveRight.clear();
        ashPreviewCurve.clear();
        ashPreviewLabel.clear();
        ashCatalogSearch.clear();
        ashCatalogReviewer.clear();
        ashCatalogMeasurement.clear();
        ashCatalogBrand.clear();
        ashCatalogModel.clear();
        ashCatalogSelectedLabel.clear();
        ashPreviewActive.store (false);
        renderedCurve.clear();
        renderedCurveRight.clear();
        nextLayerId = 1;
        activeLayerId = 0;
        averageEnabled = true;
        averageVisible = true;
        blendPerLayerMode = true;
        globalBlend = {};
        copiedEqKind = EqClipboardKind::none;
        presetName = "Untitled";
        editLocked = false;
        lastOpenTabIndex = 0;
        for (auto& visible : layerTypeVisible)
            visible = true;
    }

    if (auto* p = parameters.getParameter ("drywet")) p->setValueNotifyingHost (p->getDefaultValue());
    if (auto* p = parameters.getParameter ("crossfeed")) p->setValueNotifyingHost (p->getDefaultValue());
    if (auto* p = parameters.getParameter ("crossfeedalgorithm")) p->setValueNotifyingHost (p->getDefaultValue());
    if (auto* p = parameters.getParameter ("crossfeedcircumference")) p->setValueNotifyingHost (p->getDefaultValue());
    if (auto* p = parameters.getParameter ("crossfeedheadwidth")) p->setValueNotifyingHost (p->getDefaultValue());
    if (auto* p = parameters.getParameter ("crossfeedheadlength")) p->setValueNotifyingHost (p->getDefaultValue());
    if (auto* p = parameters.getParameter ("crossfeedangle")) p->setValueNotifyingHost (p->getDefaultValue());
    if (auto* p = parameters.getParameter ("crossfeedcutoff")) p->setValueNotifyingHost (p->getDefaultValue());
    if (auto* p = parameters.getParameter ("crossfeeddirect")) p->setValueNotifyingHost (p->getDefaultValue());
    if (auto* p = parameters.getParameter ("gain")) p->setValueNotifyingHost (p->getDefaultValue());
    if (auto* p = parameters.getParameter ("globalbalance")) p->setValueNotifyingHost (p->getDefaultValue());
    if (auto* p = parameters.getParameter ("inputgain")) p->setValueNotifyingHost (p->getDefaultValue());
    if (auto* p = parameters.getParameter ("outputgain")) p->setValueNotifyingHost (p->getDefaultValue());
    if (auto* p = parameters.getParameter ("autogain")) p->setValueNotifyingHost (p->getDefaultValue());
    if (auto* p = parameters.getParameter ("loudnessmatchmode")) p->setValueNotifyingHost (p->getDefaultValue());
    if (auto* p = parameters.getParameter ("includeoutputgainfir")) p->setValueNotifyingHost (p->getDefaultValue());
    if (auto* p = parameters.getParameter ("includeautogainfir")) p->setValueNotifyingHost (p->getDefaultValue());
    if (auto* p = parameters.getParameter ("independentlrpreamp")) p->setValueNotifyingHost (p->getDefaultValue());
    if (auto* p = parameters.getParameter ("phasemode")) p->setValueNotifyingHost (p->getDefaultValue());
    if (auto* p = parameters.getParameter ("limiter")) p->setValueNotifyingHost (p->getDefaultValue());
    if (auto* p = parameters.getParameter ("bypass")) p->setValueNotifyingHost (p->getDefaultValue());

    hasRenderedFir = false;
    firOutdated = false;
    activeImpulsePeakSamples = 0;
    applyActiveLatency (0);
    resetMeters();
    resetAutoGainState();
    markPreviewDirty();
}

void FlexCurveAudioProcessor::setActiveLayerId (int id)
{
    if (! editLocked.load() && id != getActiveLayerId())
        recordUndoState ("active-layer");

    const juce::ScopedLock lock (projectLock);
    if (id == -1 && averageEnabled)
    {
        activeLayerId = -1;
        return;
    }
    if (findLayer (id) != nullptr)
        activeLayerId = id;
    else if (! layers.empty())
        activeLayerId = layers.front().id;
    else
        activeLayerId = 0;
}

int FlexCurveAudioProcessor::getActiveLayerId() const
{
    const juce::ScopedLock lock (projectLock);
    return activeLayerId;
}

bool FlexCurveAudioProcessor::hasEditableActiveLayer() const
{
    const juce::ScopedLock lock (projectLock);
    const auto* layer = findLayer (activeLayerId);
    return layer != nullptr && layer->type == FlexCurveLayerType::eq;
}

void FlexCurveAudioProcessor::setAverageEnabled (bool enabled)
{
    if (editsBlocked())
        return;

    recordUndoState ("average-enabled");
    {
        const juce::ScopedLock lock (projectLock);
        averageEnabled = enabled;
        if (! enabled)
        {
            if (activeLayerId == -1)
                activeLayerId = layers.empty() ? 0 : layers.front().id;
        }
        finalCurve = calculateFinalCurveLocked();
    }
    markPreviewDirty();
}

bool FlexCurveAudioProcessor::isAverageEnabled() const { const juce::ScopedLock lock (projectLock); return averageEnabled; }

void FlexCurveAudioProcessor::setAverageVisible (bool visible)
{
    if (! editLocked.load())
        recordUndoState ("average-visible");
    const juce::ScopedLock lock (projectLock);
    averageVisible = visible;
}

bool FlexCurveAudioProcessor::isAverageVisible() const { const juce::ScopedLock lock (projectLock); return averageVisible; }

std::vector<FlexCurveLayer> FlexCurveAudioProcessor::getLayers() const
{
    const juce::ScopedLock lock (projectLock);
    return layers;
}

std::vector<CurvePoint> FlexCurveAudioProcessor::getFinalCurve() const
{
    const juce::ScopedLock lock (projectLock);
    return finalCurve;
}

std::vector<CurvePoint> FlexCurveAudioProcessor::getAshPreviewCurve() const
{
    const juce::ScopedLock lock (projectLock);
    return ashPreviewCurve;
}

juce::String FlexCurveAudioProcessor::getAshPreviewLabel() const
{
    const juce::ScopedLock lock (projectLock);
    return ashPreviewLabel;
}

void FlexCurveAudioProcessor::setAshPreviewCurve (const juce::String& label, std::vector<CurvePoint> points)
{
    {
        const juce::ScopedLock lock (projectLock);
        ashPreviewLabel = label;
        ashPreviewCurve = std::move (points);
    }
    ashPreviewActive.store (true);
    updateFixedAutoGainFromCurrentCurve();
    previewFiltersDirty.store (true);
    if (! restoringState.load())
        triggerAsyncUpdate();
}

void FlexCurveAudioProcessor::clearAshPreviewCurve()
{
    {
        const juce::ScopedLock lock (projectLock);
        ashPreviewLabel.clear();
        ashPreviewCurve.clear();
    }
    ashPreviewActive.store (false);
    updateFixedAutoGainFromCurrentCurve();
    previewFiltersDirty.store (true);
    if (! restoringState.load())
        triggerAsyncUpdate();
}

void FlexCurveAudioProcessor::setAshCatalogBrowserState (const juce::String& search,
                                                          const juce::String& reviewer,
                                                          const juce::String& measurement,
                                                          const juce::String& brand,
                                                          const juce::String& model,
                                                          const juce::String& selectedLabel)
{
    const juce::ScopedLock lock (projectLock);
    ashCatalogSearch = search;
    ashCatalogReviewer = reviewer;
    ashCatalogMeasurement = measurement;
    ashCatalogBrand = brand;
    ashCatalogModel = model;
    ashCatalogSelectedLabel = selectedLabel;
}

juce::StringArray FlexCurveAudioProcessor::getAshCatalogBrowserState() const
{
    const juce::ScopedLock lock (projectLock);
    return { ashCatalogSearch, ashCatalogReviewer, ashCatalogMeasurement,
             ashCatalogBrand, ashCatalogModel, ashCatalogSelectedLabel };
}

std::vector<CurvePoint> FlexCurveAudioProcessor::getRenderedCurve() const
{
    const juce::ScopedLock lock (projectLock);
    return renderedCurve;
}

FlexCurveLayer* FlexCurveAudioProcessor::findLayer (int id)
{
    const auto it = std::find_if (layers.begin(), layers.end(), [id] (const auto& layer) { return layer.id == id; });
    return it == layers.end() ? nullptr : &(*it);
}

const FlexCurveLayer* FlexCurveAudioProcessor::findLayer (int id) const
{
    const auto it = std::find_if (layers.begin(), layers.end(), [id] (const auto& layer) { return layer.id == id; });
    return it == layers.end() ? nullptr : &(*it);
}

FlexCurveLayer* FlexCurveAudioProcessor::getActiveLayer()
{
    return findLayer (activeLayerId);
}

const FlexCurveLayer* FlexCurveAudioProcessor::getActiveLayer() const
{
    return findLayer (activeLayerId);
}

std::vector<CurvePoint> FlexCurveAudioProcessor::getLayerCurve (int id) const
{
    const juce::ScopedLock lock (projectLock);
    if (const auto* layer = findLayer (id))
        return calculateLayerCurveLocked (*layer, getDisplayChannelLocked (*layer));
    return {};
}

std::vector<CurvePoint> FlexCurveAudioProcessor::getLayerCurve (
    int id, FlexChannelSelection channel) const
{
    const juce::ScopedLock lock (projectLock);
    if (const auto* layer = findLayer (id))
        return calculateLayerCurveLocked (*layer, channel);
    return {};
}

std::vector<CurvePoint> FlexCurveAudioProcessor::getLayerCurveForDisplay (int id) const
{
    const juce::ScopedLock lock (projectLock);
    const auto* layer = findLayer (id);
    if (layer == nullptr)
        return {};

    auto curve = calculateLayerCurveLocked (*layer, getDisplayChannelLocked (*layer));
    if (layer->type == FlexCurveLayerType::raw || layer->type == FlexCurveLayerType::target)
    {
        const auto displayOffset = getReferenceDisplayOffsetLocked (
            id, getDisplayChannelLocked (*layer));
        for (auto& point : curve)
            point.db += displayOffset;
    }
    return curve;
}

std::vector<CurvePoint> FlexCurveAudioProcessor::getLayerCurveForDisplay (
    int id, FlexChannelSelection channel) const
{
    const juce::ScopedLock lock (projectLock);
    const auto* layer = findLayer (id);
    if (layer == nullptr)
        return {};
    auto curve = calculateLayerCurveLocked (*layer, channel);
    if (layer->type == FlexCurveLayerType::raw || layer->type == FlexCurveLayerType::target)
    {
        const auto displayOffset = getReferenceDisplayOffsetLocked (id, channel);
        for (auto& point : curve)
            point.db += displayOffset;
    }
    return curve;
}

std::vector<CurvePoint> FlexCurveAudioProcessor::getLayerCurveWithoutFreeform (int id) const
{
    const juce::ScopedLock lock (projectLock);
    if (const auto* layer = findLayer (id))
        return calculateLayerCurveWithoutFreeformLocked (*layer, getDisplayChannelLocked (*layer));
    return {};
}

std::vector<CurvePoint> FlexCurveAudioProcessor::getAverageCurve() const
{
    const juce::ScopedLock lock (projectLock);
    return finalCurve;
}

std::vector<CurvePoint> FlexCurveAudioProcessor::getAverageCurve (
    FlexChannelSelection channel) const
{
    const juce::ScopedLock lock (projectLock);
    return calculateAverageCurveLocked (
        channel == FlexChannelSelection::right ? FlexChannelSelection::right
                                                : FlexChannelSelection::left);
}

bool FlexCurveAudioProcessor::averageChannelsDiffer() const
{
    const juce::ScopedLock lock (projectLock);
    const auto left = calculateAverageCurveLocked (FlexChannelSelection::left);
    const auto right = calculateAverageCurveLocked (FlexChannelSelection::right);
    if (left.size() != right.size())
        return true;
    for (size_t i = 0; i < left.size(); ++i)
        if (std::abs (left[i].db - right[i].db) > 1.0e-4)
            return true;
    return false;
}

std::vector<CurvePoint> FlexCurveAudioProcessor::getAverageCurveForType (FlexCurveLayerType type) const
{
    const juce::ScopedLock lock (projectLock);
    return calculateAverageCurveForTypeLocked (type);
}

std::vector<CurvePoint> FlexCurveAudioProcessor::getAverageCurveForType (
    FlexCurveLayerType type, FlexChannelSelection channel) const
{
    const juce::ScopedLock lock (projectLock);
    return calculateAverageCurveForTypeLocked (type, false, channel);
}

std::vector<CurvePoint> FlexCurveAudioProcessor::getAverageCurveForTypeForDisplay (FlexCurveLayerType type) const
{
    const juce::ScopedLock lock (projectLock);
    return calculateAverageCurveForTypeLocked (type, true);
}

std::vector<CurvePoint> FlexCurveAudioProcessor::getAverageCurveForTypeForDisplay (
    FlexCurveLayerType type, FlexChannelSelection channel) const
{
    const juce::ScopedLock lock (projectLock);
    return calculateAverageCurveForTypeLocked (type, true, channel);
}

std::vector<CurvePoint> FlexCurveAudioProcessor::calculateCorrectedMeasurementLocked (
    const FlexCurveLayer& eqLayer, FlexChannelSelection channel) const
{
    const auto* raw = findLayer (eqLayer.autoEqRawLayerId);
    if (raw == nullptr || raw->type != FlexCurveLayerType::raw)
        return {};

    const auto rawCurve = calculateLayerCurveLocked (*raw, channel);
    const auto eqCurve = calculateLayerCurveLocked (eqLayer, channel);
    auto corrected = makeDefaultGrid();
    const auto correctionActive = isEqLayerAudibleLocked (eqLayer)
                               && parameters.getRawParameterValue ("bypass")->load() < 0.5f;
    for (auto& point : corrected)
        point.db = CurveFIR::interpolateDb (rawCurve, point.frequency)
                 + (correctionActive ? CurveFIR::interpolateDb (eqCurve, point.frequency) : 0.0);
    return corrected;
}

const FlexCurveLayer* FlexCurveAudioProcessor::findAutoEqDisplayContextLocked (int referenceLayerId) const
{
    const auto matchesReference = [referenceLayerId] (const auto& layer)
    {
        return layer.type == FlexCurveLayerType::eq
            && layer.autoEqReferenceDisplayEnabled
            && (layer.autoEqRawLayerId == referenceLayerId
                || layer.autoEqTargetLayerId == referenceLayerId);
    };

    if (const auto* active = findLayer (activeLayerId); active != nullptr && matchesReference (*active))
        return active;

    const auto it = std::find_if (layers.rbegin(), layers.rend(), matchesReference);
    return it == layers.rend() ? nullptr : &(*it);
}

float FlexCurveAudioProcessor::getReferenceDisplayOffsetLocked (
    int referenceLayerId, FlexChannelSelection channel) const
{
    if (const auto* context = findAutoEqDisplayContextLocked (referenceLayerId))
        return channel == FlexChannelSelection::right && ! context->channelsLinked
            ? context->right.autoEqReferenceOffsetDb : context->autoEqReferenceOffsetDb;
    return 0.0f;
}

bool FlexCurveAudioProcessor::isEqLayerAudibleLocked (const FlexCurveLayer& layer) const
{
    if (layer.type != FlexCurveLayerType::eq || ! layer.enabled || layer.muted || layer.opacity <= 0.0f)
        return false;

    const auto anySolo = std::any_of (layers.begin(), layers.end(), [] (const auto& candidate)
    {
        return candidate.type == FlexCurveLayerType::eq && candidate.solo;
    });
    return ! anySolo || layer.solo;
}

void FlexCurveAudioProcessor::markLinkedAutoEqLayersOutdatedLocked (int referenceLayerId)
{
    for (auto& layer : layers)
        if (layer.type == FlexCurveLayerType::eq
            && (layer.autoEqRawLayerId == referenceLayerId || layer.autoEqTargetLayerId == referenceLayerId))
        {
            layer.autoEqSourcesOutdated = true;
            layer.right.autoEqSourcesOutdated = true;
        }
}

std::vector<CurvePoint> FlexCurveAudioProcessor::getCorrectedMeasurementCurve (int eqLayerId) const
{
    const juce::ScopedLock lock (projectLock);
    if (const auto* layer = findLayer (eqLayerId))
        return calculateCorrectedMeasurementLocked (*layer, getDisplayChannelLocked (*layer));
    return {};
}

std::vector<CurvePoint> FlexCurveAudioProcessor::getCorrectedMeasurementCurve (
    int eqLayerId, FlexChannelSelection channel) const
{
    const juce::ScopedLock lock (projectLock);
    if (const auto* layer = findLayer (eqLayerId))
        return calculateCorrectedMeasurementLocked (*layer, channel);
    return {};
}

double FlexCurveAudioProcessor::getAutoEqResidualRmsDb (int eqLayerId) const
{
    const juce::ScopedLock lock (projectLock);
    const auto* eq = findLayer (eqLayerId);
    if (eq == nullptr)
        return 0.0;
    const auto* target = findLayer (eq->autoEqTargetLayerId);
    if (target == nullptr || target->type != FlexCurveLayerType::target)
        return 0.0;

    const auto channel = getDisplayChannelLocked (*eq);
    const auto corrected = calculateCorrectedMeasurementLocked (*eq, channel);
    auto targetCurve = calculateLayerCurveLocked (*target, channel);
    const auto useRight = channel == FlexChannelSelection::right && ! eq->channelsLinked;
    const auto referenceDisplayEnabled = useRight
        ? eq->right.autoEqReferenceDisplayEnabled : eq->autoEqReferenceDisplayEnabled;
    const auto referenceOffset = useRight
        ? eq->right.autoEqReferenceOffsetDb : eq->autoEqReferenceOffsetDb;
    if (referenceDisplayEnabled)
        for (auto& point : targetCurve)
            point.db += referenceOffset;
    if (corrected.empty() || targetCurve.empty())
        return 0.0;

    double squared = 0.0;
    int count = 0;
    for (const auto& point : corrected)
        if (point.frequency >= 20.0 && point.frequency <= 16000.0)
        {
            const auto error = point.db - CurveFIR::interpolateDb (targetCurve, point.frequency);
            squared += error * error;
            ++count;
        }
    return count > 0 ? std::sqrt (squared / count) : 0.0;
}

void FlexCurveAudioProcessor::setRegionSettings (float bassDb, float midDb, float trebleDb, float lowMidHz, float midHighHz)
{
    if (editsBlocked())
        return;

    recordUndoState ("blend-region");
    {
        const juce::ScopedLock lock (projectLock);
        auto* active = getActiveLayer();
        if (blendPerLayerMode && active != nullptr)
        {
            const auto range = globalDbRange.load();
            applyToSelectedChannelsLocked (*active, [=] (auto& channel)
            {
                channel.blend.bassGainDb = juce::jlimit (-range, range, bassDb);
                channel.blend.midGainDb = juce::jlimit (-range, range, midDb);
                channel.blend.trebleGainDb = juce::jlimit (-range, range, trebleDb);
                channel.blend.lowMidCrossoverHz = juce::jlimit (60.0f, 1200.0f, lowMidHz);
                channel.blend.midHighCrossoverHz = juce::jlimit (1200.0f, 12000.0f, midHighHz);
                if (channel.blend.midHighCrossoverHz <= channel.blend.lowMidCrossoverHz * 1.5f)
                    channel.blend.midHighCrossoverHz = channel.blend.lowMidCrossoverHz * 1.5f;
            });
            finalCurve = calculateFinalCurveLocked();
        }
        else
        {
            auto& target = globalBlend;
            const auto range = globalDbRange.load();
            target.bassGainDb = juce::jlimit (-range, range, bassDb);
            target.midGainDb = juce::jlimit (-range, range, midDb);
            target.trebleGainDb = juce::jlimit (-range, range, trebleDb);
            target.lowMidCrossoverHz = juce::jlimit (60.0f, 1200.0f, lowMidHz);
            target.midHighCrossoverHz = juce::jlimit (1200.0f, 12000.0f, midHighHz);
            if (target.midHighCrossoverHz <= target.lowMidCrossoverHz * 1.5f)
                target.midHighCrossoverHz = target.lowMidCrossoverHz * 1.5f;
        }
        finalCurve = calculateFinalCurveLocked();
    }
    markPreviewDirty();
}

void FlexCurveAudioProcessor::getRegionSettings (float& bassDb, float& midDb, float& trebleDb, float& lowMidHz, float& midHighHz) const
{
    const juce::ScopedLock lock (projectLock);
    const auto* active = getActiveLayer();
    FlexCurveLayer rightView;
    const FlexBlendSettings* source = &globalBlend;
    if (blendPerLayerMode && active != nullptr)
    {
        if (active->selectedChannel == FlexChannelSelection::right && ! active->channelsLinked)
        {
            loadRightChannelIntoLayerView (*active, rightView);
            source = &rightView.blend;
        }
        else
        {
            source = &active->blend;
        }
    }
    bassDb = source->bassGainDb;
    midDb = source->midGainDb;
    trebleDb = source->trebleGainDb;
    lowMidHz = source->lowMidCrossoverHz;
    midHighHz = source->midHighCrossoverHz;
}

void FlexCurveAudioProcessor::setBlendPerLayerMode (bool perLayer)
{
    if (editsBlocked())
        return;

    recordUndoState ("blend-mode");
    {
        const juce::ScopedLock lock (projectLock);
        blendPerLayerMode = perLayer;
        finalCurve = calculateFinalCurveLocked();
    }
    markPreviewDirty();
}

bool FlexCurveAudioProcessor::isBlendPerLayerMode() const
{
    const juce::ScopedLock lock (projectLock);
    return blendPerLayerMode;
}

void FlexCurveAudioProcessor::resetActiveLayerBlendToFlat()
{
    if (editsBlocked())
        return;

    recordUndoState ("reset-active-blend");
    {
        const juce::ScopedLock lock (projectLock);
        if (blendPerLayerMode)
        {
            if (auto* layer = getActiveLayer())
            {
                layer->balanceDb = 0.0f;
                applyToSelectedChannelsLocked (*layer, [] (auto& channel)
                {
                    channel.points = makeDefaultGrid();
                    channel.gainDb = 0.0f;
                    channel.blend = {};
                    channel.graphic15Gains.fill (0.0f);
                    channel.graphic31Gains.fill (0.0f);
                    channel.freeformPoints.clear();
                    channel.paramBands.assign (8, {});
                    channel.graphicEnabled = false;
                    channel.preserveVariableShapeAcrossModes = false;
                    channel.smoothGraphicCurve = false;
                });
            }
        }
        else
        {
            globalBlend = {};
        }
        finalCurve = calculateFinalCurveLocked();
    }
    markPreviewDirty();
}

void FlexCurveAudioProcessor::setGraphicMode31 (bool mode31) { setGraphicMode (mode31 ? 31 : 15); }

void FlexCurveAudioProcessor::setGraphicMode (int mode)
{
    if (editsBlocked())
        return;

    recordUndoState ("graphic-mode");
    {
        const juce::ScopedLock lock (projectLock);
        if (auto* layer = getActiveLayer())
            applyToSelectedChannelsLocked (*layer, [mode] (auto& channel)
            {
                channel.graphicMode = (mode == 0 ? 0 : (mode == 15 ? 15 : 31));
            });
        finalCurve = calculateFinalCurveLocked();
    }
    markPreviewDirty();
}

void FlexCurveAudioProcessor::setGraphicEnabled (bool enabled)
{
    if (editsBlocked())
        return;

    recordUndoState ("graphic-enabled");
    {
        const juce::ScopedLock lock (projectLock);
        if (auto* layer = getActiveLayer())
            applyToSelectedChannelsLocked (*layer, [enabled] (auto& channel)
            {
                channel.graphicEnabled = enabled;
            });
        finalCurve = calculateFinalCurveLocked();
    }
    markPreviewDirty();
}

void FlexCurveAudioProcessor::setPreserveVariableShapeAcrossModes (bool preserve)
{
    if (editsBlocked())
        return;

    recordUndoState ("variable-preserve");
    {
        const juce::ScopedLock lock (projectLock);
        if (auto* layer = getActiveLayer())
            applyToSelectedChannelsLocked (*layer, [preserve] (auto& channel)
            {
                channel.preserveVariableShapeAcrossModes = preserve;
            });
        finalCurve = calculateFinalCurveLocked();
    }
    markPreviewDirty();
}

bool FlexCurveAudioProcessor::isPreserveVariableShapeAcrossModes() const
{
    const juce::ScopedLock lock (projectLock);
    if (const auto* layer = getActiveLayer())
    {
        if (layer->selectedChannel == FlexChannelSelection::right && ! layer->channelsLinked)
            return layer->right.preserveVariableShapeAcrossModes;
        return layer->preserveVariableShapeAcrossModes;
    }
    return false;
}

void FlexCurveAudioProcessor::setGraphicSmoothing (bool enabled)
{
    if (editsBlocked())
        return;

    recordUndoState ("graphic-smoothing");
    {
        const juce::ScopedLock lock (projectLock);
        if (auto* layer = getActiveLayer())
            applyToSelectedChannelsLocked (*layer, [enabled] (auto& channel)
            {
                channel.smoothGraphicCurve = enabled;
            });
        finalCurve = calculateFinalCurveLocked();
    }
    markPreviewDirty();
}

bool FlexCurveAudioProcessor::isGraphicSmoothingEnabled() const
{
    const juce::ScopedLock lock (projectLock);
    if (const auto* layer = getActiveLayer())
        return layer->selectedChannel == FlexChannelSelection::right && ! layer->channelsLinked
            ? layer->right.smoothGraphicCurve : layer->smoothGraphicCurve;
    return false;
}

void FlexCurveAudioProcessor::setAllGraphicSmoothing (bool enabled)
{
    if (editsBlocked())
        return;

    recordUndoState ("smooth-all-curves");
    {
        const juce::ScopedLock lock (projectLock);
        for (auto& layer : layers)
        {
            const auto selection = layer.selectedChannel;
            layer.selectedChannel = FlexChannelSelection::stereo;
            applyToSelectedChannelsLocked (layer, [enabled] (auto& channel)
            {
                channel.smoothGraphicCurve = enabled;
                channel.smoothSourceCurve = enabled;
            });
            layer.selectedChannel = selection;
        }
        for (const auto& layer : layers)
            if (layer.type != FlexCurveLayerType::eq)
                markLinkedAutoEqLayersOutdatedLocked (layer.id);
        finalCurve = calculateFinalCurveLocked();
    }
    markPreviewDirty();
}

bool FlexCurveAudioProcessor::areAllGraphicCurvesSmoothed() const
{
    const juce::ScopedLock lock (projectLock);
    return ! layers.empty() && std::all_of (layers.begin(), layers.end(), [] (const auto& layer)
    {
        return layer.smoothGraphicCurve && layer.smoothSourceCurve
            && (layer.channelsLinked
                || (layer.right.smoothGraphicCurve && layer.right.smoothSourceCurve));
    });
}

bool FlexCurveAudioProcessor::isGraphicMode31() const { return getGraphicMode() == 31; }

int FlexCurveAudioProcessor::getGraphicMode() const
{
    const juce::ScopedLock lock (projectLock);
    if (const auto* layer = getActiveLayer())
        return layer->selectedChannel == FlexChannelSelection::right && ! layer->channelsLinked
            ? layer->right.graphicMode : layer->graphicMode;
    return 31;
}

bool FlexCurveAudioProcessor::isGraphicEnabled() const
{
    const juce::ScopedLock lock (projectLock);
    if (const auto* layer = getActiveLayer())
        return layer->selectedChannel == FlexChannelSelection::right && ! layer->channelsLinked
            ? layer->right.graphicEnabled : layer->graphicEnabled;
    return false;
}

int FlexCurveAudioProcessor::getGraphicBandCount() const { return getGraphicMode() == 31 ? 31 : 15; }

double FlexCurveAudioProcessor::getGraphicBandFrequency (int index) const
{
    if (getGraphicMode() == 31)
        return graphic31Frequencies()[static_cast<size_t> (juce::jlimit (0, 30, index))];
    return graphic15Frequencies()[static_cast<size_t> (juce::jlimit (0, 14, index))];
}

float FlexCurveAudioProcessor::getGraphicGain (int index) const
{
    const juce::ScopedLock lock (projectLock);
    if (const auto* layer = getActiveLayer())
    {
        const auto right = layer->selectedChannel == FlexChannelSelection::right && ! layer->channelsLinked;
        const auto mode = right ? layer->right.graphicMode : layer->graphicMode;
        if (mode == 15)
            return right ? layer->right.graphic15Gains[static_cast<size_t> (juce::jlimit (0, 14, index))]
                         : layer->graphic15Gains[static_cast<size_t> (juce::jlimit (0, 14, index))];
        if (mode == 31)
            return right ? layer->right.graphic31Gains[static_cast<size_t> (juce::jlimit (0, 30, index))]
                         : layer->graphic31Gains[static_cast<size_t> (juce::jlimit (0, 30, index))];
    }
    return 0.0f;
}

void FlexCurveAudioProcessor::setGraphicGain (int index, float gainDb)
{
    if (editsBlocked())
        return;

    recordUndoState ("graphic-gain-" + juce::String (index));
    {
        const juce::ScopedLock lock (projectLock);
        if (auto* layer = getActiveLayer())
        {
            const auto limited = juce::jlimit (-globalDbRange.load(), globalDbRange.load(), gainDb);
            applyToSelectedChannelsLocked (*layer, [=] (auto& channel)
            {
                if (channel.graphicMode == 15)
                    channel.graphic15Gains[static_cast<size_t> (juce::jlimit (0, 14, index))] = limited;
                else if (channel.graphicMode == 31)
                    channel.graphic31Gains[static_cast<size_t> (juce::jlimit (0, 30, index))] = limited;
            });
        }
        finalCurve = calculateFinalCurveLocked();
    }
    markPreviewDirty();
}

void FlexCurveAudioProcessor::resetGraphic()
{
    if (editsBlocked())
        return;

    recordUndoState ("reset-graphic");
    {
        const juce::ScopedLock lock (projectLock);
        if (auto* layer = getActiveLayer())
        {
            applyToSelectedChannelsLocked (*layer, [] (auto& channel)
            {
                if (channel.graphicMode == 15)
                    channel.graphic15Gains.fill (0.0f);
                else if (channel.graphicMode == 31)
                    channel.graphic31Gains.fill (0.0f);
                else
                    channel.freeformPoints.clear();
            });
        }
        finalCurve = calculateFinalCurveLocked();
    }
    markPreviewDirty();
}

std::vector<FlexParamBand> FlexCurveAudioProcessor::getParamBands() const
{
    const juce::ScopedLock lock (projectLock);
    if (const auto* layer = getActiveLayer())
        return layer->selectedChannel == FlexChannelSelection::right && ! layer->channelsLinked
            ? layer->right.paramBands : layer->paramBands;
    return std::vector<FlexParamBand> (8);
}

void FlexCurveAudioProcessor::addParamBand()
{
    if (editsBlocked())
        return;

    recordUndoState ("add-param-band");
    {
        const juce::ScopedLock lock (projectLock);
        if (auto* layer = getActiveLayer())
        {
            applyToSelectedChannelsLocked (*layer, [] (auto& channel)
            {
                FlexParamBand band;
                const auto index = static_cast<int> (channel.paramBands.size());
                band.frequency = static_cast<float> (
                    juce::jlimit (20.0, 20000.0, 80.0 * std::pow (1.55, index)));
                channel.paramBands.push_back (band);
            });
            finalCurve = calculateFinalCurveLocked();
        }
    }
    markPreviewDirty();
}

void FlexCurveAudioProcessor::removeParamBand (int index)
{
    if (editsBlocked())
        return;

    if (index < 0)
        return;

    recordUndoState ("remove-param-band");
    {
        const juce::ScopedLock lock (projectLock);
        if (auto* layer = getActiveLayer())
        {
            applyToSelectedChannelsLocked (*layer, [index] (auto& channel)
            {
                if (index < static_cast<int> (channel.paramBands.size()))
                    channel.paramBands.erase (channel.paramBands.begin() + index);
            });
            finalCurve = calculateFinalCurveLocked();
        }
    }
    markPreviewDirty();
}

void FlexCurveAudioProcessor::setParamBand (int index, const FlexParamBand& band)
{
    if (editsBlocked())
        return;

    if (index < 0)
        return;
    recordUndoState ("param-band-" + juce::String (index));
    {
        const juce::ScopedLock lock (projectLock);
        if (auto* layer = getActiveLayer())
        {
            auto safeBand = band;
            safeBand.frequency = juce::jlimit (20.0f, 20000.0f, safeBand.frequency);
            safeBand.gainDb = juce::jlimit (-globalDbRange.load(), globalDbRange.load(), safeBand.gainDb);
            safeBand.q = juce::jlimit (0.05f, 33.3333f, safeBand.q);
            applyToSelectedChannelsLocked (*layer, [=] (auto& channel)
            {
                if (index >= static_cast<int> (channel.paramBands.size()))
                    channel.paramBands.resize (static_cast<size_t> (index + 1));
                channel.paramBands[static_cast<size_t> (index)] = safeBand;
            });
            finalCurve = calculateFinalCurveLocked();
        }
    }
    markPreviewDirty();
}

void FlexCurveAudioProcessor::resetParametric()
{
    if (editsBlocked())
        return;

    recordUndoState ("reset-parametric");
    {
        const juce::ScopedLock lock (projectLock);
        if (auto* layer = getActiveLayer())
            applyToSelectedChannelsLocked (*layer, [] (auto& channel)
            {
                channel.paramBands.assign (8, {});
            });
        finalCurve = calculateFinalCurveLocked();
    }
    markPreviewDirty();
}

std::vector<CurvePoint> FlexCurveAudioProcessor::getFreeformPoints() const
{
    const juce::ScopedLock lock (projectLock);
    if (const auto* layer = getActiveLayer())
        return layer->selectedChannel == FlexChannelSelection::right && ! layer->channelsLinked
            ? layer->right.freeformPoints : layer->freeformPoints;
    return {};
}

void FlexCurveAudioProcessor::setFreeformPoints (std::vector<CurvePoint> points)
{
    if (editsBlocked())
        return;

    recordUndoState ("variable-points");
    const auto range = static_cast<double> (globalDbRange.load());
    for (auto& point : points)
    {
        point.frequency = juce::jlimit (20.0, 20000.0, point.frequency);
        point.db = juce::jlimit (-range, range, point.db);
    }
    std::sort (points.begin(), points.end(), [] (const auto& a, const auto& b) { return a.frequency < b.frequency; });
    {
        const juce::ScopedLock lock (projectLock);
        if (auto* layer = getActiveLayer())
            applyToSelectedChannelsLocked (*layer, [&points] (auto& channel)
            {
                channel.freeformPoints = points;
            });
        finalCurve = calculateFinalCurveLocked();
    }
    markPreviewDirty();
}

void FlexCurveAudioProcessor::resetFreeform()
{
    if (editsBlocked())
        return;

    recordUndoState ("reset-freeform");
    {
        const juce::ScopedLock lock (projectLock);
        if (auto* layer = getActiveLayer())
            applyToSelectedChannelsLocked (*layer, [] (auto& channel)
            {
                channel.freeformPoints.clear();
            });
        finalCurve = calculateFinalCurveLocked();
    }
    markPreviewDirty();
}

void FlexCurveAudioProcessor::copyCurrentGraphicEq()
{
    const juce::ScopedLock lock (projectLock);
    if (const auto* layer = getActiveLayer())
    {
        auto view = *layer;
        if (layer->selectedChannel == FlexChannelSelection::right && ! layer->channelsLinked)
            loadRightChannelIntoLayerView (*layer, view);
        copiedEqSourceLayerId = layer->id;
        copiedEqSourceChannel = layer->selectedChannel;
        copiedEqKind = view.graphicMode == 15 ? EqClipboardKind::graphic15
                     : view.graphicMode == 31 ? EqClipboardKind::graphic31
                                                : EqClipboardKind::variable;
        copiedSmoothGraphicCurve = view.smoothGraphicCurve;
        if (copiedEqKind == EqClipboardKind::graphic15)
            copiedGraphic15Gains = view.graphic15Gains;
        else if (copiedEqKind == EqClipboardKind::graphic31)
            copiedGraphic31Gains = view.graphic31Gains;
        else
            copiedFreeformPoints = view.freeformPoints;
    }
}

bool FlexCurveAudioProcessor::canPasteCurrentGraphicEq() const
{
    const juce::ScopedLock lock (projectLock);
    const auto* layer = getActiveLayer();
    if (layer == nullptr)
        return false;
    if (copiedEqSourceLayerId == layer->id
        && copiedEqSourceChannel == layer->selectedChannel)
        return false;

    const auto mode = layer->selectedChannel == FlexChannelSelection::right && ! layer->channelsLinked
        ? layer->right.graphicMode : layer->graphicMode;
    if (mode == 0)
        return copiedEqKind == EqClipboardKind::graphic15
            || copiedEqKind == EqClipboardKind::graphic31
            || copiedEqKind == EqClipboardKind::variable;
    if (mode == 31)
        return copiedEqKind == EqClipboardKind::graphic15
            || copiedEqKind == EqClipboardKind::graphic31;
    return copiedEqKind == EqClipboardKind::graphic15;
}

void FlexCurveAudioProcessor::pasteCurrentGraphicEq (bool inverted)
{
    if (editsBlocked())
        return;

    if (! canPasteCurrentGraphicEq())
        return;

    recordUndoState (inverted ? "paste-inverted-graphic-eq" : "paste-graphic-eq");
    {
        const juce::ScopedLock lock (projectLock);
        if (auto* layer = getActiveLayer())
        {
            const auto sign = inverted ? -1.0f : 1.0f;
            applyToSelectedChannelsLocked (*layer, [this, sign] (auto& channel)
            {
                channel.graphicEnabled = true;
                channel.smoothGraphicCurve = copiedSmoothGraphicCurve;
                if (channel.graphicMode == 15)
                {
                    for (size_t i = 0; i < channel.graphic15Gains.size(); ++i)
                        channel.graphic15Gains[i] = sign * copiedGraphic15Gains[i];
                }
                else if (channel.graphicMode == 31)
                {
                    if (copiedEqKind == EqClipboardKind::graphic31)
                    {
                        for (size_t i = 0; i < channel.graphic31Gains.size(); ++i)
                            channel.graphic31Gains[i] = sign * copiedGraphic31Gains[i];
                    }
                    else
                    {
                        std::vector<CurvePoint> source;
                        source.reserve (copiedGraphic15Gains.size());
                        const auto frequencies = graphic15Frequencies();
                        for (size_t i = 0; i < copiedGraphic15Gains.size(); ++i)
                            source.push_back ({ frequencies[i], sign * copiedGraphic15Gains[i] });
                        const auto destinationFrequencies = graphic31Frequencies();
                        for (size_t i = 0; i < channel.graphic31Gains.size(); ++i)
                            channel.graphic31Gains[i] = static_cast<float> (
                                CurveFIR::interpolateDb (source, destinationFrequencies[i]));
                    }
                }
                else
                {
                    channel.freeformPoints.clear();
                    if (copiedEqKind == EqClipboardKind::variable)
                    {
                        channel.freeformPoints = copiedFreeformPoints;
                        for (auto& point : channel.freeformPoints)
                            point.db *= sign;
                    }
                    else
                    {
                        const auto count = copiedEqKind == EqClipboardKind::graphic31 ? 31 : 15;
                        channel.freeformPoints.reserve (static_cast<size_t> (count));
                        for (int i = 0; i < count; ++i)
                        {
                            const auto frequency = copiedEqKind == EqClipboardKind::graphic31
                                ? graphic31Frequencies()[static_cast<size_t> (i)]
                                : graphic15Frequencies()[static_cast<size_t> (i)];
                            const auto gain = copiedEqKind == EqClipboardKind::graphic31
                                ? copiedGraphic31Gains[static_cast<size_t> (i)]
                                : copiedGraphic15Gains[static_cast<size_t> (i)];
                            channel.freeformPoints.push_back ({ frequency, sign * gain });
                        }
                    }
                }
            });

            finalCurve = calculateFinalCurveLocked();
        }
    }
    markPreviewDirty();
}

void FlexCurveAudioProcessor::copyCurrentParametricEq()
{
    const juce::ScopedLock lock (projectLock);
    if (const auto* layer = getActiveLayer())
    {
        copiedEqKind = EqClipboardKind::parametric;
        copiedEqSourceLayerId = layer->id;
        copiedEqSourceChannel = layer->selectedChannel;
        copiedParamBands = layer->selectedChannel == FlexChannelSelection::right && ! layer->channelsLinked
            ? layer->right.paramBands : layer->paramBands;
    }
}

bool FlexCurveAudioProcessor::canPasteCurrentParametricEq() const
{
    const juce::ScopedLock lock (projectLock);
    const auto* layer = getActiveLayer();
    return copiedEqKind == EqClipboardKind::parametric && layer != nullptr
        && (copiedEqSourceLayerId != layer->id
            || copiedEqSourceChannel != layer->selectedChannel);
}

void FlexCurveAudioProcessor::pasteCurrentParametricEq (bool inverted)
{
    if (editsBlocked() || ! canPasteCurrentParametricEq())
        return;

    recordUndoState (inverted ? "paste-inverted-parametric-eq" : "paste-parametric-eq");
    {
        const juce::ScopedLock lock (projectLock);
        if (auto* layer = getActiveLayer())
        {
            applyToSelectedChannelsLocked (*layer, [this, inverted] (auto& channel)
            {
                channel.paramBands = copiedParamBands;
                if (channel.paramBands.empty())
                    channel.paramBands.resize (8);
                if (inverted)
                    for (auto& band : channel.paramBands)
                        band.gainDb = -band.gainDb;
            });
            finalCurve = calculateFinalCurveLocked();
        }
    }
    markPreviewDirty();
}

void FlexCurveAudioProcessor::markPreviewDirty()
{
    {
        const juce::ScopedLock lock (projectLock);
        finalCurve = calculateAverageCurveLocked (FlexChannelSelection::left);
        finalCurveRight = calculateAverageCurveLocked (FlexChannelSelection::right);
    }
    updateFixedAutoGainFromCurrentCurve();
    previewFiltersDirty = true;
    if (restoringState.load())
        return;

    triggerAsyncUpdate();
    if (hasRenderedFir)
    {
        firOutdated = true;
        if (auto* phase = parameters.getParameter ("phasemode"))
            phase->setValueNotifyingHost (0.0f);
        applyActiveLatency (0);
    }
}

std::vector<CurvePoint> FlexCurveAudioProcessor::calculateLayerCurveLocked (
    const FlexCurveLayer& layer) const
{
    return calculateLayerCurveLocked (layer, getDisplayChannelLocked (layer));
}

std::vector<CurvePoint> FlexCurveAudioProcessor::calculateLayerCurveLocked (
    const FlexCurveLayer& layer, FlexChannelSelection channel) const
{
    if (channel == FlexChannelSelection::right && ! layer.channelsLinked)
    {
        auto rightView = layer;
        loadRightChannelIntoLayerView (layer, rightView);
        rightView.channelsLinked = true;
        rightView.selectedChannel = FlexChannelSelection::left;
        rightView.balanceDb = -layer.balanceDb;
        return calculateLayerCurveLocked (rightView, FlexChannelSelection::left);
    }

    auto points = makeDefaultGrid();
    auto graphicContribution = makeDefaultGrid();

    for (auto& point : points)
        point.db = CurveFIR::interpolateDb (layer.points, point.frequency);

    if (layer.smoothSourceCurve)
        smoothCurveGeometry (points);

    for (auto& point : points)
        point.db += layer.gainDb + layer.normalizationOffsetDb;

    if (layer.type == FlexCurveLayerType::eq)
    {
        const auto balanceOffset = channelBalanceOffsetDb (layer.balanceDb, channel);
        for (auto& point : points)
            point.db += balanceOffset;
    }

    if (layer.graphicEnabled)
    {
        if (layer.graphicMode != 0)
        {
            std::vector<CurvePoint> graphicPoints;
            const auto count = layer.graphicMode == 31 ? 31 : 15;
            graphicPoints.reserve (static_cast<size_t> (count));
            for (int i = 0; i < count; ++i)
            {
                const auto freq = layer.graphicMode == 31
                    ? graphic31Frequencies()[static_cast<size_t> (i)]
                    : graphic15Frequencies()[static_cast<size_t> (i)];
                const auto gain = layer.graphicMode == 31
                    ? layer.graphic31Gains[static_cast<size_t> (i)]
                    : layer.graphic15Gains[static_cast<size_t> (i)];
                graphicPoints.push_back ({ freq, gain });
            }
            for (auto& contribution : graphicContribution)
                contribution.db += CurveFIR::interpolateDb (graphicPoints, contribution.frequency);
        }

        const auto variableApplies = layer.graphicMode == 0 || layer.preserveVariableShapeAcrossModes;
        if (variableApplies && ! layer.freeformPoints.empty())
            for (auto& contribution : graphicContribution)
                contribution.db += CurveFIR::interpolateDb (layer.freeformPoints, contribution.frequency);
    }

    if (layer.smoothGraphicCurve)
        smoothCurveGeometry (graphicContribution);

    for (size_t i = 0; i < points.size(); ++i)
    {
        auto& point = points[i];
        point.db += graphicContribution[i].db;

        for (const auto& band : layer.paramBands)
            if (band.enabled)
                point.db += getBiquadMagnitudeDb (makeFilter (currentSampleRate, band), currentSampleRate, point.frequency);
    }

    if (layer.type == FlexCurveLayerType::eq && blendPerLayerMode)
        applyRegionTrims (points,
                           layer.blend.bassGainDb,
                           layer.blend.midGainDb,
                           layer.blend.trebleGainDb,
                           layer.blend.lowMidCrossoverHz,
                           layer.blend.midHighCrossoverHz);

    if (layer.type == FlexCurveLayerType::eq && layer.inverted)
        for (auto& point : points)
            point.db = -point.db;

    if (layer.type == FlexCurveLayerType::eq && ! blendPerLayerMode)
        applyRegionTrims (points,
                          globalBlend.bassGainDb,
                          globalBlend.midGainDb,
                          globalBlend.trebleGainDb,
                          globalBlend.lowMidCrossoverHz,
                          globalBlend.midHighCrossoverHz);

    return points;
}

std::vector<CurvePoint> FlexCurveAudioProcessor::calculateLayerCurveWithoutFreeformLocked (
    const FlexCurveLayer& layer, FlexChannelSelection channel) const
{
    auto copy = layer;
    if (channel == FlexChannelSelection::right && ! copy.channelsLinked)
        copy.right.freeformPoints.clear();
    else
        copy.freeformPoints.clear();
    return calculateLayerCurveLocked (copy, channel);
}

std::vector<CurvePoint> FlexCurveAudioProcessor::calculateAverageCurveLocked (
    FlexChannelSelection channel) const
{
    const auto anySolo = std::any_of (layers.begin(), layers.end(), [] (const auto& layer)
    {
        return layer.type == FlexCurveLayerType::eq && layer.solo;
    });
    std::vector<const FlexCurveLayer*> sourceLayers;
    for (const auto& layer : layers)
        if (layer.type == FlexCurveLayerType::eq && ! layer.muted && layer.enabled
            && layer.opacity > 0.0f && (! anySolo || layer.solo))
            sourceLayers.push_back (&layer);

    if (sourceLayers.empty())
        return {};

    std::vector<std::vector<CurvePoint>> sourceCurves;
    sourceCurves.reserve (sourceLayers.size());
    for (const auto* layer : sourceLayers)
        sourceCurves.push_back (calculateLayerCurveLocked (*layer, channel));

    auto points = makeDefaultGrid();
    for (auto& point : points)
    {
        double sum = 0.0;
        double weightSum = 0.0;
        for (size_t i = 0; i < sourceLayers.size(); ++i)
        {
            sum += CurveFIR::interpolateDb (sourceCurves[i], point.frequency) * sourceLayers[i]->opacity;
            weightSum += sourceLayers[i]->opacity;
        }
        point.db = weightSum > 0.0 ? sum / weightSum : 0.0;
    }
    return points;
}

std::vector<CurvePoint> FlexCurveAudioProcessor::calculatePreviewCurveLocked (
    FlexChannelSelection channel) const
{
    juce::ignoreUnused (channel);
    return ashPreviewCurve.empty() ? calculateAverageCurveLocked (channel) : ashPreviewCurve;
}

std::vector<CurvePoint> FlexCurveAudioProcessor::calculateAverageCurveForTypeLocked (
    FlexCurveLayerType type, bool applyReferenceDisplayOffset, FlexChannelSelection channel) const
{
    const auto anySolo = type == FlexCurveLayerType::eq
        && std::any_of (layers.begin(), layers.end(), [] (const auto& layer)
           {
               return layer.type == FlexCurveLayerType::eq && layer.solo;
           });
    std::vector<const FlexCurveLayer*> sourceLayers;
    for (const auto& layer : layers)
        if (layer.type == type && ! layer.muted && layer.enabled && layer.opacity > 0.0f
            && (! anySolo || layer.solo))
            sourceLayers.push_back (&layer);

    if (sourceLayers.empty())
        return {};

    std::vector<std::vector<CurvePoint>> sourceCurves;
    sourceCurves.reserve (sourceLayers.size());
    for (const auto* layer : sourceLayers)
    {
        auto curve = calculateLayerCurveLocked (*layer, channel);
        if (applyReferenceDisplayOffset
            && (type == FlexCurveLayerType::raw || type == FlexCurveLayerType::target))
        {
            const auto displayOffset = getReferenceDisplayOffsetLocked (layer->id, channel);
            for (auto& point : curve)
                point.db += displayOffset;
        }
        sourceCurves.push_back (std::move (curve));
    }

    auto points = makeDefaultGrid();
    for (auto& point : points)
    {
        double sum = 0.0;
        double weightSum = 0.0;
        for (size_t i = 0; i < sourceLayers.size(); ++i)
        {
            sum += CurveFIR::interpolateDb (sourceCurves[i], point.frequency) * sourceLayers[i]->opacity;
            weightSum += sourceLayers[i]->opacity;
        }
        point.db = weightSum > 0.0 ? sum / weightSum : 0.0;
    }

    return points;
}

std::vector<CurvePoint> FlexCurveAudioProcessor::calculateFinalCurveLocked() const
{
    return calculateAverageCurveLocked (FlexChannelSelection::left);
}

void FlexCurveAudioProcessor::updateFinalCurve()
{
    {
        const juce::ScopedLock lock (projectLock);
        finalCurve = calculateFinalCurveLocked();
        finalCurveRight = calculateAverageCurveLocked (FlexChannelSelection::right);
    }
    updateFixedAutoGainFromCurrentCurve();
}

void FlexCurveAudioProcessor::renderFir()
{
    if (editsBlocked())
        return;

    recordUndoState ("render-fir");
    updateFinalCurve();

    std::vector<CurvePoint> points;
    std::vector<CurvePoint> rightPoints;
    {
        const juce::ScopedLock lock (projectLock);
        points = calculateAverageCurveLocked (FlexChannelSelection::left);
        rightPoints = calculateAverageCurveLocked (FlexChannelSelection::right);
        renderedCurve = points;
        renderedCurveRight = rightPoints;
    }

    if (points.empty())
        return;

    const auto phaseMode = getPhaseMode();
    const auto taps = getDefaultFirTapsForMode (phaseMode);
    auto impulse = createStereoImpulseForPhaseMode (points, rightPoints, currentSampleRate, taps);

    activeImpulsePeakSamples = findImpulseResponsePeak (impulse);
    applyActiveLatency (getLatencyForPhaseMode (phaseMode, impulse.getNumSamples()));
    convolution.loadImpulseResponse (std::move (impulse),
                                     currentSampleRate,
                                     impulse.getNumChannels() > 1 ? juce::dsp::Convolution::Stereo::yes
                                                                  : juce::dsp::Convolution::Stereo::no,
                                     juce::dsp::Convolution::Trim::no,
                                     juce::dsp::Convolution::Normalise::no);

    {
        const juce::ScopedLock lock (projectLock);
        const auto renderedChannelsDiffer = [&]
        {
            if (points.size() != rightPoints.size())
                return true;
            for (size_t i = 0; i < points.size(); ++i)
                if (std::abs (points[i].db - rightPoints[i].db) > 1.0e-4)
                    return true;
            return false;
        }();
        FlexCurveLayer renderedLayer;
        renderedLayer.id = nextLayerId++;
        renderedLayer.name = "Rendered FIR";
        renderedLayer.points = points;
        renderedLayer.colour = layerColourForIndex (0);
        renderedLayer.enabled = true;
        renderedLayer.muted = false;
        renderedLayer.solo = false;
        renderedLayer.visible = true;
        renderedLayer.gainDb = 0.0f;
        renderedLayer.opacity = 1.0f;
        renderedLayer.paramBands.resize (8);
        copyLeftChannelToRight (renderedLayer);
        renderedLayer.right.points = rightPoints;
        renderedLayer.channelsLinked = ! renderedChannelsDiffer;

        layers.clear();
        layers.push_back (std::move (renderedLayer));
        activeLayerId = layers.front().id;
        averageEnabled = true;
        averageVisible = false;
        globalBlend = {};
        blendPerLayerMode = true;
        finalCurve = points;
        finalCurveRight = rightPoints;
        renderedCurve = points;
        renderedCurveRight = rightPoints;
    }

    hasRenderedFir = true;
    firOutdated = false;
    editLocked = true;
    updateFixedAutoGainFromCurrentCurve();
}

bool FlexCurveAudioProcessor::exportCurrentFirToFile (const juce::File& file)
{
    if (! canExportRenderedFir())
        return false;

    std::vector<CurvePoint> points;
    std::vector<CurvePoint> rightPoints;
    {
        const juce::ScopedLock lock (projectLock);
        points = renderedCurve;
        rightPoints = renderedCurveRight.empty() ? renderedCurve : renderedCurveRight;
    }

    if (points.empty())
        return false;

    double exportedGainDb = 0.0;
    if (parameters.getRawParameterValue ("includeoutputgainfir")->load() > 0.5f)
        exportedGainDb += parameters.getRawParameterValue ("outputgain")->load();
    if (parameters.getRawParameterValue ("includeautogainfir")->load() > 0.5f)
        exportedGainDb += runtimeAutoGainDb.load();
    if (! juce::approximatelyEqual (exportedGainDb, 0.0))
    {
        for (auto& point : points)
            point.db += exportedGainDb;
        for (auto& point : rightPoints)
            point.db += exportedGainDb;
    }

    const auto taps = getDefaultFirTapsForMode (getPhaseMode());
    auto impulse = createStereoImpulseForPhaseMode (points, rightPoints, currentSampleRate, taps);

    juce::WavAudioFormat wavFormat;
    if (file.existsAsFile() && ! file.deleteFile())
        return false;

    std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
    if (stream == nullptr || ! stream->openedOk())
        return false;

    const auto metadata = makeFlexCurveFirMetadata (exportedGainDb);
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wavFormat.createWriterFor (stream.get(), currentSampleRate,
                                   static_cast<unsigned int> (impulse.getNumChannels()),
                                   32, metadata, 0));
    if (writer == nullptr)
        return false;

    stream.release();
    return writer->writeFromAudioSampleBuffer (impulse, 0, impulse.getNumSamples());
}

std::vector<FlexParamBand> FlexCurveAudioProcessor::approximateCurveWithParametricFilters (const std::vector<CurvePoint>& points) const
{
    std::vector<CurvePoint> residual = makeDefaultGrid();
    for (auto& point : residual)
        point.db = CurveFIR::interpolateDb (points, point.frequency);

    std::vector<FlexParamBand> result;
    result.reserve (24);

    for (int iteration = 0; iteration < 24; ++iteration)
    {
        int peakIndex = -1;
        double peakMagnitude = 0.0;
        for (int i = 0; i < static_cast<int> (residual.size()); ++i)
        {
            const auto frequency = residual[static_cast<size_t> (i)].frequency;
            if (frequency < 25.0 || frequency > 18000.0)
                continue;
            const auto magnitude = std::abs (residual[static_cast<size_t> (i)].db);
            if (magnitude > peakMagnitude)
            {
                peakMagnitude = magnitude;
                peakIndex = i;
            }
        }

        if (peakIndex < 0 || peakMagnitude < 0.35)
            break;

        const auto centreFrequency = residual[static_cast<size_t> (peakIndex)].frequency;
        const auto peakDb = residual[static_cast<size_t> (peakIndex)].db;
        const auto halfMagnitude = peakMagnitude * 0.5;
        int lowerIndex = peakIndex;
        int upperIndex = peakIndex;
        while (lowerIndex > 0
               && std::abs (residual[static_cast<size_t> (lowerIndex)].db) >= halfMagnitude
               && residual[static_cast<size_t> (lowerIndex)].db * peakDb > 0.0)
            --lowerIndex;
        while (upperIndex + 1 < static_cast<int> (residual.size())
               && std::abs (residual[static_cast<size_t> (upperIndex)].db) >= halfMagnitude
               && residual[static_cast<size_t> (upperIndex)].db * peakDb > 0.0)
            ++upperIndex;

        const auto lowerFrequency = residual[static_cast<size_t> (lowerIndex)].frequency;
        const auto upperFrequency = residual[static_cast<size_t> (upperIndex)].frequency;
        const auto bandwidth = juce::jmax (centreFrequency * 0.04, upperFrequency - lowerFrequency);

        FlexParamBand band;
        band.enabled = true;
        band.type = FlexParamBand::peak;
        band.frequency = static_cast<float> (centreFrequency);
        band.gainDb = static_cast<float> (juce::jlimit (-12.0, 12.0, peakDb));
        band.q = static_cast<float> (juce::jlimit (0.20, 12.0, centreFrequency / bandwidth));
        result.push_back (band);

        const auto filter = makeFilter (currentSampleRate, band);
        for (auto& point : residual)
            point.db -= getBiquadMagnitudeDb (filter, currentSampleRate, point.frequency);
    }

    return result;
}

bool FlexCurveAudioProcessor::normalizeReferencesAtFrequency (int rawLayerId, int targetLayerId,
                                                              double frequencyHz)
{
    if (editsBlocked())
        return false;

    frequencyHz = juce::jlimit (20.0, 20000.0, frequencyHz);
    recordUndoState ("normalize-references-" + juce::String (frequencyHz, 1));
    {
        const juce::ScopedLock lock (projectLock);
        auto* raw = findLayer (rawLayerId);
        auto* target = findLayer (targetLayerId);
        if (raw == nullptr || target == nullptr
            || raw->type != FlexCurveLayerType::raw
            || target->type != FlexCurveLayerType::target)
            return false;

        const auto channel = getActiveLayer() != nullptr
            ? getActiveLayer()->selectedChannel : raw->selectedChannel;
        const auto rawLeft = calculateLayerCurveLocked (*raw, FlexChannelSelection::left);
        const auto rawRight = calculateLayerCurveLocked (*raw, FlexChannelSelection::right);
        const auto targetLeft = calculateLayerCurveLocked (*target, FlexChannelSelection::left);
        const auto targetRight = calculateLayerCurveLocked (*target, FlexChannelSelection::right);
        const auto rawLeftOffset = -static_cast<float> (
            CurveFIR::interpolateDb (rawLeft, frequencyHz));
        const auto rawRightOffset = -static_cast<float> (
            CurveFIR::interpolateDb (rawRight, frequencyHz));
        const auto targetLeftOffset = -static_cast<float> (
            CurveFIR::interpolateDb (targetLeft, frequencyHz));
        const auto targetRightOffset = -static_cast<float> (
            CurveFIR::interpolateDb (targetRight, frequencyHz));

        auto applyOffsets = [] (FlexCurveLayer& layer, FlexChannelSelection selection,
                                float leftOffset, float rightOffset)
        {
            if (selection != FlexChannelSelection::right)
                layer.normalizationOffsetDb += leftOffset;
            if (selection != FlexChannelSelection::left)
            {
                if (layer.channelsLinked)
                {
                    FlexCurveAudioProcessor::copyLeftChannelToRight (layer);
                    if (selection == FlexChannelSelection::right)
                        layer.channelsLinked = false;
                }
                layer.right.normalizationOffsetDb += rightOffset;
            }
            if (selection == FlexChannelSelection::stereo && layer.channelsLinked)
                FlexCurveAudioProcessor::copyLeftChannelToRight (layer);
        };
        applyOffsets (*raw, channel, rawLeftOffset, rawRightOffset);
        applyOffsets (*target, channel, targetLeftOffset, targetRightOffset);
        markLinkedAutoEqLayersOutdatedLocked (raw->id);
        markLinkedAutoEqLayersOutdatedLocked (target->id);
        finalCurve = calculateFinalCurveLocked();
    }
    markPreviewDirty();
    return true;
}

void FlexCurveAudioProcessor::normalizeEqLayersToZeroDb()
{
    if (editsBlocked())
        return;

    recordUndoState ("normalize-eq-layers-zero-db");
    {
        const juce::ScopedLock lock (projectLock);
        for (auto& layer : layers)
        {
            if (layer.type != FlexCurveLayerType::eq)
                continue;

            const auto leftCurve = calculateLayerCurveLocked (
                layer, FlexChannelSelection::left);
            const auto rightCurve = calculateLayerCurveLocked (
                layer, FlexChannelSelection::right);
            if (! leftCurve.empty())
            {
                const auto peak = std::max_element (
                    leftCurve.begin(), leftCurve.end(),
                    [] (const auto& lhs, const auto& rhs) { return lhs.db < rhs.db; })->db;
                layer.gainDb += static_cast<float> (layer.inverted ? peak : -peak);
            }
            if (! layer.channelsLinked && ! rightCurve.empty())
            {
                const auto peak = std::max_element (
                    rightCurve.begin(), rightCurve.end(),
                    [] (const auto& lhs, const auto& rhs) { return lhs.db < rhs.db; })->db;
                layer.right.gainDb += static_cast<float> (
                    layer.right.inverted ? peak : -peak);
            }
            else if (layer.channelsLinked)
            {
                copyLeftChannelToRight (layer);
            }
        }
        finalCurve = calculateFinalCurveLocked();
    }
    markPreviewDirty();
}

bool FlexCurveAudioProcessor::generateAutoEq (int rawLayerId, int targetLayerId, AutoEqMode mode)
{
    if (editsBlocked() || ! canAddUserLayer())
        return false;

    struct GeneratedChannel
    {
        std::vector<CurvePoint> correction;
        std::vector<FlexParamBand> parametric;
        AutoEqMode mode = AutoEqMode::variable;
        float peakDb = 0.0f;
    };

    juce::String rawName;
    juce::String targetName;
    FlexChannelSelection requestedChannel = FlexChannelSelection::stereo;
    const FlexCurveLayer* raw = nullptr;
    const FlexCurveLayer* target = nullptr;
    {
        const juce::ScopedLock lock (projectLock);
        raw = findLayer (rawLayerId);
        target = findLayer (targetLayerId);
        if (raw == nullptr || target == nullptr
            || raw->type != FlexCurveLayerType::raw
            || target->type != FlexCurveLayerType::target)
            return false;

        rawName = raw->name;
        targetName = target->name;
        if (const auto* active = getActiveLayer())
            requestedChannel = active->selectedChannel;
        else
            requestedChannel = raw->selectedChannel;
    }

    auto generateChannel = [this, rawLayerId, targetLayerId, mode] (
        FlexChannelSelection channel) -> GeneratedChannel
    {
        GeneratedChannel generated;
        generated.correction = makeDefaultGrid();
        {
            const juce::ScopedLock lock (projectLock);
            const auto* channelRaw = findLayer (rawLayerId);
            const auto* channelTarget = findLayer (targetLayerId);
            if (channelRaw == nullptr || channelTarget == nullptr)
                return generated;
            const auto rawCurve = calculateLayerCurveLocked (*channelRaw, channel);
            const auto targetCurve = calculateLayerCurveLocked (*channelTarget, channel);
            for (auto& point : generated.correction)
                point.db = juce::jlimit (
                    -12.0, 12.0,
                    CurveFIR::interpolateDb (targetCurve, point.frequency)
                        - CurveFIR::interpolateDb (rawCurve, point.frequency));
        }

        smoothCurveGeometry (generated.correction);
        smoothCurveGeometry (generated.correction);
        generated.parametric = approximateCurveWithParametricFilters (generated.correction);
        generated.mode = mode;
        if (mode == AutoEqMode::automatic)
        {
            double squaredError = 0.0;
            for (const auto& point : generated.correction)
            {
                double approximation = 0.0;
                for (const auto& band : generated.parametric)
                    approximation += getBiquadMagnitudeDb (
                        makeFilter (currentSampleRate, band), currentSampleRate, point.frequency);
                const auto error = approximation - point.db;
                squaredError += error * error;
            }
            const auto rmsError = std::sqrt (
                squaredError / juce::jmax (size_t (1), generated.correction.size()));
            generated.mode = rmsError <= 1.0 && generated.parametric.size() <= 16
                ? AutoEqMode::parametric : AutoEqMode::variable;
        }

        auto response = makeDefaultGrid();
        if (generated.mode == AutoEqMode::parametric)
        {
            for (auto& point : response)
                for (const auto& band : generated.parametric)
                    if (band.enabled)
                        point.db += getBiquadMagnitudeDb (
                            makeFilter (currentSampleRate, band), currentSampleRate, point.frequency);
        }
        else
        {
            response = generated.correction;
            smoothCurveGeometry (response);
        }
        generated.peakDb = static_cast<float> (std::max_element (
            response.begin(), response.end(),
            [] (const auto& lhs, const auto& rhs) { return lhs.db < rhs.db; })->db);
        return generated;
    };

    auto left = generateChannel (FlexChannelSelection::left);
    auto right = generateChannel (FlexChannelSelection::right);
    const auto independentPreamp =
        parameters.getRawParameterValue ("independentlrpreamp")->load() > 0.5f;
    const auto sharedPeak = juce::jmax (left.peakDb, right.peakDb, 0.0f);
    const auto leftPreamp = independentPreamp ? -juce::jmax (left.peakDb, 0.0f) : -sharedPeak;
    const auto rightPreamp = independentPreamp ? -juce::jmax (right.peakDb, 0.0f) : -sharedPeak;
    const auto correctionsEqual = [&]
    {
        if (left.correction.size() != right.correction.size())
            return false;
        for (size_t i = 0; i < left.correction.size(); ++i)
            if (std::abs (left.correction[i].db - right.correction[i].db) > 1.0e-5)
                return false;
        return true;
    }();

    recordUndoState ("generate-autoeq");
    {
        const juce::ScopedLock lock (projectLock);
        if (findLayer (targetLayerId) == nullptr || findLayer (rawLayerId) == nullptr)
            return false;

        FlexCurveLayer layer;
        layer.id = nextLayerId++;
        layer.type = FlexCurveLayerType::eq;
        layer.name = "AutoEQ " + rawName.upToFirstOccurrenceOf (".", false, false)
                   + " to " + targetName.upToFirstOccurrenceOf (".", false, false);
        layer.points = makeDefaultGrid();
        layer.colour = nextLayerColour (layers);
        layer.autoEqRawLayerId = rawLayerId;
        layer.autoEqTargetLayerId = targetLayerId;
        layer.autoEqSourcesOutdated = false;
        layer.autoEqReferenceOffsetDb = leftPreamp;
        layer.autoEqReferenceDisplayEnabled = true;
        layer.autoEqMethod = left.mode == AutoEqMode::parametric ? "Parametric" : "Variable";
        layer.gainDb = leftPreamp;
        layer.paramBands.resize (8);

        if (left.mode == AutoEqMode::parametric)
        {
            layer.paramBands = std::move (left.parametric);
            if (layer.paramBands.empty())
                layer.paramBands.resize (8);
        }
        else
        {
            layer.graphicEnabled = true;
            layer.graphicMode = 0;
            layer.freeformPoints = std::move (left.correction);
            layer.smoothGraphicCurve = true;
        }

        copyLeftChannelToRight (layer);
        layer.right.autoEqMethod = right.mode == AutoEqMode::parametric ? "Parametric" : "Variable";
        layer.right.autoEqReferenceOffsetDb = rightPreamp;
        layer.right.gainDb = rightPreamp;
        layer.right.paramBands.assign (8, {});
        layer.right.graphicEnabled = false;
        layer.right.freeformPoints.clear();
        if (right.mode == AutoEqMode::parametric)
        {
            layer.right.paramBands = std::move (right.parametric);
            if (layer.right.paramBands.empty())
                layer.right.paramBands.resize (8);
        }
        else
        {
            layer.right.graphicEnabled = true;
            layer.right.graphicMode = 0;
            layer.right.freeformPoints = std::move (right.correction);
            layer.right.smoothGraphicCurve = true;
        }
        layer.selectedChannel = requestedChannel;
        layer.channelsLinked = requestedChannel == FlexChannelSelection::stereo
            && ! independentPreamp
            && layer.autoEqMethod == layer.right.autoEqMethod
            && std::abs (leftPreamp - rightPreamp) < 1.0e-5f
            && correctionsEqual;
        if (requestedChannel == FlexChannelSelection::left)
        {
            layer.right = {};
            layer.right.points = makeDefaultGrid();
            layer.right.paramBands.resize (8);
            layer.channelsLinked = false;
        }
        else if (requestedChannel == FlexChannelSelection::right)
        {
            const auto rightCorrection = layer.right;
            layer.autoEqMethod = rightCorrection.autoEqMethod;
            layer.autoEqSourcesOutdated = rightCorrection.autoEqSourcesOutdated;
            layer.autoEqReferenceOffsetDb = rightCorrection.autoEqReferenceOffsetDb;
            layer.autoEqReferenceDisplayEnabled = rightCorrection.autoEqReferenceDisplayEnabled;
            layer.points = makeDefaultGrid();
            layer.inverted = false;
            layer.gainDb = 0.0f;
            layer.normalizationOffsetDb = 0.0f;
            layer.smoothSourceCurve = false;
            layer.blend = {};
            layer.graphicEnabled = false;
            layer.graphicMode = 31;
            layer.preserveVariableShapeAcrossModes = false;
            layer.smoothGraphicCurve = false;
            layer.graphic15Gains.fill (0.0f);
            layer.graphic31Gains.fill (0.0f);
            layer.paramBands.assign (8, {});
            layer.freeformPoints.clear();
            layer.right = rightCorrection;
            layer.channelsLinked = false;
        }

        layers.push_back (std::move (layer));
        activeLayerId = layers.back().id;
        finalCurve = calculateFinalCurveLocked();
    }
    markPreviewDirty();
    return true;
}

bool FlexCurveAudioProcessor::exportLayerToFile (int layerId, bool average, CurveExportFormat format, const juce::File& file)
{
    std::vector<CurvePoint> points;
    std::vector<CurvePoint> rightPoints;
    double separateGainDb = 0.0;
    double separateRightGainDb = 0.0;
    const auto supportsSeparateGain = format == CurveExportFormat::firWav
                                   || format == CurveExportFormat::graphicEq
                                   || format == CurveExportFormat::apoParametric;
    {
        const juce::ScopedLock lock (projectLock);
        if (average)
        {
            points = calculateAverageCurveLocked (FlexChannelSelection::left);
            rightPoints = calculateAverageCurveLocked (FlexChannelSelection::right);
        }
        else if (const auto* layer = findLayer (layerId))
        {
            if (supportsSeparateGain)
            {
                auto baseLayer = *layer;
                baseLayer.gainDb = 0.0f;
                points = calculateLayerCurveLocked (baseLayer, FlexChannelSelection::left);
                separateGainDb = layer->inverted ? -layer->gainDb : layer->gainDb;
                if (layer->channelsLinked)
                {
                    rightPoints = points;
                    separateRightGainDb = separateGainDb;
                }
                else
                {
                    auto rightBase = *layer;
                    loadRightChannelIntoLayerView (*layer, rightBase);
                    rightBase.gainDb = 0.0f;
                    points = calculateLayerCurveLocked (baseLayer, FlexChannelSelection::left);
                    rightPoints = calculateLayerCurveLocked (rightBase, FlexChannelSelection::left);
                    separateRightGainDb = layer->right.inverted
                        ? -layer->right.gainDb : layer->right.gainDb;
                }
            }
            else
            {
                points = calculateLayerCurveLocked (*layer, FlexChannelSelection::left);
                rightPoints = calculateLayerCurveLocked (*layer, FlexChannelSelection::right);
            }
        }
    }

    if (points.empty())
        return false;
    if (rightPoints.empty())
        rightPoints = points;
    const auto channelsDiffer = [&]
    {
        if (points.size() != rightPoints.size()
            || std::abs (separateGainDb - separateRightGainDb) > 1.0e-5)
            return true;
        for (size_t i = 0; i < points.size(); ++i)
            if (std::abs (points[i].db - rightPoints[i].db) > 1.0e-5)
                return true;
        return false;
    }();

    if (format == CurveExportFormat::firWav)
    {
        if (parameters.getRawParameterValue ("includeoutputgainfir")->load() > 0.5f)
        {
            separateGainDb += parameters.getRawParameterValue ("outputgain")->load();
            separateRightGainDb += parameters.getRawParameterValue ("outputgain")->load();
        }
        if (parameters.getRawParameterValue ("includeautogainfir")->load() > 0.5f)
        {
            separateGainDb += runtimeAutoGainDb.load();
            separateRightGainDb += runtimeAutoGainDb.load();
        }
        auto firPoints = points;
        auto rightFirPoints = rightPoints;
        if (! juce::approximatelyEqual (separateGainDb, 0.0))
            for (auto& point : firPoints)
                point.db += separateGainDb;
        if (! juce::approximatelyEqual (separateRightGainDb, 0.0))
            for (auto& point : rightFirPoints)
                point.db += separateRightGainDb;
        const auto phaseMode = getPhaseMode();
        const auto taps = getDefaultFirTapsForMode (phaseMode);
        auto impulse = createStereoImpulseForPhaseMode (firPoints, rightFirPoints,
                                                        currentSampleRate, taps);
        juce::WavAudioFormat wav;
        if (file.existsAsFile() && ! file.deleteFile())
            return false;
        auto stream = file.createOutputStream();
        if (stream == nullptr || ! stream->openedOk())
            return false;
        const auto metadata = makeFlexCurveFirMetadata (separateGainDb);
        std::unique_ptr<juce::AudioFormatWriter> writer (
            wav.createWriterFor (stream.get(), currentSampleRate,
                                 static_cast<unsigned int> (impulse.getNumChannels()),
                                 32, metadata, 0));
        if (writer == nullptr)
            return false;
        stream.release();
        return writer->writeFromAudioSampleBuffer (impulse, 0, impulse.getNumSamples());
    }

    auto makeText = [this, format, average] (const std::vector<CurvePoint>& curve,
                                             double channelGainDb)
    {
        juce::String text;
        if (format == CurveExportFormat::graphicEq)
        {
            if (! average)
                text << "Preamp: " << juce::String (channelGainDb, 5) << " dB" << newLine;
            text << "GraphicEQ: ";
            for (size_t i = 0; i < curve.size(); ++i)
            {
                if (i != 0)
                    text << "; ";
                text << juce::String (curve[i].frequency, 3) << " " << juce::String (curve[i].db, 4);
            }
            text << newLine;
        }
        else if (format == CurveExportFormat::apoParametric)
        {
            if (! average)
                text << "Preamp: " << juce::String (channelGainDb, 5) << " dB" << newLine;
            const auto parametric = approximateCurveWithParametricFilters (curve);
            for (const auto& band : parametric)
            {
                if (! band.enabled)
                    continue;
                const auto type = band.type == FlexParamBand::peak ? "PK"
                                : band.type == FlexParamBand::lowShelf ? "LS"
                                : band.type == FlexParamBand::highShelf ? "HS"
                                : band.type == FlexParamBand::lowPass ? "LP"
                                : band.type == FlexParamBand::highPass ? "HP" : "NO";
                text << "Filter: ON " << type << " Fc " << juce::String (band.frequency, 3) << " Hz";
                if (band.type == FlexParamBand::peak || band.type == FlexParamBand::lowShelf || band.type == FlexParamBand::highShelf)
                    text << " Gain " << juce::String (band.gainDb, 3) << " dB";
                text << " Q " << juce::String (band.q, 4) << newLine;
            }
        }
        else
        {
            if (format == CurveExportFormat::frequencyCsv)
                text = "frequency,db\n";
            else if (format == CurveExportFormat::meldaCsv)
                text = "Frequency (Hz),Gain (dB)\n";
            const auto separator = format == CurveExportFormat::frequencyText ? "\t" : ",";
            for (const auto& point : curve)
                text << juce::String (point.frequency, 5) << separator
                     << juce::String (point.db, 5) << newLine;
        }
        return text;
    };

    if (channelsDiffer)
    {
        const auto leftFile = file.getSiblingFile (
            file.getFileNameWithoutExtension() + "_L" + file.getFileExtension());
        const auto rightFile = file.getSiblingFile (
            file.getFileNameWithoutExtension() + "_R" + file.getFileExtension());
        return leftFile.replaceWithText (makeText (points, separateGainDb))
            && rightFile.replaceWithText (makeText (rightPoints, separateRightGainDb));
    }

    if (file.existsAsFile() && ! file.deleteFile())
        return false;
    return file.replaceWithText (makeText (points, separateGainDb));
}

juce::String FlexCurveAudioProcessor::getStatusText() const
{
    if (! hasRenderedFir)
        return "Preview active";
    if (firOutdated)
        return "FIR outdated - preview active";
    return "FIR rendered";
}

FlexCurveAudioProcessor::PhaseMode FlexCurveAudioProcessor::getPhaseMode() const
{
    const auto index = juce::roundToInt (parameters.getRawParameterValue ("phasemode")->load());
    if (index <= 0)
        return PhaseMode::minimum;
    if (index >= 2)
        return PhaseMode::linear;
    return PhaseMode::natural;
}

int FlexCurveAudioProcessor::scaleReferenceSampleCount (int referenceSamples) const
{
    const auto safeSampleRate = currentSampleRate > 0.0 ? currentSampleRate : referenceSampleRate;
    auto scaled = static_cast<int> (std::round (static_cast<double> (referenceSamples) * safeSampleRate / referenceSampleRate));
    scaled = juce::jlimit (256, maxFirTaps, scaled);
    if ((scaled & 1) != 0)
        ++scaled;
    return juce::jmin (scaled, maxFirTaps);
}

int FlexCurveAudioProcessor::getDefaultFirTapsForMode (PhaseMode mode) const
{
    switch (mode)
    {
        case PhaseMode::minimum: return scaleReferenceSampleCount (minimumPhaseReferenceTaps);
        case PhaseMode::natural: return scaleReferenceSampleCount (naturalPhaseReferenceTaps);
        case PhaseMode::linear:  return scaleReferenceSampleCount (linearPhaseReferenceTaps);
    }
    return scaleReferenceSampleCount (naturalPhaseReferenceTaps);
}

int FlexCurveAudioProcessor::getLatencyForPhaseMode (PhaseMode mode, int taps) const
{
    switch (mode)
    {
        case PhaseMode::minimum: return 0;
        case PhaseMode::natural: return juce::jlimit (0, juce::jmax (0, taps - 1), scaleReferenceSampleCount (naturalReferenceLatencySamples));
        case PhaseMode::linear:  return juce::jmax (0, taps / 2);
    }
    return 0;
}

juce::AudioBuffer<float> FlexCurveAudioProcessor::createImpulseForPhaseMode (const std::vector<CurvePoint>& points, double sampleRate, int taps) const
{
    switch (getPhaseMode())
    {
        case PhaseMode::minimum: return CurveFIR::createMinimumPhaseFIR (points, sampleRate, taps);
        case PhaseMode::natural: return CurveFIR::createMixedPhaseFIR (points, sampleRate, taps, 0.72f, juce::jlimit (0, taps - 1, scaleReferenceSampleCount (naturalReferenceLatencySamples)));
        case PhaseMode::linear:  return CurveFIR::createLinearPhaseFIR (points, sampleRate, taps);
    }
    return CurveFIR::createMixedPhaseFIR (points, sampleRate, taps);
}

juce::AudioBuffer<float> FlexCurveAudioProcessor::createStereoImpulseForPhaseMode (
    const std::vector<CurvePoint>& leftPoints,
    const std::vector<CurvePoint>& rightPoints,
    double sampleRate,
    int taps) const
{
    auto left = createImpulseForPhaseMode (leftPoints, sampleRate, taps);
    if (getTotalNumOutputChannels() < 2)
        return left;
    bool equal = leftPoints.size() == rightPoints.size();
    if (equal)
        for (size_t i = 0; i < leftPoints.size(); ++i)
            if (std::abs (leftPoints[i].db - rightPoints[i].db) > 1.0e-5
                || std::abs (leftPoints[i].frequency - rightPoints[i].frequency) > 1.0e-5)
            {
                equal = false;
                break;
            }
    if (equal)
        return left;

    auto right = createImpulseForPhaseMode (rightPoints, sampleRate, taps);
    juce::AudioBuffer<float> stereo (2, juce::jmax (left.getNumSamples(), right.getNumSamples()));
    stereo.clear();
    stereo.copyFrom (0, 0, left, 0, 0, left.getNumSamples());
    stereo.copyFrom (1, 0, right, 0, 0, right.getNumSamples());
    return stereo;
}

int FlexCurveAudioProcessor::findImpulseResponsePeak (const juce::AudioBuffer<float>& impulse) const
{
    int peakIndex = 0;
    float peak = -1.0f;
    if (impulse.getNumSamples() <= 0)
        return 0;
    const auto* samples = impulse.getReadPointer (0);
    for (int i = 0; i < impulse.getNumSamples(); ++i)
    {
        const auto value = std::abs (samples[i]);
        if (value > peak)
        {
            peak = value;
            peakIndex = i;
        }
    }
    return peakIndex;
}

void FlexCurveAudioProcessor::applyActiveLatency (int latencySamples)
{
    activeLatencySamples = juce::jmax (0, latencySamples);
    setLatencySamples (activeLatencySamples);
    updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withLatencyChanged (true).withNonParameterStateChanged (true));
    dryDelayBuffer.clear();
    dryDelayWriteIndex = 0;
}

void FlexCurveAudioProcessor::updateMeters (const juce::AudioBuffer<float>& buffer, bool input)
{
    const auto stats = measureBufferPower (buffer);
    const auto peakDb = juce::Decibels::gainToDecibels (stats.peak, -100.0f);
    const auto rmsDb = juce::Decibels::gainToDecibels (
        static_cast<float> (std::sqrt (juce::jmax (0.0, stats.meanSquare))), -100.0f);

    auto updateBallistic = [] (std::atomic<float>& target, float next)
    {
        const auto previous = target.load();
        target.store (next >= previous ? next : previous * 0.88f + next * 0.12f);
    };
    auto updateStage = [&stats, &updateBallistic] (auto& peakTargets, auto& rmsTargets,
                                                   auto& momentaryTargets, auto& shortTargets,
                                                   auto& integratedTargets)
    {
        for (size_t channel = 0; channel < 3; ++channel)
        {
            const auto channelPeakDb = juce::Decibels::gainToDecibels (stats.channelPeak[channel], -100.0f);
            const auto channelRmsDb = juce::Decibels::gainToDecibels (
                static_cast<float> (std::sqrt (juce::jmax (0.0, stats.channelMeanSquare[channel]))), -100.0f);
            const auto channelLufs = approximateLufsFromRmsDb (channelRmsDb);
            updateBallistic (peakTargets[channel], channelPeakDb);
            updateBallistic (rmsTargets[channel], channelRmsDb);
            updateBallistic (momentaryTargets[channel], channelLufs);
            shortTargets[channel].store (shortTargets[channel].load() * 0.94f + channelLufs * 0.06f);
            integratedTargets[channel].store (integratedTargets[channel].load() * 0.985f + channelLufs * 0.015f);
        }
    };

    if (input)
    {
        updateBallistic (inputPeakDb, peakDb);
        updateBallistic (inputRmsDb, rmsDb);
        updateStage (inputChannelPeakDb, inputChannelRmsDb, inputChannelLufsMomentary,
                     inputChannelLufsShortTerm, inputChannelLufsIntegrated);
        inputClip.store (stats.peak > 1.0f);
        inputClipOverDb.store (stats.peak > 1.0f ? juce::Decibels::gainToDecibels (stats.peak, 0.0f) : 0.0f);
    }
    else
    {
        updateBallistic (outputPeakDb, peakDb);
        updateBallistic (outputRmsDb, rmsDb);
        updateStage (outputChannelPeakDb, outputChannelRmsDb, outputChannelLufsMomentary,
                     outputChannelLufsShortTerm, outputChannelLufsIntegrated);
        outputClip.store (stats.peak > 1.0f);
        outputClipOverDb.store (stats.peak > 1.0f ? juce::Decibels::gainToDecibels (stats.peak, 0.0f) : 0.0f);
    }
}

void FlexCurveAudioProcessor::updateFixedAutoGainFromCurrentCurve() noexcept
{
    float fixedDb = 0.0f;
    try
    {
        std::vector<CurvePoint> points;
        {
            const juce::ScopedLock lock (projectLock);
            if (ashPreviewActive.load() && ! ashPreviewCurve.empty())
                points = ashPreviewCurve;
            else if (hasRenderedFir.load() && ! firOutdated.load() && ! renderedCurve.empty())
                points = renderedCurve;
            else
                points = finalCurve;
        }

        if (! points.empty())
        {
            fixedDb = static_cast<float> (juce::jlimit (
                -18.0, 18.0, CurveFIR::calculateKWeightedGainOffset (points, currentSampleRate)));
            if (juce::roundToInt (parameters.getRawParameterValue ("loudnessmatchmode")->load()) == 1)
                fixedDb = -fixedDb;
        }
    }
    catch (...)
    {
        fixedDb = 0.0f;
    }

    runtimeAutoGainDb.store (fixedDb);
}

FlexCurveAudioProcessor::MeterSnapshot FlexCurveAudioProcessor::getMeterSnapshot() const noexcept
{
    MeterSnapshot result;
    result.inputPeakDb = inputPeakDb.load();
    result.inputRmsDb = inputRmsDb.load();
    result.preAutoPeakDb = preAutoPeakDb.load();
    result.preAutoRmsDb = preAutoRmsDb.load();
    result.outputPeakDb = outputPeakDb.load();
    result.outputRmsDb = outputRmsDb.load();
    result.deltaPeakDb = result.outputPeakDb - result.inputPeakDb;
    result.deltaRmsDb = result.outputRmsDb - result.inputRmsDb;
    result.inputClipped = inputClip.load();
    result.preAutoClipped = preAutoClip.load();
    result.outputClipped = outputClip.load();
    result.autoGainDb = runtimeAutoGainDb.load();
    result.inputClipOverDb = inputClipOverDb.load();
    result.preAutoClipOverDb = preAutoClipOverDb.load();
    result.outputClipOverDb = outputClipOverDb.load();
    for (size_t channel = 0; channel < 3; ++channel)
    {
        result.inputChannels[channel] = { inputChannelPeakDb[channel].load(),
                                          inputChannelRmsDb[channel].load(),
                                          inputChannelLufsMomentary[channel].load(),
                                          inputChannelLufsShortTerm[channel].load(),
                                          inputChannelLufsIntegrated[channel].load() };
        result.preAutoChannels[channel] = { preAutoChannelPeakDb[channel].load(),
                                            preAutoChannelRmsDb[channel].load(),
                                            preAutoChannelLufsMomentary[channel].load(),
                                            preAutoChannelLufsShortTerm[channel].load(),
                                            preAutoChannelLufsIntegrated[channel].load() };
        result.outputChannels[channel] = { outputChannelPeakDb[channel].load(),
                                           outputChannelRmsDb[channel].load(),
                                           outputChannelLufsMomentary[channel].load(),
                                           outputChannelLufsShortTerm[channel].load(),
                                           outputChannelLufsIntegrated[channel].load() };
    }
    return result;
}

void FlexCurveAudioProcessor::resetMeters() noexcept
{
    inputPeakDb.store (-100.0f);
    inputRmsDb.store (-100.0f);
    preAutoPeakDb.store (-100.0f);
    preAutoRmsDb.store (-100.0f);
    outputPeakDb.store (-100.0f);
    outputRmsDb.store (-100.0f);
    for (size_t channel = 0; channel < 3; ++channel)
    {
        for (auto* value : { &inputChannelPeakDb[channel], &inputChannelRmsDb[channel],
                             &inputChannelLufsMomentary[channel], &inputChannelLufsShortTerm[channel],
                             &inputChannelLufsIntegrated[channel], &preAutoChannelPeakDb[channel],
                             &preAutoChannelRmsDb[channel], &preAutoChannelLufsMomentary[channel],
                             &preAutoChannelLufsShortTerm[channel], &preAutoChannelLufsIntegrated[channel],
                             &outputChannelPeakDb[channel], &outputChannelRmsDb[channel],
                             &outputChannelLufsMomentary[channel], &outputChannelLufsShortTerm[channel],
                             &outputChannelLufsIntegrated[channel] })
            value->store (-100.0f);
    }
    inputClip.store (false);
    preAutoClip.store (false);
    outputClip.store (false);
    inputClipOverDb.store (0.0f);
    preAutoClipOverDb.store (0.0f);
    outputClipOverDb.store (0.0f);
}

void FlexCurveAudioProcessor::resetAutoGainState() noexcept
{
    updateFixedAutoGainFromCurrentCurve();
}

void FlexCurveAudioProcessor::parameterChanged (const juce::String& parameterID, float)
{
    if (restoringState.load())
        return;

    if (parameterID == "phasemode" && hasRenderedFir && ! firOutdated)
        triggerAsyncUpdate();
    else if (parameterID == "loudnessmatchmode")
        updateFixedAutoGainFromCurrentCurve();
}

void FlexCurveAudioProcessor::handleAsyncUpdate()
{
    if (ashPreviewActive.load() && previewFiltersDirty.load())
    {
        rebuildPreviewFilters();
        return;
    }

    if (hasRenderedFir && ! firOutdated)
    {
        rebuildConvolutionFromRenderedCurve();
        return;
    }

    if (previewFiltersDirty.load())
        rebuildPreviewFilters();
}

std::array<double, 31> FlexCurveAudioProcessor::graphic31Frequencies()
{
    return { 20, 25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630,
             800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000, 20000 };
}

std::array<double, 15> FlexCurveAudioProcessor::graphic15Frequencies()
{
    return { 25, 40, 63, 100, 160, 250, 400, 630, 1000, 1600, 2500, 4000, 6300, 10000, 16000 };
}

FlexCurveAudioProcessor::Biquad FlexCurveAudioProcessor::makePeak (double sampleRate, double frequency, double gainDb, double q)
{
    Biquad out;
    const auto a = std::pow (10.0, gainDb / 40.0);
    const auto w0 = juce::MathConstants<double>::twoPi * juce::jlimit (10.0, sampleRate * 0.48, frequency) / sampleRate;
    const auto alpha = std::sin (w0) / (2.0 * juce::jmax (0.05, q));
    const auto c = std::cos (w0);
    const auto a0 = 1.0 + alpha / a;
    out.b0 = (1.0 + alpha * a) / a0;
    out.b1 = (-2.0 * c) / a0;
    out.b2 = (1.0 - alpha * a) / a0;
    out.a1 = (-2.0 * c) / a0;
    out.a2 = (1.0 - alpha / a) / a0;
    return out;
}

FlexCurveAudioProcessor::Biquad FlexCurveAudioProcessor::makeShelf (double sampleRate, double frequency, double gainDb, double q, bool highShelf)
{
    Biquad out;
    const auto a = std::pow (10.0, gainDb / 40.0);
    const auto w0 = juce::MathConstants<double>::twoPi * juce::jlimit (10.0, sampleRate * 0.48, frequency) / sampleRate;
    const auto alpha = std::sin (w0) / (2.0 * juce::jmax (0.05, q));
    const auto c = std::cos (w0);
    const auto sqrtA = std::sqrt (a);
    const auto beta = 2.0 * sqrtA * alpha;
    double b0, b1, b2, a0, a1, a2;

    if (highShelf)
    {
        b0 = a * ((a + 1.0) + (a - 1.0) * c + beta);
        b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * c);
        b2 = a * ((a + 1.0) + (a - 1.0) * c - beta);
        a0 = (a + 1.0) - (a - 1.0) * c + beta;
        a1 = 2.0 * ((a - 1.0) - (a + 1.0) * c);
        a2 = (a + 1.0) - (a - 1.0) * c - beta;
    }
    else
    {
        b0 = a * ((a + 1.0) - (a - 1.0) * c + beta);
        b1 = 2.0 * a * ((a - 1.0) - (a + 1.0) * c);
        b2 = a * ((a + 1.0) - (a - 1.0) * c - beta);
        a0 = (a + 1.0) + (a - 1.0) * c + beta;
        a1 = -2.0 * ((a - 1.0) + (a + 1.0) * c);
        a2 = (a + 1.0) + (a - 1.0) * c - beta;
    }

    out.b0 = b0 / a0; out.b1 = b1 / a0; out.b2 = b2 / a0; out.a1 = a1 / a0; out.a2 = a2 / a0;
    return out;
}

FlexCurveAudioProcessor::Biquad FlexCurveAudioProcessor::makeFilter (double sampleRate, const FlexParamBand& band)
{
    if (! band.enabled)
        return {};

    if (band.type == FlexParamBand::lowShelf)
        return makeShelf (sampleRate, band.frequency, band.gainDb, band.q, false);
    if (band.type == FlexParamBand::highShelf)
        return makeShelf (sampleRate, band.frequency, band.gainDb, band.q, true);
    if (band.type == FlexParamBand::notch
        || band.type == FlexParamBand::lowPass
        || band.type == FlexParamBand::highPass)
    {
        Biquad out;
        const auto frequency = juce::jlimit (10.0, sampleRate * 0.48, static_cast<double> (band.frequency));
        const auto q = juce::jmax (0.05, static_cast<double> (band.q));
        const auto w0 = juce::MathConstants<double>::twoPi * frequency / sampleRate;
        const auto c = std::cos (w0);
        const auto alpha = std::sin (w0) / (2.0 * q);
        const auto a0 = 1.0 + alpha;

        if (band.type == FlexParamBand::notch)
        {
            out.b0 = 1.0 / a0;
            out.b1 = -2.0 * c / a0;
            out.b2 = 1.0 / a0;
        }
        else if (band.type == FlexParamBand::lowPass)
        {
            out.b0 = 0.5 * (1.0 - c) / a0;
            out.b1 = (1.0 - c) / a0;
            out.b2 = out.b0;
        }
        else
        {
            out.b0 = 0.5 * (1.0 + c) / a0;
            out.b1 = -(1.0 + c) / a0;
            out.b2 = out.b0;
        }

        out.a1 = -2.0 * c / a0;
        out.a2 = (1.0 - alpha) / a0;
        return out;
    }

    return makePeak (sampleRate, band.frequency, band.gainDb, band.q);
}

double FlexCurveAudioProcessor::getBiquadMagnitudeDb (const Biquad& filter, double sampleRate, double frequency)
{
    const auto omega = juce::MathConstants<double>::twoPi * juce::jlimit (10.0, sampleRate * 0.48, frequency) / sampleRate;
    const std::complex<double> z1 = std::exp (std::complex<double> (0.0, -omega));
    const auto z2 = z1 * z1;
    const auto numerator = filter.b0 + filter.b1 * z1 + filter.b2 * z2;
    const auto denominator = 1.0 + filter.a1 * z1 + filter.a2 * z2;
    const auto magnitude = std::abs (numerator / denominator);
    return juce::Decibels::gainToDecibels (juce::jmax (1.0e-9, magnitude));
}

void FlexCurveAudioProcessor::rebuildPreviewFilters()
{
    previewFiltersDirty.store (false);
    std::vector<CurvePoint> points;
    std::vector<CurvePoint> rightPoints;
    {
        const juce::ScopedLock lock (projectLock);
        points = calculatePreviewCurveLocked (FlexChannelSelection::left);
        rightPoints = calculatePreviewCurveLocked (FlexChannelSelection::right);
    }
    if (points.empty() || currentSampleRate <= 0.0)
    {
        previewFirReady.store (false);
        return;
    }

    // GraphicEQ-style preview: curve edits are converted to a causal
    // minimum-phase FIR outside processBlock, then swapped into JUCE's
    // partitioned convolution engine.
    const auto taps = scaleReferenceSampleCount (minimumPhaseReferenceTaps);
    const auto previousMode = parameters.getRawParameterValue ("phasemode")->load();
    juce::ignoreUnused (previousMode);
    auto leftImpulse = CurveFIR::createMinimumPhaseFIR (points, currentSampleRate, taps);
    bool equal = points.size() == rightPoints.size();
    if (getTotalNumOutputChannels() < 2)
        equal = true;
    if (equal)
        for (size_t i = 0; i < points.size(); ++i)
            if (std::abs (points[i].db - rightPoints[i].db) > 1.0e-5)
            {
                equal = false;
                break;
            }
    juce::AudioBuffer<float> impulse;
    if (equal)
        impulse = std::move (leftImpulse);
    else
    {
        auto rightImpulse = CurveFIR::createMinimumPhaseFIR (rightPoints, currentSampleRate, taps);
        impulse.setSize (2, juce::jmax (leftImpulse.getNumSamples(), rightImpulse.getNumSamples()));
        impulse.clear();
        impulse.copyFrom (0, 0, leftImpulse, 0, 0, leftImpulse.getNumSamples());
        impulse.copyFrom (1, 0, rightImpulse, 0, 0, rightImpulse.getNumSamples());
    }
    previewConvolution.loadImpulseResponse (std::move (impulse),
                                            currentSampleRate,
                                            equal ? juce::dsp::Convolution::Stereo::no
                                                  : juce::dsp::Convolution::Stereo::yes,
                                            juce::dsp::Convolution::Trim::no,
                                            juce::dsp::Convolution::Normalise::no);
    previewFirReady.store (true);
}

void FlexCurveAudioProcessor::applyPreviewFilters (juce::AudioBuffer<float>& buffer)
{
    if (! previewFirReady.load())
        return;
    juce::dsp::AudioBlock<float> block (buffer);
    juce::dsp::ProcessContextReplacing<float> context (block);
    previewConvolution.process (context);
}

void FlexCurveAudioProcessor::resetCrossfeed()
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

void FlexCurveAudioProcessor::applyCrossfeed (juce::AudioBuffer<float>& buffer, float amount)
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
        const auto gLo = dbToGain (gbLo);
        const auto gHi = 1.0 - dbToGain (gbHi);
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

void FlexCurveAudioProcessor::applyGlobalBalance (juce::AudioBuffer<float>& buffer, float balanceDb) const
{
    if (buffer.getNumChannels() < 2 || std::abs (balanceDb) < 0.001f)
        return;

    const auto leftGain = juce::Decibels::decibelsToGain (channelBalanceOffsetDb (balanceDb, FlexChannelSelection::left));
    const auto rightGain = juce::Decibels::decibelsToGain (channelBalanceOffsetDb (balanceDb, FlexChannelSelection::right));
    buffer.applyGain (0, 0, buffer.getNumSamples(), leftGain);
    buffer.applyGain (1, 0, buffer.getNumSamples(), rightGain);
}

void FlexCurveAudioProcessor::addLayersToState (juce::ValueTree& state) const
{
    juce::ValueTree layersTree ("Layers");
    const juce::ScopedLock lock (projectLock);

    for (const auto& layer : layers)
    {
        juce::ValueTree node ("Layer");
        node.setProperty ("id", layer.id, nullptr);
        node.setProperty ("type", static_cast<int> (layer.type), nullptr);
        node.setProperty ("name", layer.name, nullptr);
        node.setProperty ("path", layer.sourceFile.getFullPathName(), nullptr);
        node.setProperty ("autoEqMethod", layer.autoEqMethod, nullptr);
        node.setProperty ("autoEqRawLayerId", layer.autoEqRawLayerId, nullptr);
        node.setProperty ("autoEqTargetLayerId", layer.autoEqTargetLayerId, nullptr);
        node.setProperty ("autoEqSourcesOutdated", layer.autoEqSourcesOutdated, nullptr);
        node.setProperty ("autoEqReferenceOffsetDb", layer.autoEqReferenceOffsetDb, nullptr);
        node.setProperty ("autoEqReferenceDisplayEnabled", layer.autoEqReferenceDisplayEnabled, nullptr);
        node.setProperty ("selectedChannel", static_cast<int> (layer.selectedChannel), nullptr);
        node.setProperty ("channelsLinked", layer.channelsLinked, nullptr);
        node.setProperty ("colour", static_cast<int> (layer.colour.getARGB()), nullptr);
        node.setProperty ("enabled", layer.enabled, nullptr);
        node.setProperty ("muted", layer.muted, nullptr);
        node.setProperty ("solo", layer.solo, nullptr);
        node.setProperty ("visible", layer.visible, nullptr);
        node.setProperty ("inverted", layer.inverted, nullptr);
        node.setProperty ("gainDb", layer.gainDb, nullptr);
        node.setProperty ("balanceDb", layer.balanceDb, nullptr);
        node.setProperty ("normalizationOffsetDb", layer.normalizationOffsetDb, nullptr);
        node.setProperty ("opacity", layer.opacity, nullptr);
        node.setProperty ("smoothSourceCurve", layer.smoothSourceCurve, nullptr);
        node.setProperty ("blendBassGainDb", layer.blend.bassGainDb, nullptr);
        node.setProperty ("blendMidGainDb", layer.blend.midGainDb, nullptr);
        node.setProperty ("blendTrebleGainDb", layer.blend.trebleGainDb, nullptr);
        node.setProperty ("blendLowMidHz", layer.blend.lowMidCrossoverHz, nullptr);
        node.setProperty ("blendMidHighHz", layer.blend.midHighCrossoverHz, nullptr);
        node.setProperty ("graphicEnabled", layer.graphicEnabled, nullptr);
        node.setProperty ("graphicMode", layer.graphicMode, nullptr);
        node.setProperty ("preserveVariableShapeAcrossModes", layer.preserveVariableShapeAcrossModes, nullptr);
        node.setProperty ("smoothGraphicCurve", layer.smoothGraphicCurve, nullptr);
        for (int i = 0; i < 15; ++i)
            node.setProperty ("graphic15_" + juce::String (i), layer.graphic15Gains[static_cast<size_t> (i)], nullptr);
        for (int i = 0; i < 31; ++i)
            node.setProperty ("graphic31_" + juce::String (i), layer.graphic31Gains[static_cast<size_t> (i)], nullptr);

        juce::ValueTree pointsTree ("Points");
        for (const auto& point : layer.points)
        {
            juce::ValueTree p ("Point");
            p.setProperty ("f", point.frequency, nullptr);
            p.setProperty ("db", point.db, nullptr);
            pointsTree.addChild (p, -1, nullptr);
        }
        node.addChild (pointsTree, -1, nullptr);

        juce::ValueTree paramsTree ("Parametric");
        for (const auto& band : layer.paramBands)
        {
            juce::ValueTree bandNode ("Band");
            bandNode.setProperty ("enabled", band.enabled, nullptr);
            bandNode.setProperty ("type", static_cast<int> (band.type), nullptr);
            bandNode.setProperty ("frequency", band.frequency, nullptr);
            bandNode.setProperty ("gainDb", band.gainDb, nullptr);
            bandNode.setProperty ("q", band.q, nullptr);
            paramsTree.addChild (bandNode, -1, nullptr);
        }
        node.addChild (paramsTree, -1, nullptr);

        juce::ValueTree freeformTree ("Freeform");
        for (const auto& point : layer.freeformPoints)
        {
            juce::ValueTree p ("Point");
            p.setProperty ("f", point.frequency, nullptr);
            p.setProperty ("db", point.db, nullptr);
            freeformTree.addChild (p, -1, nullptr);
        }
        node.addChild (freeformTree, -1, nullptr);

        juce::ValueTree rightTree ("RightChannel");
        rightTree.setProperty ("autoEqMethod", layer.right.autoEqMethod, nullptr);
        rightTree.setProperty ("autoEqSourcesOutdated", layer.right.autoEqSourcesOutdated, nullptr);
        rightTree.setProperty ("autoEqReferenceOffsetDb", layer.right.autoEqReferenceOffsetDb, nullptr);
        rightTree.setProperty ("autoEqReferenceDisplayEnabled", layer.right.autoEqReferenceDisplayEnabled, nullptr);
        rightTree.setProperty ("inverted", layer.right.inverted, nullptr);
        rightTree.setProperty ("gainDb", layer.right.gainDb, nullptr);
        rightTree.setProperty ("normalizationOffsetDb", layer.right.normalizationOffsetDb, nullptr);
        rightTree.setProperty ("smoothSourceCurve", layer.right.smoothSourceCurve, nullptr);
        rightTree.setProperty ("blendBassGainDb", layer.right.blend.bassGainDb, nullptr);
        rightTree.setProperty ("blendMidGainDb", layer.right.blend.midGainDb, nullptr);
        rightTree.setProperty ("blendTrebleGainDb", layer.right.blend.trebleGainDb, nullptr);
        rightTree.setProperty ("blendLowMidHz", layer.right.blend.lowMidCrossoverHz, nullptr);
        rightTree.setProperty ("blendMidHighHz", layer.right.blend.midHighCrossoverHz, nullptr);
        rightTree.setProperty ("graphicEnabled", layer.right.graphicEnabled, nullptr);
        rightTree.setProperty ("graphicMode", layer.right.graphicMode, nullptr);
        rightTree.setProperty ("preserveVariableShapeAcrossModes",
                               layer.right.preserveVariableShapeAcrossModes, nullptr);
        rightTree.setProperty ("smoothGraphicCurve", layer.right.smoothGraphicCurve, nullptr);
        for (int g = 0; g < 15; ++g)
            rightTree.setProperty ("graphic15_" + juce::String (g),
                                   layer.right.graphic15Gains[static_cast<size_t> (g)], nullptr);
        for (int g = 0; g < 31; ++g)
            rightTree.setProperty ("graphic31_" + juce::String (g),
                                   layer.right.graphic31Gains[static_cast<size_t> (g)], nullptr);

        juce::ValueTree rightPoints ("Points");
        for (const auto& point : layer.right.points)
        {
            juce::ValueTree p ("Point");
            p.setProperty ("f", point.frequency, nullptr);
            p.setProperty ("db", point.db, nullptr);
            rightPoints.addChild (p, -1, nullptr);
        }
        rightTree.addChild (rightPoints, -1, nullptr);

        juce::ValueTree rightParams ("Parametric");
        for (const auto& band : layer.right.paramBands)
        {
            juce::ValueTree bandNode ("Band");
            bandNode.setProperty ("enabled", band.enabled, nullptr);
            bandNode.setProperty ("type", static_cast<int> (band.type), nullptr);
            bandNode.setProperty ("frequency", band.frequency, nullptr);
            bandNode.setProperty ("gainDb", band.gainDb, nullptr);
            bandNode.setProperty ("q", band.q, nullptr);
            rightParams.addChild (bandNode, -1, nullptr);
        }
        rightTree.addChild (rightParams, -1, nullptr);

        juce::ValueTree rightFreeform ("Freeform");
        for (const auto& point : layer.right.freeformPoints)
        {
            juce::ValueTree p ("Point");
            p.setProperty ("f", point.frequency, nullptr);
            p.setProperty ("db", point.db, nullptr);
            rightFreeform.addChild (p, -1, nullptr);
        }
        rightTree.addChild (rightFreeform, -1, nullptr);
        node.addChild (rightTree, -1, nullptr);

        layersTree.addChild (node, -1, nullptr);
    }

    state.addChild (layersTree, -1, nullptr);
}

void FlexCurveAudioProcessor::addRenderedCurveToState (juce::ValueTree& state) const
{
    juce::ValueTree renderedTree ("RenderedCurve");
    const juce::ScopedLock lock (projectLock);

    renderedTree.setProperty ("phaseMode", static_cast<int> (getPhaseMode()), nullptr);
    renderedTree.setProperty ("sampleRate", currentSampleRate, nullptr);

    for (const auto& point : renderedCurve)
    {
        juce::ValueTree node ("Point");
        node.setProperty ("f", point.frequency, nullptr);
        node.setProperty ("db", point.db, nullptr);
        renderedTree.addChild (node, -1, nullptr);
    }

    juce::ValueTree rightTree ("Right");
    for (const auto& point : renderedCurveRight)
    {
        juce::ValueTree node ("Point");
        node.setProperty ("f", point.frequency, nullptr);
        node.setProperty ("db", point.db, nullptr);
        rightTree.addChild (node, -1, nullptr);
    }
    renderedTree.addChild (rightTree, -1, nullptr);

    state.addChild (renderedTree, -1, nullptr);
}

void FlexCurveAudioProcessor::restoreLayersFromState (const juce::ValueTree& state)
{
    const auto layersTree = state.getChildWithName ("Layers");
    std::vector<FlexCurveLayer> restored;
    int maxId = 0;

    std::array<int, 3> typeCounts {};
    for (int i = 0; i < layersTree.getNumChildren(); ++i)
    {
        const auto node = layersTree.getChild (i);
        FlexCurveLayer layer;
        layer.id = static_cast<int> (node.getProperty ("id", i + 1));
        layer.type = static_cast<FlexCurveLayerType> (juce::jlimit (0, 2, static_cast<int> (node.getProperty ("type", 0))));
        auto& typeCount = typeCounts[static_cast<size_t> (layer.type)];
        if (typeCount >= maxUserLayers)
            continue;
        ++typeCount;
        layer.name = node.getProperty ("name", "Layer").toString();
        layer.sourceFile = juce::File (node.getProperty ("path").toString());
        layer.autoEqMethod = node.getProperty ("autoEqMethod").toString();
        layer.autoEqRawLayerId = static_cast<int> (node.getProperty ("autoEqRawLayerId", 0));
        layer.autoEqTargetLayerId = static_cast<int> (node.getProperty ("autoEqTargetLayerId", 0));
        layer.autoEqSourcesOutdated = static_cast<bool> (node.getProperty ("autoEqSourcesOutdated", false));
        layer.autoEqReferenceOffsetDb = static_cast<float> (node.getProperty ("autoEqReferenceOffsetDb", 0.0f));
        layer.autoEqReferenceDisplayEnabled
            = static_cast<bool> (node.getProperty ("autoEqReferenceDisplayEnabled", true));
        layer.selectedChannel = static_cast<FlexChannelSelection> (juce::jlimit (
            0, 2, static_cast<int> (node.getProperty ("selectedChannel", 0))));
        layer.channelsLinked = static_cast<bool> (node.getProperty ("channelsLinked", true));
        layer.colour = juce::Colour (static_cast<juce::uint32> (static_cast<int> (node.getProperty ("colour", static_cast<int> (layerColourForIndex (i).getARGB())))));
        layer.enabled = static_cast<bool> (node.getProperty ("enabled", true));
        layer.muted = static_cast<bool> (node.getProperty ("muted", ! layer.enabled));
        layer.solo = static_cast<bool> (node.getProperty ("solo", false));
        layer.visible = static_cast<bool> (node.getProperty ("visible", true));
        layer.inverted = static_cast<bool> (node.getProperty ("inverted", false));
        layer.gainDb = static_cast<float> (node.getProperty ("gainDb", 0.0f));
        layer.balanceDb = static_cast<float> (node.getProperty ("balanceDb", 0.0f));
        layer.normalizationOffsetDb = static_cast<float> (node.getProperty ("normalizationOffsetDb", 0.0f));
        layer.opacity = static_cast<float> (node.getProperty ("opacity", 1.0f));
        layer.smoothSourceCurve = static_cast<bool> (node.getProperty ("smoothSourceCurve", false));
        layer.blend.bassGainDb = static_cast<float> (node.getProperty ("blendBassGainDb", 0.0f));
        layer.blend.midGainDb = static_cast<float> (node.getProperty ("blendMidGainDb", 0.0f));
        layer.blend.trebleGainDb = static_cast<float> (node.getProperty ("blendTrebleGainDb", 0.0f));
        layer.blend.lowMidCrossoverHz = static_cast<float> (node.getProperty ("blendLowMidHz", 250.0f));
        layer.blend.midHighCrossoverHz = static_cast<float> (node.getProperty ("blendMidHighHz", 4000.0f));
        layer.graphicEnabled = static_cast<bool> (node.getProperty ("graphicEnabled", false));
        layer.graphicMode = static_cast<int> (node.getProperty ("graphicMode", 31));
        layer.preserveVariableShapeAcrossModes = static_cast<bool> (node.getProperty ("preserveVariableShapeAcrossModes", false));
        layer.smoothGraphicCurve = static_cast<bool> (node.getProperty ("smoothGraphicCurve", false));
        if (layer.graphicMode != 0 && layer.graphicMode != 15 && layer.graphicMode != 31)
            layer.graphicMode = 31;
        const auto hasIndependentGraphicBanks = node.hasProperty ("graphic15_0") || node.hasProperty ("graphic31_0");
        for (int g = 0; g < 15; ++g)
            layer.graphic15Gains[static_cast<size_t> (g)] = static_cast<float> (
                node.getProperty (hasIndependentGraphicBanks ? "graphic15_" + juce::String (g)
                                                             : "graphic" + juce::String (g),
                                  0.0f));
        for (int g = 0; g < 31; ++g)
            layer.graphic31Gains[static_cast<size_t> (g)] = static_cast<float> (
                node.getProperty (hasIndependentGraphicBanks ? "graphic31_" + juce::String (g)
                                                             : "graphic" + juce::String (g),
                                  0.0f));

        const auto pointsTree = node.getChildWithName ("Points");
        for (int p = 0; p < pointsTree.getNumChildren(); ++p)
        {
            const auto pointNode = pointsTree.getChild (p);
            layer.points.push_back ({ static_cast<double> (pointNode.getProperty ("f", 0.0)),
                                      static_cast<double> (pointNode.getProperty ("db", 0.0)) });
        }

        const auto paramsTree = node.getChildWithName ("Parametric");
        for (int b = 0; b < paramsTree.getNumChildren(); ++b)
        {
            const auto bandNode = paramsTree.getChild (b);
            FlexParamBand band;
            band.enabled = static_cast<bool> (bandNode.getProperty ("enabled", false));
            band.type = static_cast<FlexParamBand::Type> (static_cast<int> (bandNode.getProperty ("type", 0)));
            band.frequency = static_cast<float> (bandNode.getProperty ("frequency", 1000.0f));
            band.gainDb = static_cast<float> (bandNode.getProperty ("gainDb", 0.0f));
            band.q = static_cast<float> (bandNode.getProperty ("q", 1.0f));
            layer.paramBands.push_back (band);
        }
        if (layer.paramBands.empty())
            layer.paramBands.resize (8);

        const auto freeformTree = node.getChildWithName ("Freeform");
        for (int f = 0; f < freeformTree.getNumChildren(); ++f)
        {
            const auto pointNode = freeformTree.getChild (f);
            layer.freeformPoints.push_back ({ static_cast<double> (pointNode.getProperty ("f", 0.0)),
                                              static_cast<double> (pointNode.getProperty ("db", 0.0)) });
        }

        copyLeftChannelToRight (layer);
        const auto rightTree = node.getChildWithName ("RightChannel");
        if (rightTree.isValid())
        {
            layer.right.autoEqMethod = rightTree.getProperty ("autoEqMethod", layer.autoEqMethod).toString();
            layer.right.autoEqSourcesOutdated = static_cast<bool> (
                rightTree.getProperty ("autoEqSourcesOutdated", layer.autoEqSourcesOutdated));
            layer.right.autoEqReferenceOffsetDb = static_cast<float> (
                rightTree.getProperty ("autoEqReferenceOffsetDb", layer.autoEqReferenceOffsetDb));
            layer.right.autoEqReferenceDisplayEnabled = static_cast<bool> (
                rightTree.getProperty ("autoEqReferenceDisplayEnabled",
                                       layer.autoEqReferenceDisplayEnabled));
            layer.right.inverted = static_cast<bool> (
                rightTree.getProperty ("inverted", layer.inverted));
            layer.right.gainDb = static_cast<float> (
                rightTree.getProperty ("gainDb", layer.gainDb));
            layer.right.normalizationOffsetDb = static_cast<float> (
                rightTree.getProperty ("normalizationOffsetDb", layer.normalizationOffsetDb));
            layer.right.smoothSourceCurve = static_cast<bool> (
                rightTree.getProperty ("smoothSourceCurve", layer.smoothSourceCurve));
            layer.right.blend.bassGainDb = static_cast<float> (
                rightTree.getProperty ("blendBassGainDb", layer.blend.bassGainDb));
            layer.right.blend.midGainDb = static_cast<float> (
                rightTree.getProperty ("blendMidGainDb", layer.blend.midGainDb));
            layer.right.blend.trebleGainDb = static_cast<float> (
                rightTree.getProperty ("blendTrebleGainDb", layer.blend.trebleGainDb));
            layer.right.blend.lowMidCrossoverHz = static_cast<float> (
                rightTree.getProperty ("blendLowMidHz", layer.blend.lowMidCrossoverHz));
            layer.right.blend.midHighCrossoverHz = static_cast<float> (
                rightTree.getProperty ("blendMidHighHz", layer.blend.midHighCrossoverHz));
            layer.right.graphicEnabled = static_cast<bool> (
                rightTree.getProperty ("graphicEnabled", layer.graphicEnabled));
            layer.right.graphicMode = static_cast<int> (
                rightTree.getProperty ("graphicMode", layer.graphicMode));
            layer.right.preserveVariableShapeAcrossModes = static_cast<bool> (
                rightTree.getProperty ("preserveVariableShapeAcrossModes",
                                       layer.preserveVariableShapeAcrossModes));
            layer.right.smoothGraphicCurve = static_cast<bool> (
                rightTree.getProperty ("smoothGraphicCurve", layer.smoothGraphicCurve));
            for (int g = 0; g < 15; ++g)
                layer.right.graphic15Gains[static_cast<size_t> (g)] = static_cast<float> (
                    rightTree.getProperty ("graphic15_" + juce::String (g),
                                           layer.graphic15Gains[static_cast<size_t> (g)]));
            for (int g = 0; g < 31; ++g)
                layer.right.graphic31Gains[static_cast<size_t> (g)] = static_cast<float> (
                    rightTree.getProperty ("graphic31_" + juce::String (g),
                                           layer.graphic31Gains[static_cast<size_t> (g)]));

            const auto rightPoints = rightTree.getChildWithName ("Points");
            layer.right.points.clear();
            for (int p = 0; p < rightPoints.getNumChildren(); ++p)
            {
                const auto pointNode = rightPoints.getChild (p);
                layer.right.points.push_back ({
                    static_cast<double> (pointNode.getProperty ("f", 0.0)),
                    static_cast<double> (pointNode.getProperty ("db", 0.0)) });
            }
            if (layer.right.points.empty())
                layer.right.points = layer.points;

            const auto rightParams = rightTree.getChildWithName ("Parametric");
            layer.right.paramBands.clear();
            for (int b = 0; b < rightParams.getNumChildren(); ++b)
            {
                const auto bandNode = rightParams.getChild (b);
                FlexParamBand band;
                band.enabled = static_cast<bool> (bandNode.getProperty ("enabled", false));
                band.type = static_cast<FlexParamBand::Type> (
                    static_cast<int> (bandNode.getProperty ("type", 0)));
                band.frequency = static_cast<float> (bandNode.getProperty ("frequency", 1000.0f));
                band.gainDb = static_cast<float> (bandNode.getProperty ("gainDb", 0.0f));
                band.q = static_cast<float> (bandNode.getProperty ("q", 1.0f));
                layer.right.paramBands.push_back (band);
            }
            if (layer.right.paramBands.empty())
                layer.right.paramBands.resize (8);

            const auto rightFreeform = rightTree.getChildWithName ("Freeform");
            layer.right.freeformPoints.clear();
            for (int f = 0; f < rightFreeform.getNumChildren(); ++f)
            {
                const auto pointNode = rightFreeform.getChild (f);
                layer.right.freeformPoints.push_back ({
                    static_cast<double> (pointNode.getProperty ("f", 0.0)),
                    static_cast<double> (pointNode.getProperty ("db", 0.0)) });
            }
        }
        else
        {
            layer.selectedChannel = FlexChannelSelection::stereo;
            layer.channelsLinked = true;
        }

        if (! layer.points.empty())
        {
            const auto duplicateColour = std::any_of (restored.begin(), restored.end(), [&layer] (const auto& existing)
            {
                return existing.colour.getARGB() == layer.colour.getARGB();
            });
            if (duplicateColour)
                layer.colour = nextLayerColour (restored);

            maxId = juce::jmax (maxId, layer.id);
            restored.push_back (std::move (layer));
        }
    }

    const juce::ScopedLock lock (projectLock);
    layers = std::move (restored);
    nextLayerId = maxId + 1;
    if (activeLayerId <= 0 || findLayer (activeLayerId) == nullptr)
        activeLayerId = layers.empty() ? 0 : layers.front().id;
    finalCurve = calculateFinalCurveLocked();
}

void FlexCurveAudioProcessor::restoreRenderedCurveFromState (const juce::ValueTree& state)
{
    std::vector<CurvePoint> restored;
    std::vector<CurvePoint> restoredRight;
    const auto renderedTree = state.getChildWithName ("RenderedCurve");
    for (int i = 0; i < renderedTree.getNumChildren(); ++i)
    {
        const auto node = renderedTree.getChild (i);
        if (node.hasType ("Point"))
            restored.push_back ({ static_cast<double> (node.getProperty ("f", 0.0)),
                                  static_cast<double> (node.getProperty ("db", 0.0)) });
    }
    const auto rightTree = renderedTree.getChildWithName ("Right");
    for (int i = 0; i < rightTree.getNumChildren(); ++i)
    {
        const auto node = rightTree.getChild (i);
        restoredRight.push_back ({ static_cast<double> (node.getProperty ("f", 0.0)),
                                   static_cast<double> (node.getProperty ("db", 0.0)) });
    }
    if (restoredRight.empty())
        restoredRight = restored;

    const juce::ScopedLock lock (projectLock);
    renderedCurve = std::move (restored);
    renderedCurveRight = std::move (restoredRight);
}

bool FlexCurveAudioProcessor::rebuildConvolutionFromRenderedCurve()
{
    std::vector<CurvePoint> points;
    std::vector<CurvePoint> rightPoints;
    {
        const juce::ScopedLock lock (projectLock);
        points = renderedCurve;
        rightPoints = renderedCurveRight.empty() ? renderedCurve : renderedCurveRight;
    }

    if (points.empty() || currentSampleRate <= 0.0)
        return false;

    const auto phaseMode = getPhaseMode();
    const auto taps = getDefaultFirTapsForMode (phaseMode);
    auto impulse = createStereoImpulseForPhaseMode (points, rightPoints, currentSampleRate, taps);

    activeImpulsePeakSamples = findImpulseResponsePeak (impulse);
    applyActiveLatency (getLatencyForPhaseMode (phaseMode, impulse.getNumSamples()));
    convolution.loadImpulseResponse (std::move (impulse),
                                     currentSampleRate,
                                     impulse.getNumChannels() > 1 ? juce::dsp::Convolution::Stereo::yes
                                                                  : juce::dsp::Convolution::Stereo::no,
                                     juce::dsp::Convolution::Trim::no,
                                     juce::dsp::Convolution::Normalise::no);
    return true;
}

void FlexCurveAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    for (const auto& childType : { juce::Identifier ("Layers"),
                                  juce::Identifier ("RenderedCurve"),
                                  juce::Identifier ("AshPreview"),
                                  juce::Identifier ("Parametric"),
                                  juce::Identifier ("Freeform") })
    {
        for (;;)
        {
            const auto child = state.getChildWithName (childType);
            if (! child.isValid())
                break;
            state.removeChild (child, nullptr);
        }
    }

    {
        const juce::ScopedLock lock (projectLock);
        state.setProperty ("blendPerLayerMode", blendPerLayerMode, nullptr);
        state.setProperty ("bassGainDb", globalBlend.bassGainDb, nullptr);
        state.setProperty ("midGainDb", globalBlend.midGainDb, nullptr);
        state.setProperty ("trebleGainDb", globalBlend.trebleGainDb, nullptr);
        state.setProperty ("lowMidHz", globalBlend.lowMidCrossoverHz, nullptr);
        state.setProperty ("midHighHz", globalBlend.midHighCrossoverHz, nullptr);
        state.setProperty ("activeLayerId", activeLayerId, nullptr);
        state.setProperty ("averageEnabled", averageEnabled, nullptr);
        state.setProperty ("averageVisible", averageVisible, nullptr);
        state.setProperty ("presetName", presetName, nullptr);
        state.setProperty ("hasRenderedFir", hasRenderedFir.load(), nullptr);
        state.setProperty ("firOutdated", firOutdated.load(), nullptr);
        state.setProperty ("globalDbRange", globalDbRange.load(), nullptr);
        state.setProperty ("editLocked", editLocked.load(), nullptr);
        state.setProperty ("lastOpenTabIndex", lastOpenTabIndex.load(), nullptr);
        state.setProperty ("showEqLayers", isLayerTypeVisible (FlexCurveLayerType::eq), nullptr);
        state.setProperty ("showTargetLayers", isLayerTypeVisible (FlexCurveLayerType::target), nullptr);
        state.setProperty ("showRawLayers", isLayerTypeVisible (FlexCurveLayerType::raw), nullptr);
        state.setProperty ("showCorrectedMeasurements", areCorrectedMeasurementsVisible(), nullptr);
        state.setProperty ("renderedSampleRate", currentSampleRate, nullptr);
        state.setProperty ("renderedPhaseMode", static_cast<int> (getPhaseMode()), nullptr);
        state.setProperty ("ashCatalogSearch", ashCatalogSearch, nullptr);
        state.setProperty ("ashCatalogReviewer", ashCatalogReviewer, nullptr);
        state.setProperty ("ashCatalogMeasurement", ashCatalogMeasurement, nullptr);
        state.setProperty ("ashCatalogBrand", ashCatalogBrand, nullptr);
        state.setProperty ("ashCatalogModel", ashCatalogModel, nullptr);
        state.setProperty ("ashCatalogSelectedLabel", ashCatalogSelectedLabel, nullptr);
        state.setProperty ("ashPreviewLabel", ashPreviewLabel, nullptr);
        state.setProperty ("ashPreviewActive", ashPreviewActive.load(), nullptr);
        if (! ashPreviewCurve.empty())
        {
            juce::ValueTree previewTree ("AshPreview");
            for (const auto& point : ashPreviewCurve)
            {
                juce::ValueTree pointTree ("Point");
                pointTree.setProperty ("f", point.frequency, nullptr);
                pointTree.setProperty ("db", point.db, nullptr);
                previewTree.addChild (pointTree, -1, nullptr);
            }
            state.addChild (previewTree, -1, nullptr);
        }
    }

    addLayersToState (state);
    addRenderedCurveToState (state);

    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void FlexCurveAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml == nullptr || ! xml->hasTagName (parameters.state.getType()))
        return;

    auto state = juce::ValueTree::fromXml (*xml);
    auto parameterState = state.createCopy();
    for (const auto& childType : { juce::Identifier ("Layers"),
                                  juce::Identifier ("RenderedCurve"),
                                  juce::Identifier ("AshPreview"),
                                  juce::Identifier ("Parametric"),
                                  juce::Identifier ("Freeform") })
    {
        for (;;)
        {
            const auto child = parameterState.getChildWithName (childType);
            if (! child.isValid())
                break;
            parameterState.removeChild (child, nullptr);
        }
    }

    restoringState = true;
    parameters.replaceState (parameterState);

    {
        const juce::ScopedLock lock (projectLock);
        blendPerLayerMode = static_cast<bool> (state.getProperty ("blendPerLayerMode", true));
        globalBlend.bassGainDb = static_cast<float> (state.getProperty ("bassGainDb", 0.0f));
        globalBlend.midGainDb = static_cast<float> (state.getProperty ("midGainDb", 0.0f));
        globalBlend.trebleGainDb = static_cast<float> (state.getProperty ("trebleGainDb", 0.0f));
        globalBlend.lowMidCrossoverHz = static_cast<float> (state.getProperty ("lowMidHz", 250.0f));
        globalBlend.midHighCrossoverHz = static_cast<float> (state.getProperty ("midHighHz", 4000.0f));
        activeLayerId = static_cast<int> (state.getProperty ("activeLayerId", 0));
        averageEnabled = static_cast<bool> (state.getProperty ("averageEnabled", true));
        averageVisible = static_cast<bool> (state.getProperty ("averageVisible", true));
        presetName = state.getProperty ("presetName", "Untitled").toString();
        globalDbRange.store (juce::jlimit (1.0f, 96.0f, static_cast<float> (state.getProperty ("globalDbRange", 12.0f))));
        editLocked = static_cast<bool> (state.getProperty ("editLocked", false));
        lastOpenTabIndex = juce::jlimit (0, 3, static_cast<int> (state.getProperty ("lastOpenTabIndex", 0)));
        layerTypeVisible[static_cast<size_t> (FlexCurveLayerType::eq)]
            = static_cast<bool> (state.getProperty ("showEqLayers", true));
        layerTypeVisible[static_cast<size_t> (FlexCurveLayerType::target)]
            = static_cast<bool> (state.getProperty ("showTargetLayers", true));
        layerTypeVisible[static_cast<size_t> (FlexCurveLayerType::raw)]
            = static_cast<bool> (state.getProperty ("showRawLayers", true));
        correctedMeasurementsVisible
            = static_cast<bool> (state.getProperty ("showCorrectedMeasurements", true));
        ashCatalogSearch = state.getProperty ("ashCatalogSearch").toString();
        ashCatalogReviewer = state.getProperty ("ashCatalogReviewer").toString();
        ashCatalogMeasurement = state.getProperty ("ashCatalogMeasurement").toString();
        ashCatalogBrand = state.getProperty ("ashCatalogBrand").toString();
        ashCatalogModel = state.getProperty ("ashCatalogModel").toString();
        ashCatalogSelectedLabel = state.getProperty ("ashCatalogSelectedLabel").toString();
        ashPreviewLabel = state.getProperty ("ashPreviewLabel").toString();
        ashPreviewCurve.clear();
        const auto ashPreviewTree = state.getChildWithName ("AshPreview");
        for (int i = 0; i < ashPreviewTree.getNumChildren(); ++i)
        {
            const auto pointTree = ashPreviewTree.getChild (i);
            ashPreviewCurve.push_back ({ static_cast<double> (pointTree.getProperty ("f", 0.0)),
                                         static_cast<double> (pointTree.getProperty ("db", 0.0)) });
        }
        ashPreviewActive.store (static_cast<bool> (state.getProperty ("ashPreviewActive", false))
                                && ! ashPreviewCurve.empty());
    }

    restoreLayersFromState (state);
    restoreRenderedCurveFromState (state);

    if (! state.getChildWithName ("Parametric").isValid() && ! state.getChildWithName ("Freeform").isValid())
    {
        // New-format preset, nothing else to migrate.
    }
    else
    {
        const juce::ScopedLock lock (projectLock);
        if (layers.empty())
        {
            FlexCurveLayer layer;
            layer.id = nextLayerId++;
            layer.name = "Legacy Curve";
            layer.points = makeDefaultGrid();
            layer.colour = layerColourForIndex (0);
            layer.paramBands.resize (8);
            layers.push_back (std::move (layer));
            activeLayerId = layers.front().id;
        }

        if (auto* layer = getActiveLayer())
        {
            layer->graphicEnabled = static_cast<bool> (state.getProperty ("graphicEnabled", layer->graphicEnabled));
            if (state.hasProperty ("graphicMode"))
                layer->graphicMode = static_cast<int> (state.getProperty ("graphicMode", 31));
            else if (state.hasProperty ("graphicMode31"))
                layer->graphicMode = static_cast<bool> (state.getProperty ("graphicMode31", true)) ? 31 : 15;
            for (int i = 0; i < 31; ++i)
                if (state.hasProperty ("graphic" + juce::String (i)))
                {
                    const auto gain = static_cast<float> (state.getProperty ("graphic" + juce::String (i), 0.0f));
                    layer->graphic31Gains[static_cast<size_t> (i)] = gain;
                    if (i < 15)
                        layer->graphic15Gains[static_cast<size_t> (i)] = gain;
                }

            const auto paramsTree = state.getChildWithName ("Parametric");
            if (paramsTree.isValid())
            {
                layer->paramBands.clear();
                for (int i = 0; i < paramsTree.getNumChildren(); ++i)
                {
                    const auto node = paramsTree.getChild (i);
                    FlexParamBand band;
                    band.enabled = static_cast<bool> (node.getProperty ("enabled", false));
                    band.type = static_cast<FlexParamBand::Type> (static_cast<int> (node.getProperty ("type", 0)));
                    band.frequency = static_cast<float> (node.getProperty ("frequency", 1000.0f));
                    band.gainDb = static_cast<float> (node.getProperty ("gainDb", 0.0f));
                    band.q = static_cast<float> (node.getProperty ("q", 1.0f));
                    layer->paramBands.push_back (band);
                }
                if (layer->paramBands.empty())
                    layer->paramBands.resize (8);
            }

            const auto freeformTree = state.getChildWithName ("Freeform");
            if (freeformTree.isValid())
            {
                layer->freeformPoints.clear();
                for (int i = 0; i < freeformTree.getNumChildren(); ++i)
                {
                    const auto node = freeformTree.getChild (i);
                    layer->freeformPoints.push_back ({ static_cast<double> (node.getProperty ("f", 0.0)),
                                                       static_cast<double> (node.getProperty ("db", 0.0)) });
                }
            }

            copyLeftChannelToRight (*layer);
            layer->channelsLinked = true;
            layer->selectedChannel = FlexChannelSelection::stereo;
        }
        finalCurve = calculateFinalCurveLocked();
        finalCurveRight = calculateAverageCurveLocked (FlexChannelSelection::right);
    }

    {
        const juce::ScopedLock lock (projectLock);
        if (activeLayerId == -1 && ! averageEnabled)
            activeLayerId = layers.empty() ? 0 : layers.front().id;
        if (activeLayerId > 0 && findLayer (activeLayerId) == nullptr)
            activeLayerId = layers.empty() ? 0 : layers.front().id;
        finalCurve = calculateFinalCurveLocked();
        finalCurveRight = calculateAverageCurveLocked (FlexChannelSelection::right);
    }

    const auto shouldHaveRenderedFir = static_cast<bool> (state.getProperty ("hasRenderedFir", false));
    const auto shouldBeOutdated = static_cast<bool> (state.getProperty ("firOutdated", false));

    hasRenderedFir = false;
    firOutdated = false;
    activeImpulsePeakSamples = 0;
    applyActiveLatency (0);
    previewFiltersDirty = true;
    previewFirReady = false;

    if (shouldHaveRenderedFir)
    {
        if (rebuildConvolutionFromRenderedCurve())
        {
            hasRenderedFir = true;
            firOutdated = shouldBeOutdated;

            if (firOutdated)
            {
                if (auto* phase = parameters.getParameter ("phasemode"))
                    phase->setValueNotifyingHost (0.0f);
                applyActiveLatency (0);
            }
        }
        else
        {
            renderFir();
        }
    }

    updateFixedAutoGainFromCurrentCurve();
    restoringState = false;
    triggerAsyncUpdate();
}

void FlexCurveAudioProcessor::setEditLocked (bool shouldLock)
{
    editLocked = shouldLock;
}

void FlexCurveAudioProcessor::recordUndoState (const juce::String& actionKey)
{
    if (restoringHistory.load() || restoringState.load())
        return;

    const auto now = juce::Time::getMillisecondCounterHiRes();
    const auto nowMs = static_cast<juce::int64> (now);
    {
        const juce::ScopedLock lock (historyLock);
        if (actionKey == lastUndoActionKey && nowMs - lastUndoActionTimeMs < 350)
        {
            lastUndoActionTimeMs = nowMs;
            return;
        }
    }

    juce::MemoryBlock state;
    getStateInformation (state);

    const juce::ScopedLock lock (historyLock);
    undoHistory.push_back (std::move (state));
    if (undoHistory.size() > 64)
        undoHistory.erase (undoHistory.begin());
    redoHistory.clear();
    lastUndoActionKey = actionKey;
    lastUndoActionTimeMs = nowMs;
}

void FlexCurveAudioProcessor::restoreHistoryState (const juce::MemoryBlock& state)
{
    restoringHistory = true;
    setStateInformation (state.getData(), static_cast<int> (state.getSize()));
    restoringHistory = false;
}

bool FlexCurveAudioProcessor::canUndo() const
{
    const juce::ScopedLock lock (historyLock);
    return ! undoHistory.empty();
}

bool FlexCurveAudioProcessor::canRedo() const
{
    const juce::ScopedLock lock (historyLock);
    return ! redoHistory.empty();
}

void FlexCurveAudioProcessor::undo()
{
    if (editsBlocked())
        return;

    juce::MemoryBlock target;
    juce::MemoryBlock current;
    getStateInformation (current);
    {
        const juce::ScopedLock lock (historyLock);
        if (undoHistory.empty())
            return;
        target = undoHistory.back();
        undoHistory.pop_back();
        redoHistory.push_back (std::move (current));
        lastUndoActionKey.clear();
    }
    restoreHistoryState (target);
}

void FlexCurveAudioProcessor::redo()
{
    if (editsBlocked())
        return;

    juce::MemoryBlock target;
    juce::MemoryBlock current;
    getStateInformation (current);
    {
        const juce::ScopedLock lock (historyLock);
        if (redoHistory.empty())
            return;
        target = redoHistory.back();
        redoHistory.pop_back();
        undoHistory.push_back (std::move (current));
        lastUndoActionKey.clear();
    }
    restoreHistoryState (target);
}

juce::String FlexCurveAudioProcessor::getPresetName() const
{
    return presetName;
}

bool FlexCurveAudioProcessor::savePresetToFile (const juce::File& file, const juce::String& newPresetName)
{
    juce::MemoryBlock data;
    presetName = newPresetName;
    getStateInformation (data);

    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data.getData(), static_cast<int> (data.getSize())));
    if (xml == nullptr)
        return false;

    presetName = newPresetName;
    return xml->writeTo (file);
}

bool FlexCurveAudioProcessor::loadPresetFromFile (const juce::File& file)
{
    std::unique_ptr<juce::XmlElement> xml (juce::XmlDocument::parse (file));
    if (xml == nullptr || ! xml->hasTagName (parameters.state.getType()))
        return false;

    auto state = juce::ValueTree::fromXml (*xml);
    state.setProperty ("presetName", state.getProperty ("presetName", file.getFileNameWithoutExtension()).toString(), nullptr);

    std::unique_ptr<juce::XmlElement> stateXml (state.createXml());
    if (stateXml == nullptr)
        return false;

    juce::MemoryBlock data;
    copyXmlToBinary (*stateXml, data);
    setStateInformation (data.getData(), static_cast<int> (data.getSize()));
    presetName = state.getProperty ("presetName", file.getFileNameWithoutExtension()).toString();
    return true;
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new FlexCurveAudioProcessor();
}
