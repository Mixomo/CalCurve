#pragma once

#include <JuceHeader.h>
#include <atomic>
#include "CurveFIR.h"

struct FlexParamBand
{
    enum Type
    {
        peak = 0,
        lowShelf,
        highShelf,
        lowPass,
        highPass,
        notch
    };

    bool enabled = false;
    Type type = peak;
    float frequency = 1000.0f;
    float gainDb = 0.0f;
    float q = 1.0f;
};

struct FlexBlendSettings
{
    float bassGainDb = 0.0f;
    float midGainDb = 0.0f;
    float trebleGainDb = 0.0f;
    float lowMidCrossoverHz = 250.0f;
    float midHighCrossoverHz = 4000.0f;
};

enum class FlexCurveLayerType
{
    eq = 0,
    target = 1,
    raw = 2
};

struct FlexCurveLayer
{
    int id = 0;
    FlexCurveLayerType type = FlexCurveLayerType::eq;
    juce::String name;
    juce::File sourceFile;
    juce::String autoEqMethod;
    int autoEqRawLayerId = 0;
    int autoEqTargetLayerId = 0;
    bool autoEqSourcesOutdated = false;
    float autoEqReferenceOffsetDb = 0.0f;
    bool autoEqReferenceDisplayEnabled = true;
    std::vector<CurvePoint> points;
    juce::Colour colour = juce::Colours::white;
    bool enabled = true;
    bool muted = false;
    bool solo = false;
    bool visible = true;
    bool inverted = false;
    float gainDb = 0.0f;
    float normalizationOffsetDb = 0.0f;
    float opacity = 1.0f;
    bool smoothSourceCurve = false;
    FlexBlendSettings blend;
    bool graphicEnabled = false;
    int graphicMode = 31;
    bool preserveVariableShapeAcrossModes = false;
    bool smoothGraphicCurve = false;
    std::array<float, 15> graphic15Gains {};
    std::array<float, 31> graphic31Gains {};
    std::vector<FlexParamBand> paramBands;
    std::vector<CurvePoint> freeformPoints;
};

class FlexCurveAudioProcessor final : public juce::AudioProcessor,
                                      private juce::AudioProcessorValueTreeState::Listener,
                                      private juce::AsyncUpdater
{
public:
    struct MeterSnapshot
    {
        float inputPeakDb = -100.0f;
        float inputRmsDb = -100.0f;
        float preAutoPeakDb = -100.0f;
        float preAutoRmsDb = -100.0f;
        float outputPeakDb = -100.0f;
        float outputRmsDb = -100.0f;
        float deltaPeakDb = 0.0f;
        float deltaRmsDb = 0.0f;
        bool inputClipped = false;
        bool preAutoClipped = false;
        bool outputClipped = false;
        float autoGainDb = 0.0f;
    };

    enum class AutoEqMode
    {
        automatic,
        parametric,
        variable
    };

    enum class CurveExportFormat
    {
        firWav,
        frequencyText,
        frequencyCsv,
        graphicEq,
        meldaCsv,
        apoParametric
    };

    FlexCurveAudioProcessor();
    ~FlexCurveAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "FlexCurve"; }
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

    static constexpr int maxUserLayers = 6;
    bool addCurveFile (const juce::File& file);
    bool addReferenceCurveFile (const juce::File& file, FlexCurveLayerType type);
    bool addFlatCurve();
    bool cloneLayer (int id);
    bool canAddUserLayer() const;
    bool canAddLayerType (FlexCurveLayerType type) const;
    bool generateAutoEq (int rawLayerId, int targetLayerId, AutoEqMode mode);
    bool normalizeReferencesAtFrequency (int rawLayerId, int targetLayerId, double frequencyHz);
    void normalizeEqLayersToZeroDb();
    void setLayerTypeVisible (FlexCurveLayerType type, bool visible);
    bool isLayerTypeVisible (FlexCurveLayerType type) const noexcept;
    void setCorrectedMeasurementsVisible (bool visible);
    bool areCorrectedMeasurementsVisible() const noexcept;
    void removeLayer (int id);
    void setLayerEnabled (int id, bool enabled);
    void setLayerMuted (int id, bool muted);
    void setLayerVisible (int id, bool visible);
    void setLayerSolo (int id, bool solo);
    void setLayerGain (int id, float gainDb);
    void setLayerSourceSmoothing (int id, bool enabled);
    void toggleLayerInverted (int id);
    void setLayerOpacity (int id, float opacity);
    void renameLayer (int id, const juce::String& name);
    void resetLayerEdits (int id);
    void resetLayerToFlat (int id);
    void resetAllLayerCurvesToFlat();
    void resetAll();
    void setActiveLayerId (int id);
    int getActiveLayerId() const;
    bool hasEditableActiveLayer() const;
    void setAverageEnabled (bool enabled);
    bool isAverageEnabled() const;
    void setAverageVisible (bool visible);
    bool isAverageVisible() const;

    std::vector<FlexCurveLayer> getLayers() const;
    std::vector<CurvePoint> getFinalCurve() const;
    std::vector<CurvePoint> getRenderedCurve() const;
    std::vector<CurvePoint> getLayerCurve (int id) const;
    std::vector<CurvePoint> getLayerCurveForDisplay (int id) const;
    std::vector<CurvePoint> getLayerCurveWithoutFreeform (int id) const;
    std::vector<CurvePoint> getAverageCurve() const;
    std::vector<CurvePoint> getAverageCurveForType (FlexCurveLayerType type) const;
    std::vector<CurvePoint> getAverageCurveForTypeForDisplay (FlexCurveLayerType type) const;
    std::vector<CurvePoint> getCorrectedMeasurementCurve (int eqLayerId) const;
    double getAutoEqResidualRmsDb (int eqLayerId) const;
    float getGlobalDbRange() const noexcept { return globalDbRange.load(); }
    void setGlobalDbRange (float rangeDb);

    void setRegionSettings (float bassDb, float midDb, float trebleDb, float lowMidHz, float midHighHz);
    void getRegionSettings (float& bassDb, float& midDb, float& trebleDb, float& lowMidHz, float& midHighHz) const;
    void setBlendPerLayerMode (bool perLayer);
    bool isBlendPerLayerMode() const;
    void resetActiveLayerBlendToFlat();

    void copyCurrentGraphicEq();
    void pasteCurrentGraphicEq (bool inverted = false);
    bool canPasteCurrentGraphicEq() const;
    void copyCurrentParametricEq();
    void pasteCurrentParametricEq (bool inverted = false);
    bool canPasteCurrentParametricEq() const;

    bool canUndo() const;
    bool canRedo() const;
    void undo();
    void redo();

    void setGraphicMode31 (bool mode31);
    void setGraphicMode (int mode);
    void setGraphicEnabled (bool enabled);
    void setPreserveVariableShapeAcrossModes (bool preserve);
    bool isPreserveVariableShapeAcrossModes() const;
    void setGraphicSmoothing (bool enabled);
    bool isGraphicSmoothingEnabled() const;
    void setAllGraphicSmoothing (bool enabled);
    bool areAllGraphicCurvesSmoothed() const;
    bool isGraphicMode31() const;
    int getGraphicMode() const;
    bool isGraphicEnabled() const;
    int getGraphicBandCount() const;
    double getGraphicBandFrequency (int index) const;
    float getGraphicGain (int index) const;
    void setGraphicGain (int index, float gainDb);
    void resetGraphic();

    std::vector<FlexParamBand> getParamBands() const;
    void addParamBand();
    void removeParamBand (int index);
    void setParamBand (int index, const FlexParamBand& band);
    void resetParametric();

    std::vector<CurvePoint> getFreeformPoints() const;
    void setFreeformPoints (std::vector<CurvePoint> points);
    void resetFreeform();

    void renderFir();
    bool exportCurrentFirToFile (const juce::File& file);
    bool exportLayerToFile (int layerId, bool average, CurveExportFormat format, const juce::File& file);
    bool canExportRenderedFir() const noexcept { return hasRenderedFir.load() && ! firOutdated.load(); }
    MeterSnapshot getMeterSnapshot() const noexcept;
    void resetMeters() noexcept;
    bool savePresetToFile (const juce::File& file, const juce::String& presetName);
    bool loadPresetFromFile (const juce::File& file);
    juce::String getPresetName() const;
    void setEditLocked (bool shouldLock);
    bool isEditLocked() const noexcept { return editLocked.load(); }
    void setLastOpenTabIndex (int index) noexcept { lastOpenTabIndex.store (juce::jlimit (0, 2, index)); }
    int getLastOpenTabIndex() const noexcept { return lastOpenTabIndex.load(); }
    juce::File getPresetsDirectory() const
    {
        auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                       .getChildFile ("Mixomo")
                       .getChildFile ("FlexCurve")
                       .getChildFile ("UserPresets");
        dir.createDirectory();
        return dir;
    }
    juce::String getStatusText() const;
    bool canSelectPhaseMode() const noexcept { return hasRenderedFir.load() && ! firOutdated.load(); }
    int getActiveLatencySamples() const noexcept { return activeLatencySamples; }
    int getActiveImpulsePeakSamples() const noexcept { return activeImpulsePeakSamples; }
    double getCurrentSampleRate() const noexcept { return currentSampleRate; }

private:
    enum class PhaseMode
    {
        minimum = 0,
        natural = 1,
        linear = 2
    };

    struct Biquad
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double z1 = 0.0, z2 = 0.0;
        float process (float x) noexcept;
        void reset() noexcept { z1 = z2 = 0.0; }
    };

    void parameterChanged (const juce::String& parameterID, float newValue) override;
    void handleAsyncUpdate() override;
    PhaseMode getPhaseMode() const;
    int scaleReferenceSampleCount (int referenceSamples) const;
    int getDefaultFirTapsForMode (PhaseMode mode) const;
    int getLatencyForPhaseMode (PhaseMode mode, int taps) const;
    juce::AudioBuffer<float> createImpulseForPhaseMode (const std::vector<CurvePoint>& points, double sampleRate, int taps) const;
    int findImpulseResponsePeak (const juce::AudioBuffer<float>& impulse) const;
    void applyActiveLatency (int latencySamples);
    void markPreviewDirty();
    void updateFinalCurve();
    FlexCurveLayer* findLayer (int id);
    const FlexCurveLayer* findLayer (int id) const;
    FlexCurveLayer* getActiveLayer();
    const FlexCurveLayer* getActiveLayer() const;
    std::vector<CurvePoint> calculateLayerCurveLocked (const FlexCurveLayer& layer) const;
    std::vector<CurvePoint> calculateLayerCurveWithoutFreeformLocked (const FlexCurveLayer& layer) const;
    std::vector<CurvePoint> calculateAverageCurveLocked() const;
    std::vector<CurvePoint> calculateAverageCurveForTypeLocked (FlexCurveLayerType type,
                                                                 bool applyReferenceDisplayOffset = false) const;
    std::vector<CurvePoint> calculateFinalCurveLocked() const;
    std::vector<CurvePoint> calculateCorrectedMeasurementLocked (const FlexCurveLayer& eqLayer) const;
    const FlexCurveLayer* findAutoEqDisplayContextLocked (int referenceLayerId) const;
    float getReferenceDisplayOffsetLocked (int referenceLayerId) const;
    bool isEqLayerAudibleLocked (const FlexCurveLayer& layer) const;
    void markLinkedAutoEqLayersOutdatedLocked (int referenceLayerId);
    ParsedCurveData loadCurveData (const juce::File& file) const;
    void rebuildPreviewFilters();
    void applyPreviewFilters (juce::AudioBuffer<float>& buffer);
    void applyCrossfeed (juce::AudioBuffer<float>& buffer, float amount);
    void resetCrossfeed();
    void updateMeters (const juce::AudioBuffer<float>& buffer, bool input);
    void updateRuntimeAutoGain (float inputPower, float outputPower, float outputPeak,
                                float downstreamGain, int numSamples);
    void resetAutoGainState() noexcept;
    void addLayersToState (juce::ValueTree& state) const;
    void restoreLayersFromState (const juce::ValueTree& state);
    void addRenderedCurveToState (juce::ValueTree& state) const;
    void restoreRenderedCurveFromState (const juce::ValueTree& state);
    bool rebuildConvolutionFromRenderedCurve();
    bool editsBlocked() const noexcept { return editLocked.load() && ! restoringState.load(); }
    void recordUndoState (const juce::String& actionKey);
    void restoreHistoryState (const juce::MemoryBlock& state);
    std::vector<FlexParamBand> approximateCurveWithParametricFilters (const std::vector<CurvePoint>& points) const;

    static std::array<double, 31> graphic31Frequencies();
    static std::array<double, 15> graphic15Frequencies();
    static Biquad makePeak (double sampleRate, double frequency, double gainDb, double q);
    static Biquad makeShelf (double sampleRate, double frequency, double gainDb, double q, bool highShelf);
    static Biquad makeFilter (double sampleRate, const FlexParamBand& band);
    static double getBiquadMagnitudeDb (const Biquad& filter, double sampleRate, double frequency);

    juce::dsp::Convolution convolution;
    juce::dsp::Convolution previewConvolution;
    juce::AudioBuffer<float> wetBuffer;
    juce::AudioBuffer<float> dryDelayBuffer;
    int dryDelayWriteIndex = 0;
    int activeLatencySamples = 0;
    int activeImpulsePeakSamples = 0;
    std::atomic<bool> hasRenderedFir { false };
    std::atomic<bool> firOutdated { false };
    std::atomic<float> globalDbRange { 12.0f };
    std::array<std::atomic<bool>, 3> layerTypeVisible { true, true, true };
    std::atomic<bool> correctedMeasurementsVisible { true };
    std::atomic<bool> editLocked { false };
    std::atomic<int> lastOpenTabIndex { 0 };
    std::atomic<bool> restoringState { false };

    mutable juce::CriticalSection projectLock;
    std::vector<FlexCurveLayer> layers;
    std::vector<CurvePoint> finalCurve;
    std::vector<CurvePoint> renderedCurve;
    int nextLayerId = 1;
    int activeLayerId = 0;
    bool averageEnabled = true;
    bool averageVisible = true;
    juce::String presetName { "Untitled" };
    bool blendPerLayerMode = true;
    FlexBlendSettings globalBlend;
    enum class EqClipboardKind
    {
        none,
        graphic15,
        graphic31,
        variable,
        parametric
    };
    EqClipboardKind copiedEqKind = EqClipboardKind::none;
    bool copiedSmoothGraphicCurve = false;
    std::array<float, 15> copiedGraphic15Gains {};
    std::array<float, 31> copiedGraphic31Gains {};
    std::vector<FlexParamBand> copiedParamBands;
    std::vector<CurvePoint> copiedFreeformPoints;

    std::atomic<bool> previewFiltersDirty { true };
    std::atomic<bool> previewFirReady { false };
    std::atomic<float> runtimeAutoGainDb { 0.0f };
    std::atomic<float> smoothedRuntimeAutoGainDb { 0.0f };
    std::atomic<bool> autoGainResetRequested { false };
    double inputPowerIntegrator = 0.0;
    double outputPowerIntegrator = 0.0;
    juce::int64 autoGainSampleCount = 0;

    std::atomic<float> inputPeakDb { -100.0f };
    std::atomic<float> inputRmsDb { -100.0f };
    std::atomic<float> preAutoPeakDb { -100.0f };
    std::atomic<float> preAutoRmsDb { -100.0f };
    std::atomic<float> outputPeakDb { -100.0f };
    std::atomic<float> outputRmsDb { -100.0f };
    std::atomic<bool> inputClip { false };
    std::atomic<bool> preAutoClip { false };
    std::atomic<bool> outputClip { false };

    double currentSampleRate = 48000.0;
    float limiterGain = 1.0f;

    juce::AudioBuffer<float> crossfeedDelay;
    int crossfeedWrite = 0;
    float lpL = 0.0f;
    float lpR = 0.0f;

    mutable juce::CriticalSection historyLock;
    std::vector<juce::MemoryBlock> undoHistory;
    std::vector<juce::MemoryBlock> redoHistory;
    juce::String lastUndoActionKey;
    juce::int64 lastUndoActionTimeMs = 0;
    std::atomic<bool> restoringHistory { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FlexCurveAudioProcessor)
};
