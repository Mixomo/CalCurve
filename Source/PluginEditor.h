#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

class CurveDisplay : public juce::Component
{
public:
    explicit CurveDisplay (CalCurveAudioProcessor&);
    void paint (juce::Graphics&) override;
    void restartDraw();

private:
    juce::Path buildPath (const std::vector<CurvePoint>& points,
                          juce::Rectangle<float> graph,
                          float scale,
                          double minDb,
                          double maxDb) const;
    juce::Point<float> mapPoint (double frequency,
                                 double db,
                                 juce::Rectangle<float> graph,
                                 double minDb,
                                 double maxDb) const;

    CalCurveAudioProcessor& processor;
    float drawProgress = 1.0f;
    int lastCurveHash = 0;
};

class CalCurveAudioProcessorEditor : public juce::AudioProcessorEditor,
                                        private juce::Timer
{
public:
    explicit CalCurveAudioProcessorEditor (CalCurveAudioProcessor&);
    ~CalCurveAudioProcessorEditor() override = default;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void openFileChooser();
    void showHelp();
    void styleSlider (juce::Slider& slider, const juce::String& suffix = {});
    void updatePresetCombo();
    void handlePresetComboChange();

    CalCurveAudioProcessor& processor;

    CurveDisplay display;
    juce::Label title;
    juce::Label loadedFile;
    juce::Slider dryWet;
    juce::Slider crossfeed;
    juce::Slider gain;
    juce::Label dryWetLabel;
    juce::Label crossfeedLabel;
    juce::Label gainLabel;
    juce::Label phaseModeLabel;
    juce::Label latencyLabel;
    juce::ComboBox phaseMode;
    juce::ToggleButton limiter;
    juce::ToggleButton bypassButton;
    juce::TextButton loadFile { "Load TXT / CSV / FIR" };
    juce::TextButton help { "Help" };
    juce::ComboBox presetCombo { "Presets" };
    std::vector<juce::File> presetFiles;
    std::unique_ptr<juce::FileChooser> chooser;

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    std::unique_ptr<SliderAttachment> dryWetAttachment;
    std::unique_ptr<SliderAttachment> crossfeedAttachment;
    std::unique_ptr<SliderAttachment> gainAttachment;
    std::unique_ptr<ComboBoxAttachment> phaseModeAttachment;
    std::unique_ptr<ButtonAttachment> limiterAttachment;
    std::unique_ptr<ButtonAttachment> bypassAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CalCurveAudioProcessorEditor)
};
