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

    struct BufferPower
    {
        float peak = 0.0f;
        double meanSquare = 0.0;
    };

    BufferPower measureBufferPower (const juce::AudioBuffer<float>& buffer)
    {
        BufferPower result;
        double sum = 0.0;
        juce::int64 count = 0;
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            const auto* data = buffer.getReadPointer (channel);
            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            {
                const auto value = data[sample];
                result.peak = juce::jmax (result.peak, std::abs (value));
                sum += static_cast<double> (value) * value;
                ++count;
            }
        }
        result.meanSquare = count > 0 ? sum / static_cast<double> (count) : 0.0;
        return result;
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

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "gain", "Gain", juce::NormalisableRange<float> (-96.0f, 96.0f, 0.01f), 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "inputgain", "Input Gain", juce::NormalisableRange<float> (-36.0f, 36.0f, 0.01f), 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "outputgain", "Output Gain", juce::NormalisableRange<float> (-36.0f, 36.0f, 0.01f), 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterBool> ("autogain", "Auto Gain", true));
    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        "loudnessmatchmode", "Loudness Match Mode",
        juce::StringArray { "Output to Input", "Downward Match" }, 0));
    params.push_back (std::make_unique<juce::AudioParameterBool> (
        "includeoutputgainfir", "Include Output Gain in FIR Export", false));
    params.push_back (std::make_unique<juce::AudioParameterBool> (
        "includeautogainfir", "Include Auto Gain in FIR Export", false));

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

    crossfeedDelay.setSize (2, juce::jmax (32, static_cast<int> (std::ceil (sampleRate * 0.001))));
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

    const bool useFir = hasRenderedFir.load() && ! firOutdated.load() && dryWet > 0.0f;

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
    if (preAutoPeak > 1.0f)
        preAutoClip.store (true);

    updateRuntimeAutoGain (static_cast<float> (inputStats.meanSquare),
                           static_cast<float> (preAutoStats.meanSquare),
                           preAutoStats.peak,
                           downstreamGain,
                           buffer.getNumSamples());
    if (autoGainEnabled)
        buffer.applyGain (juce::Decibels::decibelsToGain (smoothedRuntimeAutoGainDb.load()));

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
            result.points = CurveFIR::createMagnitudeCurveFromImpulse (impulse, reader->sampleRate);
            result.hasExplicitGain = parseFlexCurveGainMetadata (reader->metadataValues, result.gainDb);
            if (result.hasExplicitGain)
                for (auto& point : result.points)
                    point.db -= result.gainDb;
        }
    }
    else
    {
        result = CurveFIR::parseCurveFileWithGain (file);
    }
    return result;
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
        layers.push_back (std::move (layer));
        if (activeLayerId == 0)
            activeLayerId = layers.back().id;
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
        layer.opacity = 1.0f;
        layer.paramBands.resize (8);
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
                layer.gainDb = gainDb;
                if (layer.type != FlexCurveLayerType::eq)
                    markLinkedAutoEqLayersOutdatedLocked (layer.id);
            }
        finalCurve = calculateFinalCurveLocked();
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
            layer->smoothSourceCurve = enabled;
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
            layer.gainDb = juce::jlimit (-next, next, layer.gainDb);
            layer.blend.bassGainDb = juce::jlimit (-next, next, layer.blend.bassGainDb);
            layer.blend.midGainDb = juce::jlimit (-next, next, layer.blend.midGainDb);
            layer.blend.trebleGainDb = juce::jlimit (-next, next, layer.blend.trebleGainDb);
            for (auto& gain : layer.graphic15Gains)
                gain = juce::jlimit (-next, next, gain);
            for (auto& gain : layer.graphic31Gains)
                gain = juce::jlimit (-next, next, gain);
            for (auto& band : layer.paramBands)
                band.gainDb = juce::jlimit (-next, next, band.gainDb);
            for (auto& point : layer.freeformPoints)
                point.db = juce::jlimit (static_cast<double> (-next), static_cast<double> (next), point.db);
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
            layer->inverted = ! layer->inverted;
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
            layer->gainDb = 0.0f;
            layer->normalizationOffsetDb = 0.0f;
            layer->inverted = false;
            layer->blend = {};
            layer->muted = false;
            layer->enabled = true;
            layer->solo = false;
            layer->opacity = 1.0f;
            layer->graphicEnabled = false;
            layer->graphicMode = 31;
            layer->preserveVariableShapeAcrossModes = false;
            layer->smoothGraphicCurve = false;
            layer->smoothSourceCurve = false;
            layer->graphic15Gains.fill (0.0f);
            layer->graphic31Gains.fill (0.0f);
            layer->paramBands.assign (8, {});
            layer->freeformPoints.clear();
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
            layer->points = makeDefaultGrid();
            layer->gainDb = 0.0f;
            layer->normalizationOffsetDb = 0.0f;
            layer->inverted = false;
            layer->blend = {};
            layer->graphic15Gains.fill (0.0f);
            layer->graphic31Gains.fill (0.0f);
            layer->freeformPoints.clear();
            layer->paramBands.assign (8, {});
            layer->graphicEnabled = false;
            layer->preserveVariableShapeAcrossModes = false;
            layer->smoothGraphicCurve = false;
            layer->smoothSourceCurve = false;
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
            layer.points = makeDefaultGrid();
            layer.gainDb = 0.0f;
            layer.normalizationOffsetDb = 0.0f;
            layer.inverted = false;
            layer.blend = {};
            layer.graphic15Gains.fill (0.0f);
            layer.graphic31Gains.fill (0.0f);
            layer.freeformPoints.clear();
            layer.paramBands.assign (8, {});
            layer.graphicEnabled = false;
            layer.preserveVariableShapeAcrossModes = false;
            layer.smoothGraphicCurve = false;
            layer.smoothSourceCurve = false;
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
        renderedCurve.clear();
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
    if (auto* p = parameters.getParameter ("gain")) p->setValueNotifyingHost (p->getDefaultValue());
    if (auto* p = parameters.getParameter ("inputgain")) p->setValueNotifyingHost (p->getDefaultValue());
    if (auto* p = parameters.getParameter ("outputgain")) p->setValueNotifyingHost (p->getDefaultValue());
    if (auto* p = parameters.getParameter ("autogain")) p->setValueNotifyingHost (p->getDefaultValue());
    if (auto* p = parameters.getParameter ("loudnessmatchmode")) p->setValueNotifyingHost (p->getDefaultValue());
    if (auto* p = parameters.getParameter ("includeoutputgainfir")) p->setValueNotifyingHost (p->getDefaultValue());
    if (auto* p = parameters.getParameter ("includeautogainfir")) p->setValueNotifyingHost (p->getDefaultValue());
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
        return calculateLayerCurveLocked (*layer);
    return {};
}

std::vector<CurvePoint> FlexCurveAudioProcessor::getLayerCurveForDisplay (int id) const
{
    const juce::ScopedLock lock (projectLock);
    const auto* layer = findLayer (id);
    if (layer == nullptr)
        return {};

    auto curve = calculateLayerCurveLocked (*layer);
    if (layer->type == FlexCurveLayerType::raw || layer->type == FlexCurveLayerType::target)
    {
        const auto displayOffset = getReferenceDisplayOffsetLocked (id);
        for (auto& point : curve)
            point.db += displayOffset;
    }
    return curve;
}

std::vector<CurvePoint> FlexCurveAudioProcessor::getLayerCurveWithoutFreeform (int id) const
{
    const juce::ScopedLock lock (projectLock);
    if (const auto* layer = findLayer (id))
        return calculateLayerCurveWithoutFreeformLocked (*layer);
    return {};
}

std::vector<CurvePoint> FlexCurveAudioProcessor::getAverageCurve() const
{
    const juce::ScopedLock lock (projectLock);
    return finalCurve;
}

std::vector<CurvePoint> FlexCurveAudioProcessor::getAverageCurveForType (FlexCurveLayerType type) const
{
    const juce::ScopedLock lock (projectLock);
    return calculateAverageCurveForTypeLocked (type);
}

std::vector<CurvePoint> FlexCurveAudioProcessor::getAverageCurveForTypeForDisplay (FlexCurveLayerType type) const
{
    const juce::ScopedLock lock (projectLock);
    return calculateAverageCurveForTypeLocked (type, true);
}

std::vector<CurvePoint> FlexCurveAudioProcessor::calculateCorrectedMeasurementLocked (const FlexCurveLayer& eqLayer) const
{
    const auto* raw = findLayer (eqLayer.autoEqRawLayerId);
    if (raw == nullptr || raw->type != FlexCurveLayerType::raw)
        return {};

    const auto rawCurve = calculateLayerCurveLocked (*raw);
    const auto eqCurve = calculateLayerCurveLocked (eqLayer);
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

float FlexCurveAudioProcessor::getReferenceDisplayOffsetLocked (int referenceLayerId) const
{
    if (const auto* context = findAutoEqDisplayContextLocked (referenceLayerId))
        return context->autoEqReferenceOffsetDb;
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
            layer.autoEqSourcesOutdated = true;
}

std::vector<CurvePoint> FlexCurveAudioProcessor::getCorrectedMeasurementCurve (int eqLayerId) const
{
    const juce::ScopedLock lock (projectLock);
    if (const auto* layer = findLayer (eqLayerId))
        return calculateCorrectedMeasurementLocked (*layer);
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

    const auto corrected = calculateCorrectedMeasurementLocked (*eq);
    auto targetCurve = calculateLayerCurveLocked (*target);
    if (eq->autoEqReferenceDisplayEnabled)
        for (auto& point : targetCurve)
            point.db += eq->autoEqReferenceOffsetDb;
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
        auto& target = blendPerLayerMode && active != nullptr ? active->blend : globalBlend;
        const auto range = globalDbRange.load();
        target.bassGainDb = juce::jlimit (-range, range, bassDb);
        target.midGainDb = juce::jlimit (-range, range, midDb);
        target.trebleGainDb = juce::jlimit (-range, range, trebleDb);
        target.lowMidCrossoverHz = juce::jlimit (60.0f, 1200.0f, lowMidHz);
        target.midHighCrossoverHz = juce::jlimit (1200.0f, 12000.0f, midHighHz);
        if (target.midHighCrossoverHz <= target.lowMidCrossoverHz * 1.5f)
            target.midHighCrossoverHz = target.lowMidCrossoverHz * 1.5f;
        finalCurve = calculateFinalCurveLocked();
    }
    markPreviewDirty();
}

void FlexCurveAudioProcessor::getRegionSettings (float& bassDb, float& midDb, float& trebleDb, float& lowMidHz, float& midHighHz) const
{
    const juce::ScopedLock lock (projectLock);
    const auto* active = getActiveLayer();
    const auto& source = blendPerLayerMode && active != nullptr ? active->blend : globalBlend;
    bassDb = source.bassGainDb;
    midDb = source.midGainDb;
    trebleDb = source.trebleGainDb;
    lowMidHz = source.lowMidCrossoverHz;
    midHighHz = source.midHighCrossoverHz;
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
                layer->points = makeDefaultGrid();
                layer->gainDb = 0.0f;
                layer->blend = {};
                layer->graphic15Gains.fill (0.0f);
                layer->graphic31Gains.fill (0.0f);
                layer->freeformPoints.clear();
                layer->paramBands.assign (8, {});
                layer->graphicEnabled = false;
                layer->preserveVariableShapeAcrossModes = false;
                layer->smoothGraphicCurve = false;
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
            layer->graphicMode = (mode == 0 ? 0 : (mode == 15 ? 15 : 31));
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
            layer->graphicEnabled = enabled;
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
            layer->preserveVariableShapeAcrossModes = preserve;
        finalCurve = calculateFinalCurveLocked();
    }
    markPreviewDirty();
}

bool FlexCurveAudioProcessor::isPreserveVariableShapeAcrossModes() const
{
    const juce::ScopedLock lock (projectLock);
    if (const auto* layer = getActiveLayer())
        return layer->preserveVariableShapeAcrossModes;
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
            layer->smoothGraphicCurve = enabled;
        finalCurve = calculateFinalCurveLocked();
    }
    markPreviewDirty();
}

bool FlexCurveAudioProcessor::isGraphicSmoothingEnabled() const
{
    const juce::ScopedLock lock (projectLock);
    if (const auto* layer = getActiveLayer())
        return layer->smoothGraphicCurve;
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
            layer.smoothGraphicCurve = enabled;
            layer.smoothSourceCurve = enabled;
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
        return layer.smoothGraphicCurve && layer.smoothSourceCurve;
    });
}

bool FlexCurveAudioProcessor::isGraphicMode31() const { return getGraphicMode() == 31; }

int FlexCurveAudioProcessor::getGraphicMode() const
{
    const juce::ScopedLock lock (projectLock);
    if (const auto* layer = getActiveLayer())
        return layer->graphicMode;
    return 31;
}

bool FlexCurveAudioProcessor::isGraphicEnabled() const
{
    const juce::ScopedLock lock (projectLock);
    if (const auto* layer = getActiveLayer())
        return layer->graphicEnabled;
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
        if (layer->graphicMode == 15)
            return layer->graphic15Gains[static_cast<size_t> (juce::jlimit (0, 14, index))];
        if (layer->graphicMode == 31)
            return layer->graphic31Gains[static_cast<size_t> (juce::jlimit (0, 30, index))];
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
            if (layer->graphicMode == 15)
                layer->graphic15Gains[static_cast<size_t> (juce::jlimit (0, 14, index))] = limited;
            else if (layer->graphicMode == 31)
                layer->graphic31Gains[static_cast<size_t> (juce::jlimit (0, 30, index))] = limited;
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
            if (layer->graphicMode == 15)
                layer->graphic15Gains.fill (0.0f);
            else if (layer->graphicMode == 31)
                layer->graphic31Gains.fill (0.0f);
            else
                layer->freeformPoints.clear();
        }
        finalCurve = calculateFinalCurveLocked();
    }
    markPreviewDirty();
}

std::vector<FlexParamBand> FlexCurveAudioProcessor::getParamBands() const
{
    const juce::ScopedLock lock (projectLock);
    if (const auto* layer = getActiveLayer())
        return layer->paramBands;
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
            FlexParamBand band;
            const auto index = static_cast<int> (layer->paramBands.size());
            band.frequency = static_cast<float> (juce::jlimit (20.0, 20000.0, 80.0 * std::pow (1.55, index)));
            layer->paramBands.push_back (band);
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
            if (index < static_cast<int> (layer->paramBands.size()))
                layer->paramBands.erase (layer->paramBands.begin() + index);
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
            if (index >= static_cast<int> (layer->paramBands.size()))
                layer->paramBands.resize (static_cast<size_t> (index + 1));
            auto safeBand = band;
            safeBand.frequency = juce::jlimit (20.0f, 20000.0f, safeBand.frequency);
            safeBand.gainDb = juce::jlimit (-globalDbRange.load(), globalDbRange.load(), safeBand.gainDb);
            safeBand.q = juce::jlimit (0.05f, 33.3333f, safeBand.q);
            layer->paramBands[static_cast<size_t> (index)] = safeBand;
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
            layer->paramBands.assign (8, {});
        finalCurve = calculateFinalCurveLocked();
    }
    markPreviewDirty();
}

std::vector<CurvePoint> FlexCurveAudioProcessor::getFreeformPoints() const
{
    const juce::ScopedLock lock (projectLock);
    if (const auto* layer = getActiveLayer())
        return layer->freeformPoints;
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
            layer->freeformPoints = std::move (points);
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
            layer->freeformPoints.clear();
        finalCurve = calculateFinalCurveLocked();
    }
    markPreviewDirty();
}

void FlexCurveAudioProcessor::copyCurrentGraphicEq()
{
    const juce::ScopedLock lock (projectLock);
    if (const auto* layer = getActiveLayer())
    {
        copiedEqKind = layer->graphicMode == 15 ? EqClipboardKind::graphic15
                     : layer->graphicMode == 31 ? EqClipboardKind::graphic31
                                                : EqClipboardKind::variable;
        copiedSmoothGraphicCurve = layer->smoothGraphicCurve;
        if (copiedEqKind == EqClipboardKind::graphic15)
            copiedGraphic15Gains = layer->graphic15Gains;
        else if (copiedEqKind == EqClipboardKind::graphic31)
            copiedGraphic31Gains = layer->graphic31Gains;
        else
            copiedFreeformPoints = layer->freeformPoints;
    }
}

bool FlexCurveAudioProcessor::canPasteCurrentGraphicEq() const
{
    const juce::ScopedLock lock (projectLock);
    const auto* layer = getActiveLayer();
    if (layer == nullptr)
        return false;

    if (layer->graphicMode == 0)
        return copiedEqKind == EqClipboardKind::graphic15
            || copiedEqKind == EqClipboardKind::graphic31
            || copiedEqKind == EqClipboardKind::variable;
    if (layer->graphicMode == 31)
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
            layer->graphicEnabled = true;
            layer->smoothGraphicCurve = copiedSmoothGraphicCurve;
            if (layer->graphicMode == 15)
            {
                for (size_t i = 0; i < layer->graphic15Gains.size(); ++i)
                    layer->graphic15Gains[i] = sign * copiedGraphic15Gains[i];
            }
            else if (layer->graphicMode == 31)
            {
                if (copiedEqKind == EqClipboardKind::graphic31)
                {
                    for (size_t i = 0; i < layer->graphic31Gains.size(); ++i)
                        layer->graphic31Gains[i] = sign * copiedGraphic31Gains[i];
                }
                else
                {
                    std::vector<CurvePoint> source;
                    source.reserve (copiedGraphic15Gains.size());
                    const auto frequencies = graphic15Frequencies();
                    for (size_t i = 0; i < copiedGraphic15Gains.size(); ++i)
                        source.push_back ({ frequencies[i], sign * copiedGraphic15Gains[i] });
                    const auto destinationFrequencies = graphic31Frequencies();
                    for (size_t i = 0; i < layer->graphic31Gains.size(); ++i)
                        layer->graphic31Gains[i] = static_cast<float> (
                            CurveFIR::interpolateDb (source, destinationFrequencies[i]));
                }
            }
            else
            {
                layer->freeformPoints.clear();
                if (copiedEqKind == EqClipboardKind::variable)
                {
                    layer->freeformPoints = copiedFreeformPoints;
                    for (auto& point : layer->freeformPoints)
                        point.db *= sign;
                }
                else
                {
                    const auto count = copiedEqKind == EqClipboardKind::graphic31 ? 31 : 15;
                    layer->freeformPoints.reserve (static_cast<size_t> (count));
                    for (int i = 0; i < count; ++i)
                    {
                        const auto frequency = copiedEqKind == EqClipboardKind::graphic31
                            ? graphic31Frequencies()[static_cast<size_t> (i)]
                            : graphic15Frequencies()[static_cast<size_t> (i)];
                        const auto gain = copiedEqKind == EqClipboardKind::graphic31
                            ? copiedGraphic31Gains[static_cast<size_t> (i)]
                            : copiedGraphic15Gains[static_cast<size_t> (i)];
                        layer->freeformPoints.push_back ({ frequency, sign * gain });
                    }
                }
            }

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
        copiedParamBands = layer->paramBands;
    }
}

bool FlexCurveAudioProcessor::canPasteCurrentParametricEq() const
{
    const juce::ScopedLock lock (projectLock);
    return copiedEqKind == EqClipboardKind::parametric && getActiveLayer() != nullptr;
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
            layer->paramBands = copiedParamBands;
            if (layer->paramBands.empty())
                layer->paramBands.resize (8);
            if (inverted)
                for (auto& band : layer->paramBands)
                    band.gainDb = -band.gainDb;
            finalCurve = calculateFinalCurveLocked();
        }
    }
    markPreviewDirty();
}

void FlexCurveAudioProcessor::markPreviewDirty()
{
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

std::vector<CurvePoint> FlexCurveAudioProcessor::calculateLayerCurveLocked (const FlexCurveLayer& layer) const
{
    auto points = makeDefaultGrid();
    auto graphicContribution = makeDefaultGrid();

    for (auto& point : points)
        point.db = CurveFIR::interpolateDb (layer.points, point.frequency);

    if (layer.smoothSourceCurve)
        smoothCurveGeometry (points);

    for (auto& point : points)
        point.db += layer.gainDb + layer.normalizationOffsetDb;

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

std::vector<CurvePoint> FlexCurveAudioProcessor::calculateLayerCurveWithoutFreeformLocked (const FlexCurveLayer& layer) const
{
    auto copy = layer;
    copy.freeformPoints.clear();
    return calculateLayerCurveLocked (copy);
}

std::vector<CurvePoint> FlexCurveAudioProcessor::calculateAverageCurveLocked() const
{
    return calculateAverageCurveForTypeLocked (FlexCurveLayerType::eq);
}

std::vector<CurvePoint> FlexCurveAudioProcessor::calculateAverageCurveForTypeLocked (
    FlexCurveLayerType type, bool applyReferenceDisplayOffset) const
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
        auto curve = calculateLayerCurveLocked (*layer);
        if (applyReferenceDisplayOffset
            && (type == FlexCurveLayerType::raw || type == FlexCurveLayerType::target))
        {
            const auto displayOffset = getReferenceDisplayOffsetLocked (layer->id);
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
    return calculateAverageCurveLocked();
}

void FlexCurveAudioProcessor::updateFinalCurve()
{
    const juce::ScopedLock lock (projectLock);
    finalCurve = calculateFinalCurveLocked();
}

void FlexCurveAudioProcessor::renderFir()
{
    if (editsBlocked())
        return;

    recordUndoState ("render-fir");
    updateFinalCurve();

    std::vector<CurvePoint> points;
    {
        const juce::ScopedLock lock (projectLock);
        points = finalCurve;
        renderedCurve = finalCurve;
    }

    if (points.empty())
        return;

    const auto phaseMode = getPhaseMode();
    const auto taps = getDefaultFirTapsForMode (phaseMode);
    auto impulse = createImpulseForPhaseMode (points, currentSampleRate, taps);

    activeImpulsePeakSamples = findImpulseResponsePeak (impulse);
    applyActiveLatency (getLatencyForPhaseMode (phaseMode, impulse.getNumSamples()));
    convolution.loadImpulseResponse (std::move (impulse),
                                     currentSampleRate,
                                     juce::dsp::Convolution::Stereo::no,
                                     juce::dsp::Convolution::Trim::no,
                                     juce::dsp::Convolution::Normalise::no);

    {
        const juce::ScopedLock lock (projectLock);
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

        layers.clear();
        layers.push_back (std::move (renderedLayer));
        activeLayerId = layers.front().id;
        averageEnabled = true;
        averageVisible = false;
        globalBlend = {};
        blendPerLayerMode = true;
        finalCurve = points;
        renderedCurve = points;
    }

    hasRenderedFir = true;
    firOutdated = false;
    editLocked = true;
}

bool FlexCurveAudioProcessor::exportCurrentFirToFile (const juce::File& file)
{
    if (! canExportRenderedFir())
        return false;

    std::vector<CurvePoint> points;
    {
        const juce::ScopedLock lock (projectLock);
        points = renderedCurve;
    }

    if (points.empty())
        return false;

    double exportedGainDb = 0.0;
    if (parameters.getRawParameterValue ("includeoutputgainfir")->load() > 0.5f)
        exportedGainDb += parameters.getRawParameterValue ("outputgain")->load();
    if (parameters.getRawParameterValue ("includeautogainfir")->load() > 0.5f)
        exportedGainDb += smoothedRuntimeAutoGainDb.load();
    if (! juce::approximatelyEqual (exportedGainDb, 0.0))
        for (auto& point : points)
            point.db += exportedGainDb;

    const auto taps = getDefaultFirTapsForMode (getPhaseMode());
    auto impulse = createImpulseForPhaseMode (points, currentSampleRate, taps);

    juce::WavAudioFormat wavFormat;
    if (file.existsAsFile() && ! file.deleteFile())
        return false;

    std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
    if (stream == nullptr || ! stream->openedOk())
        return false;

    const auto metadata = makeFlexCurveFirMetadata (exportedGainDb);
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wavFormat.createWriterFor (stream.get(), currentSampleRate, 1, 32, metadata, 0));
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

        const auto rawCurve = calculateLayerCurveLocked (*raw);
        const auto targetCurve = calculateLayerCurveLocked (*target);
        raw->normalizationOffsetDb -= static_cast<float> (CurveFIR::interpolateDb (rawCurve, frequencyHz));
        target->normalizationOffsetDb -= static_cast<float> (CurveFIR::interpolateDb (targetCurve, frequencyHz));
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

            const auto curve = calculateLayerCurveLocked (layer);
            if (curve.empty())
                continue;

            const auto peak = std::max_element (curve.begin(), curve.end(),
                                                [] (const auto& lhs, const auto& rhs)
                                                {
                                                    return lhs.db < rhs.db;
                                                })->db;
            layer.gainDb += static_cast<float> (layer.inverted ? peak : -peak);
        }
        finalCurve = calculateFinalCurveLocked();
    }
    markPreviewDirty();
}

bool FlexCurveAudioProcessor::generateAutoEq (int rawLayerId, int targetLayerId, AutoEqMode mode)
{
    if (editsBlocked() || ! canAddUserLayer())
        return false;

    std::vector<CurvePoint> correction = makeDefaultGrid();
    juce::String rawName;
    juce::String targetName;
    {
        const juce::ScopedLock lock (projectLock);
        const auto* raw = findLayer (rawLayerId);
        const auto* target = findLayer (targetLayerId);
        if (raw == nullptr || target == nullptr
            || raw->type != FlexCurveLayerType::raw
            || target->type != FlexCurveLayerType::target)
            return false;

        rawName = raw->name;
        targetName = target->name;
        const auto rawCurve = calculateLayerCurveLocked (*raw);
        const auto targetCurve = calculateLayerCurveLocked (*target);
        for (auto& point : correction)
            point.db = juce::jlimit (-12.0, 12.0,
                                     CurveFIR::interpolateDb (targetCurve, point.frequency)
                                     - CurveFIR::interpolateDb (rawCurve, point.frequency));
    }

    smoothCurveGeometry (correction);
    smoothCurveGeometry (correction);
    auto parametric = approximateCurveWithParametricFilters (correction);

    auto selectedMode = mode;
    if (mode == AutoEqMode::automatic)
    {
        double squaredError = 0.0;
        for (const auto& point : correction)
        {
            double approximation = 0.0;
            for (const auto& band : parametric)
                approximation += getBiquadMagnitudeDb (makeFilter (currentSampleRate, band),
                                                       currentSampleRate,
                                                       point.frequency);
            const auto error = approximation - point.db;
            squaredError += error * error;
        }
        const auto rmsError = std::sqrt (squaredError / juce::jmax (size_t (1), correction.size()));
        selectedMode = rmsError <= 1.0 && parametric.size() <= 16
            ? AutoEqMode::parametric
            : AutoEqMode::variable;
    }

    auto generatedResponse = makeDefaultGrid();
    if (selectedMode == AutoEqMode::parametric)
    {
        for (auto& point : generatedResponse)
            for (const auto& band : parametric)
                if (band.enabled)
                    point.db += getBiquadMagnitudeDb (makeFilter (currentSampleRate, band),
                                                      currentSampleRate,
                                                      point.frequency);
    }
    else
    {
        generatedResponse = correction;
        smoothCurveGeometry (generatedResponse);
    }

    const auto generatedPeakDb = static_cast<float> (std::max_element (
        generatedResponse.begin(), generatedResponse.end(),
        [] (const auto& lhs, const auto& rhs) { return lhs.db < rhs.db; })->db);

    recordUndoState ("generate-autoeq");
    {
        const juce::ScopedLock lock (projectLock);
        auto* target = findLayer (targetLayerId);
        if (target == nullptr || target->type != FlexCurveLayerType::target)
            return false;

        auto* raw = findLayer (rawLayerId);
        if (raw == nullptr || raw->type != FlexCurveLayerType::raw)
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
        layer.autoEqReferenceOffsetDb = -generatedPeakDb;
        layer.autoEqReferenceDisplayEnabled = true;
        layer.autoEqMethod = selectedMode == AutoEqMode::parametric ? "Parametric" : "Variable";
        layer.gainDb = -generatedPeakDb;
        layer.paramBands.resize (8);

        if (selectedMode == AutoEqMode::parametric)
        {
            layer.paramBands = std::move (parametric);
            if (layer.paramBands.empty())
                layer.paramBands.resize (8);
        }
        else
        {
            layer.graphicEnabled = true;
            layer.graphicMode = 0;
            layer.freeformPoints = std::move (correction);
            layer.smoothGraphicCurve = true;
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
    double separateGainDb = 0.0;
    const auto supportsSeparateGain = format == CurveExportFormat::firWav
                                   || format == CurveExportFormat::graphicEq
                                   || format == CurveExportFormat::apoParametric;
    {
        const juce::ScopedLock lock (projectLock);
        if (average)
            points = calculateAverageCurveLocked();
        else if (const auto* layer = findLayer (layerId))
        {
            if (supportsSeparateGain)
            {
                auto baseLayer = *layer;
                baseLayer.gainDb = 0.0f;
                points = calculateLayerCurveLocked (baseLayer);
                separateGainDb = layer->inverted ? -layer->gainDb : layer->gainDb;
            }
            else
            {
                points = calculateLayerCurveLocked (*layer);
            }
        }
    }

    if (points.empty())
        return false;

    if (format == CurveExportFormat::firWav)
    {
        if (parameters.getRawParameterValue ("includeoutputgainfir")->load() > 0.5f)
            separateGainDb += parameters.getRawParameterValue ("outputgain")->load();
        if (parameters.getRawParameterValue ("includeautogainfir")->load() > 0.5f)
            separateGainDb += smoothedRuntimeAutoGainDb.load();
        auto firPoints = points;
        if (! juce::approximatelyEqual (separateGainDb, 0.0))
            for (auto& point : firPoints)
                point.db += separateGainDb;
        const auto phaseMode = getPhaseMode();
        const auto taps = getDefaultFirTapsForMode (phaseMode);
        auto impulse = createImpulseForPhaseMode (firPoints, currentSampleRate, taps);
        juce::WavAudioFormat wav;
        if (file.existsAsFile() && ! file.deleteFile())
            return false;
        auto stream = file.createOutputStream();
        if (stream == nullptr || ! stream->openedOk())
            return false;
        const auto metadata = makeFlexCurveFirMetadata (separateGainDb);
        std::unique_ptr<juce::AudioFormatWriter> writer (
            wav.createWriterFor (stream.get(), currentSampleRate, 1, 32, metadata, 0));
        if (writer == nullptr)
            return false;
        stream.release();
        return writer->writeFromAudioSampleBuffer (impulse, 0, impulse.getNumSamples());
    }

    juce::String text;
    if (format == CurveExportFormat::graphicEq)
    {
        if (! average)
            text << "Preamp: " << juce::String (separateGainDb, 5) << " dB" << newLine;
        text << "GraphicEQ: ";
        for (size_t i = 0; i < points.size(); ++i)
        {
            if (i != 0)
                text << "; ";
            text << juce::String (points[i].frequency, 3) << " " << juce::String (points[i].db, 4);
        }
        text << newLine;
    }
    else if (format == CurveExportFormat::apoParametric)
    {
        if (! average)
            text << "Preamp: " << juce::String (separateGainDb, 5) << " dB" << newLine;
        const auto parametric = approximateCurveWithParametricFilters (points);
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
        if (text.isEmpty())
            return false;
    }
    else
    {
        if (format == CurveExportFormat::frequencyCsv)
            text = "frequency,db\n";
        else if (format == CurveExportFormat::meldaCsv)
            text = "Frequency (Hz),Gain (dB)\n";

        const auto separator = format == CurveExportFormat::frequencyText ? "\t" : ",";
        for (const auto& point : points)
            text << juce::String (point.frequency, 5) << separator << juce::String (point.db, 5) << newLine;
    }

    if (file.existsAsFile() && ! file.deleteFile())
        return false;
    return file.replaceWithText (text);
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

    if (input)
    {
        updateBallistic (inputPeakDb, peakDb);
        updateBallistic (inputRmsDb, rmsDb);
        if (stats.peak > 1.0f)
            inputClip.store (true);
    }
    else
    {
        updateBallistic (outputPeakDb, peakDb);
        updateBallistic (outputRmsDb, rmsDb);
        if (stats.peak > 1.0f)
            outputClip.store (true);
    }
}

void FlexCurveAudioProcessor::updateRuntimeAutoGain (float inputPower, float outputPower, float outputPeak,
                                                     float downstreamGain, int numSamples)
{
    if (autoGainResetRequested.exchange (false))
        resetAutoGainState();

    const auto enabled = parameters.getRawParameterValue ("autogain")->load() > 0.5f;
    const auto mode = juce::roundToInt (parameters.getRawParameterValue ("loudnessmatchmode")->load());
    const auto predictedPeak = outputPeak * downstreamGain;
    const auto headroomLimitDb = predictedPeak > 1.0e-6f
        ? juce::jlimit (-18.0f, 18.0f,
                       juce::Decibels::gainToDecibels (0.999f / predictedPeak, -18.0f))
        : 18.0f;
    if (inputPower > 1.0e-10f && outputPower > 1.0e-10f)
    {
        inputPowerIntegrator += static_cast<double> (inputPower) * numSamples;
        outputPowerIntegrator += static_cast<double> (outputPower) * numSamples;
        autoGainSampleCount += numSamples;
    }

    const auto updateSamples = static_cast<juce::int64> (juce::jmax (1.0, currentSampleRate * 0.40));
    if (autoGainSampleCount >= updateSamples)
    {
        const auto ratio = std::sqrt (inputPowerIntegrator / juce::jmax (1.0e-20, outputPowerIntegrator));
        auto targetDb = static_cast<float> (juce::jlimit (-18.0, 18.0,
            juce::Decibels::gainToDecibels (ratio, -18.0)));

        if (mode == 1)
        {
            targetDb = juce::jmin (0.0f, targetDb, headroomLimitDb);
            runtimeAutoGainDb.store (juce::jmin (runtimeAutoGainDb.load(), targetDb));
        }
        else
        {
            runtimeAutoGainDb.store (juce::jmin (targetDb, headroomLimitDb));
        }
        inputPowerIntegrator = 0.0;
        outputPowerIntegrator = 0.0;
        autoGainSampleCount = 0;
    }

    auto smoothedDb = smoothedRuntimeAutoGainDb.load();
    if (enabled && headroomLimitDb < smoothedDb)
    {
        runtimeAutoGainDb.store (juce::jmin (runtimeAutoGainDb.load(), headroomLimitDb));
        smoothedDb = headroomLimitDb;
    }

    const auto targetDb = enabled ? runtimeAutoGainDb.load() : 0.0f;
    const auto movingDown = targetDb < smoothedDb;
    const auto timeSeconds = movingDown ? 0.20 : (mode == 1 ? 30.0 : 1.25);
    const auto smoothing = static_cast<float> (1.0 - std::exp (
        -static_cast<double> (numSamples) / juce::jmax (1.0, currentSampleRate * timeSeconds)));
    smoothedDb += (targetDb - smoothedDb) * smoothing;
    smoothedRuntimeAutoGainDb.store (smoothedDb);
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
    result.autoGainDb = smoothedRuntimeAutoGainDb.load();
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
    inputClip.store (false);
    preAutoClip.store (false);
    outputClip.store (false);
    inputPowerIntegrator = 0.0;
    outputPowerIntegrator = 0.0;
    autoGainSampleCount = 0;
}

void FlexCurveAudioProcessor::resetAutoGainState() noexcept
{
    runtimeAutoGainDb.store (0.0f);
    smoothedRuntimeAutoGainDb.store (0.0f);
    inputPowerIntegrator = 0.0;
    outputPowerIntegrator = 0.0;
    autoGainSampleCount = 0;
}

void FlexCurveAudioProcessor::parameterChanged (const juce::String& parameterID, float)
{
    if (restoringState.load())
        return;

    if (parameterID == "phasemode" && hasRenderedFir && ! firOutdated)
        triggerAsyncUpdate();
    else if (parameterID == "autogain" || parameterID == "loudnessmatchmode")
        autoGainResetRequested.store (true);
}

void FlexCurveAudioProcessor::handleAsyncUpdate()
{
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
    const auto points = getFinalCurve();
    if (points.empty() || currentSampleRate <= 0.0)
    {
        previewFirReady.store (false);
        return;
    }

    // GraphicEQ-style preview: curve edits are converted to a causal
    // minimum-phase FIR outside processBlock, then swapped into JUCE's
    // partitioned convolution engine.
    const auto taps = scaleReferenceSampleCount (minimumPhaseReferenceTaps);
    auto impulse = CurveFIR::createMinimumPhaseFIR (points, currentSampleRate, taps);
    previewConvolution.loadImpulseResponse (std::move (impulse),
                                            currentSampleRate,
                                            juce::dsp::Convolution::Stereo::no,
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
}

void FlexCurveAudioProcessor::applyCrossfeed (juce::AudioBuffer<float>& buffer, float amount)
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
    const auto feed = juce::jmap (amount, 0.0f, 0.28f);

    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        const auto inL = left[i];
        const auto inR = right[i];
        delayL[crossfeedWrite] = inL;
        delayR[crossfeedWrite] = inR;
        const auto read = (crossfeedWrite - juce::jmax (1, static_cast<int> (currentSampleRate * 0.00028)) + delaySize) % delaySize;
        lpL += lpAlpha * (delayL[read] - lpL);
        lpR += lpAlpha * (delayR[read] - lpR);
        const auto mid = 0.5f * (inL + inR);
        const auto side = 0.5f * (inL - inR) * width;
        left[i] = (mid + side) + feed * lpR;
        right[i] = (mid - side) + feed * lpL;
        crossfeedWrite = (crossfeedWrite + 1) % delaySize;
    }
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
        node.setProperty ("colour", static_cast<int> (layer.colour.getARGB()), nullptr);
        node.setProperty ("enabled", layer.enabled, nullptr);
        node.setProperty ("muted", layer.muted, nullptr);
        node.setProperty ("solo", layer.solo, nullptr);
        node.setProperty ("visible", layer.visible, nullptr);
        node.setProperty ("inverted", layer.inverted, nullptr);
        node.setProperty ("gainDb", layer.gainDb, nullptr);
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
        layer.colour = juce::Colour (static_cast<juce::uint32> (static_cast<int> (node.getProperty ("colour", static_cast<int> (layerColourForIndex (i).getARGB())))));
        layer.enabled = static_cast<bool> (node.getProperty ("enabled", true));
        layer.muted = static_cast<bool> (node.getProperty ("muted", ! layer.enabled));
        layer.solo = static_cast<bool> (node.getProperty ("solo", false));
        layer.visible = static_cast<bool> (node.getProperty ("visible", true));
        layer.inverted = static_cast<bool> (node.getProperty ("inverted", false));
        layer.gainDb = static_cast<float> (node.getProperty ("gainDb", 0.0f));
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
    const auto renderedTree = state.getChildWithName ("RenderedCurve");
    for (int i = 0; i < renderedTree.getNumChildren(); ++i)
    {
        const auto node = renderedTree.getChild (i);
        restored.push_back ({ static_cast<double> (node.getProperty ("f", 0.0)),
                              static_cast<double> (node.getProperty ("db", 0.0)) });
    }

    const juce::ScopedLock lock (projectLock);
    renderedCurve = std::move (restored);
}

bool FlexCurveAudioProcessor::rebuildConvolutionFromRenderedCurve()
{
    std::vector<CurvePoint> points;
    {
        const juce::ScopedLock lock (projectLock);
        points = renderedCurve;
    }

    if (points.empty() || currentSampleRate <= 0.0)
        return false;

    const auto phaseMode = getPhaseMode();
    const auto taps = getDefaultFirTapsForMode (phaseMode);
    auto impulse = createImpulseForPhaseMode (points, currentSampleRate, taps);

    activeImpulsePeakSamples = findImpulseResponsePeak (impulse);
    applyActiveLatency (getLatencyForPhaseMode (phaseMode, impulse.getNumSamples()));
    convolution.loadImpulseResponse (std::move (impulse),
                                     currentSampleRate,
                                     juce::dsp::Convolution::Stereo::no,
                                     juce::dsp::Convolution::Trim::no,
                                     juce::dsp::Convolution::Normalise::no);
    return true;
}

void FlexCurveAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    for (const auto& childType : { juce::Identifier ("Layers"),
                                  juce::Identifier ("RenderedCurve"),
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
        state.setProperty ("runtimeAutoGainDb", runtimeAutoGainDb.load(), nullptr);
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
        lastOpenTabIndex = juce::jlimit (0, 2, static_cast<int> (state.getProperty ("lastOpenTabIndex", 0)));
        layerTypeVisible[static_cast<size_t> (FlexCurveLayerType::eq)]
            = static_cast<bool> (state.getProperty ("showEqLayers", true));
        layerTypeVisible[static_cast<size_t> (FlexCurveLayerType::target)]
            = static_cast<bool> (state.getProperty ("showTargetLayers", true));
        layerTypeVisible[static_cast<size_t> (FlexCurveLayerType::raw)]
            = static_cast<bool> (state.getProperty ("showRawLayers", true));
        correctedMeasurementsVisible
            = static_cast<bool> (state.getProperty ("showCorrectedMeasurements", true));
        runtimeAutoGainDb.store (static_cast<float> (state.getProperty ("runtimeAutoGainDb", 0.0f)));
        smoothedRuntimeAutoGainDb.store (runtimeAutoGainDb.load());
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
        }
        finalCurve = calculateFinalCurveLocked();
    }

    {
        const juce::ScopedLock lock (projectLock);
        if (activeLayerId == -1 && ! averageEnabled)
            activeLayerId = layers.empty() ? 0 : layers.front().id;
        if (activeLayerId > 0 && findLayer (activeLayerId) == nullptr)
            activeLayerId = layers.empty() ? 0 : layers.front().id;
        finalCurve = calculateFinalCurveLocked();
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
