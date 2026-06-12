#pragma once

#include <JuceHeader.h>
#include "FlexCurveProcessor.h"

class FlexCurveResettableSlider : public juce::Slider
{
public:
    void setResetValue (double value)
    {
        resetValue = value;
        setDoubleClickReturnValue (true, value);
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        if (event.mods.isRightButtonDown())
        {
            setValue (resetValue, juce::sendNotificationSync);
            return;
        }

        juce::Slider::mouseDown (event);
    }

private:
    double resetValue = 0.0;
};

class FlexCurveSelectableSlider final : public FlexCurveResettableSlider
{
public:
    std::function<void(bool, bool)> onSelect;
    std::function<void()> onResetSelection;
    bool selected = false;

    void mouseDown (const juce::MouseEvent& event) override
    {
        if (event.getNumberOfClicks() > 1)
        {
            if (onSelect != nullptr)
                onSelect (false, false);
            if (onResetSelection != nullptr)
                onResetSelection();
            return;
        }

        if (onSelect != nullptr)
            onSelect (event.mods.isCtrlDown(), event.mods.isShiftDown());
        if (event.mods.isCtrlDown() || event.mods.isShiftDown())
            return;
        FlexCurveResettableSlider::mouseDown (event);
    }

    void mouseDoubleClick (const juce::MouseEvent&) override {}

    void paint (juce::Graphics& g) override
    {
        FlexCurveResettableSlider::paint (g);
        if (selected)
        {
            g.setColour (juce::Colour (0xff5ea1ff));
            g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 4.0f, 1.5f);
        }
    }
};

class FlexCurveLockButton final : public juce::Button
{
public:
    FlexCurveLockButton() : juce::Button ("Lock editing")
    {
        setClickingTogglesState (true);
        setTooltip ("Lock or unlock correction editing");
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        auto bounds = getLocalBounds().toFloat().reduced (2.0f);
        auto background = getToggleState() ? juce::Colour (0xff247a68) : juce::Colour (0xff202832);
        if (highlighted) background = background.brighter (0.08f);
        if (down) background = background.darker (0.10f);
        g.setColour (background);
        g.fillRoundedRectangle (bounds, 5.0f);
        g.setColour (juce::Colour (0xffeff5f3));
        g.drawRoundedRectangle (bounds, 5.0f, 1.0f);

        auto icon = bounds.withSizeKeepingCentre (18.0f, 20.0f);
        auto body = icon.removeFromBottom (11.0f);
        g.fillRoundedRectangle (body, 2.0f);
        juce::Path shackle;
        const auto left = icon.getX() + (getToggleState() ? 3.0f : 7.0f);
        shackle.startNewSubPath (left, body.getY() + 1.0f);
        shackle.cubicTo (left, icon.getY() - 2.0f, icon.getRight() - 3.0f, icon.getY() - 2.0f,
                         icon.getRight() - 3.0f, body.getY() + 1.0f);
        g.strokePath (shackle, juce::PathStrokeType (2.2f));
    }
};

class FlexCurveHistoryButton final : public juce::Button
{
public:
    explicit FlexCurveHistoryButton (bool pointsLeft)
        : juce::Button (pointsLeft ? "Undo" : "Redo"), left (pointsLeft)
    {}

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        auto bounds = getLocalBounds().toFloat().reduced (2.0f);
        auto background = isEnabled() ? juce::Colour (0xff202832) : juce::Colour (0xff171c22);
        if (highlighted && isEnabled()) background = background.brighter (0.08f);
        if (down && isEnabled()) background = background.darker (0.10f);
        g.setColour (background);
        g.fillRoundedRectangle (bounds, 5.0f);
        g.setColour (isEnabled() ? juce::Colour (0xffeff5f3) : juce::Colour (0xff59636d));
        g.drawRoundedRectangle (bounds, 5.0f, 1.0f);

        juce::Path arrow;
        const auto cx = bounds.getCentreX();
        const auto cy = bounds.getCentreY();
        if (left)
        {
            arrow.startNewSubPath (cx + 7.0f, cy + 5.0f);
            arrow.cubicTo (cx + 7.0f, cy - 5.0f, cx - 3.0f, cy - 6.0f, cx - 7.0f, cy);
            arrow.lineTo (cx - 2.0f, cy - 5.0f);
            arrow.startNewSubPath (cx - 7.0f, cy);
            arrow.lineTo (cx - 1.0f, cy + 3.0f);
        }
        else
        {
            arrow.startNewSubPath (cx - 7.0f, cy + 5.0f);
            arrow.cubicTo (cx - 7.0f, cy - 5.0f, cx + 3.0f, cy - 6.0f, cx + 7.0f, cy);
            arrow.lineTo (cx + 2.0f, cy - 5.0f);
            arrow.startNewSubPath (cx + 7.0f, cy);
            arrow.lineTo (cx + 1.0f, cy + 3.0f);
        }
        g.strokePath (arrow, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

private:
    bool left;
};

class FlexCurveGraph final : public juce::Component
{
public:
    explicit FlexCurveGraph (FlexCurveAudioProcessor&);
    void paint (juce::Graphics&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    std::vector<int> getSelectedFreeformIndices() const;
    void selectFreeformPoint (int index, bool additive);
    void setVariableEditingAllowed (bool allowed);
    void zoomIn();
    void zoomOut();
    void resetZoom();
    void setViewRange (double halfRangeDb);

private:
    double getMinDb() const;
    double getMaxDb() const;
    int getLegendRowCount() const;
    juce::Rectangle<float> getPlotBounds() const;
    juce::Point<float> mapPoint (double frequency, double db, juce::Rectangle<float> graph, double minDb, double maxDb) const;
    CurvePoint unmapPoint (juce::Point<float> point, juce::Rectangle<float> graph, double minDb, double maxDb) const;
    juce::Path buildPath (const std::vector<CurvePoint>& points, juce::Rectangle<float> graph, double minDb, double maxDb) const;
    int findFreeformPointNear (juce::Point<float> point, juce::Rectangle<float> graph, double minDb, double maxDb) const;
    bool isVariableEditingActive() const;
    bool isSelected (int index) const;
    void deleteSelectedPoints();
    void setSingleSelection (int index);
    void toggleSelection (int index);
    void paintFreeformAt (juce::Point<float> position);

    FlexCurveAudioProcessor& processor;
    int draggingFreeformIndex = -1;
    int selectedFreeformIndex = -1;
    std::vector<int> selectedFreeformIndices;
    bool marqueeSelecting = false;
    juce::Point<float> marqueeStart;
    juce::Rectangle<float> marqueeBounds;
    CurvePoint lastDragPoint {};
    bool paintingFreeform = false;
    juce::Point<float> lastPaintPosition { -1000.0f, -1000.0f };
    float paintDistanceToNextPoint = 6.0f;
    std::vector<std::pair<juce::Rectangle<float>, int>> legendHitBoxes;
    bool variableEditingAllowed = false;
    double viewZoom = 1.0;
    double viewCentreDb = 0.0;
    double viewHalfRangeDb = 12.0;
};

class FlexCurveAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                            private juce::Timer
{
public:
    explicit FlexCurveAudioProcessorEditor (FlexCurveAudioProcessor&);
    ~FlexCurveAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class BlendTab;
    class GraphicTab;
    class ParametricTab;
    class GlobalLayerRack;
    class MeterPanel;
    class GraphResizeHandle;

    void timerCallback() override;
    void styleSlider (juce::Slider&, const juce::String& suffix = {});
    void openAddCurveChooser();
    void openExportFirChooser();
    void showHelp();
    void updateActiveLayerCombo();
    void updatePresetCombo();
    void handlePresetComboChange();
    void updateAddButtons();
    void moveUiIntoScaledContent();
    void applyUiScalePreset (float scale);

    FlexCurveAudioProcessor& processor;
    juce::Component scaledContent;
    FlexCurveGraph graph;
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
    std::unique_ptr<BlendTab> blendTab;
    std::unique_ptr<GraphicTab> graphicTab;
    std::unique_ptr<ParametricTab> parametricTab;
    std::unique_ptr<GlobalLayerRack> globalLayerRack;
    std::unique_ptr<MeterPanel> meterPanel;
    std::unique_ptr<GraphResizeHandle> graphResizeHandle;

    juce::Label title;
    juce::Label status;
    juce::ComboBox presetCombo { "Presets" };
    juce::Label activeLayerLabel;
    juce::TextButton addCurve { "Import Curve as a New Layer" };
    juce::TextButton newFlat { "Add Curve as a New Layer" };
    juce::TextButton renderFir { "Render FIR" };
    juce::TextButton exportFir { "Export FIR" };
    juce::TextButton resetAll { "RESET ALL" };
    juce::TextButton resetFlat { "Reset to Flat Curves" };
    juce::TextButton help { "Help" };
    juce::TextButton zoomIn { "+" };
    juce::TextButton zoomOut { "-" };
    juce::TextButton zoomReset { "1:1" };
    juce::Label dbScaleLabel;
    juce::ComboBox dbScale;
    juce::Label uiScaleLabel;
    juce::ComboBox uiScale;
    juce::Label dryWetLabel;
    juce::Label crossfeedLabel;
    juce::Label gainLabel;
    juce::Label inputGainLabel;
    juce::Label outputGainLabel;
    juce::Label phaseModeLabel;
    juce::Label globalControlsLabel;
    FlexCurveResettableSlider dryWet;
    FlexCurveResettableSlider crossfeed;
    FlexCurveResettableSlider gain;
    FlexCurveResettableSlider inputGain;
    FlexCurveResettableSlider outputGain;
    juce::ComboBox phaseMode;
    juce::ToggleButton autoGain { "Auto Gain" };
    juce::ComboBox loudnessMatchMode;
    juce::ToggleButton includeOutputGainFir { "Include Output Gain in FIR export" };
    juce::ToggleButton includeAutoGainFir { "Include Auto Gain in FIR export" };
    juce::TextButton resetMeters { "Reset Meters" };
    FlexCurveLockButton lockMode;
    FlexCurveHistoryButton undoButton { true };
    FlexCurveHistoryButton redoButton { false };
    juce::TextButton smoothAll { "Smooth All Curves" };
    juce::ToggleButton limiter { "Limiter" };
    juce::ToggleButton bypass { "Bypass" };
    juce::TooltipWindow tooltipWindow { this, 500 };
    std::vector<juce::File> presetFiles;
    std::unique_ptr<juce::FileChooser> chooser;
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    std::unique_ptr<SliderAttachment> dryWetAttachment;
    std::unique_ptr<SliderAttachment> crossfeedAttachment;
    std::unique_ptr<SliderAttachment> gainAttachment;
    std::unique_ptr<SliderAttachment> inputGainAttachment;
    std::unique_ptr<SliderAttachment> outputGainAttachment;
    std::unique_ptr<ComboBoxAttachment> phaseModeAttachment;
    std::unique_ptr<ButtonAttachment> limiterAttachment;
    std::unique_ptr<ButtonAttachment> bypassAttachment;
    std::unique_ptr<ButtonAttachment> autoGainAttachment;
    std::unique_ptr<ComboBoxAttachment> loudnessMatchAttachment;
    std::unique_ptr<ButtonAttachment> includeOutputGainFirAttachment;
    std::unique_ptr<ButtonAttachment> includeAutoGainFirAttachment;
    static constexpr int designWidth = 1680;
    static constexpr int designHeight = 1040;
    int graphSectionHeight = 430;
    int globalControlsWidth = 560;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FlexCurveAudioProcessorEditor)
};
