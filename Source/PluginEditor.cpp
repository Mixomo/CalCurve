#include "PluginEditor.h"

namespace
{
    juce::Colour panelColour() { return juce::Colour (0xff12151a); }
    juce::Colour inkColour() { return juce::Colour (0xffeff5f3); }
    juce::Colour accentColour() { return juce::Colour (0xff27d7a4); }

    juce::String helpText()
    {
        return
            "CalCurve loads local headphone calibration files and applies them as convolution correction. It is intentionally narrow: it does not include a headphone database, headphone simulation, or target selection. The loaded file is assumed to already be an exported correction curve or FIR.\n\n"
            "Supported imports\n\n"
            "- WAV FIR impulse responses. WAV files are read as impulse responses, converted to a magnitude curve, then regenerated with the selected phase mode.\n\n"
            "- Frequency/dB TXT or CSV files separated by tabs, spaces, commas, or semicolons.\n\n"
            "- APO / Wavelet GraphicEQ text files.\n\n"
            "- APO CSV correction curves.\n\n"
            "- Melda FreeForm EQ CSV exports when the first two numeric columns are frequency and dB.\n\n"
            "- APO parametric EQ text files with Preamp and Filter lines. Parametric filters are sampled into a correction curve internally.\n\n"
            "- Log-frequency curve exports are inferred when the final frequency value is 5 or lower; frequency is converted with 10^x and small gain values are treated as amplitude/log values.\n\n"
            "Note: the file chooser shows TXT / CSV / FIR. In this build, WAV is the supported impulse-response FIR container. Non-WAV .fir files are treated as text curve files, not raw binary FIR files.\n\n"
            "Controls\n\n"
            "- Dry/Wet: blends the latency-aligned dry signal with the corrected wet signal. At 0% the graph is flat; at 100% it shows the full loaded correction.\n\n"
            "- Crossfeed: stereo headphone crossfeed. It narrows side information and adds a small delayed, low-passed opposite-channel feed for more natural headphone listening.\n\n"
            "- Gain: final output trim in dB, applied after Dry/Wet and Crossfeed.\n\n"
            "- Phase Mode: regenerates the active FIR whenever the mode changes. Minimum uses a 4096-tap minimum-phase FIR and reports 0 samples. Natural uses a 4096-tap mixed-phase FIR, 0.72 minimum-phase weighting, and reports 1024 samples. Linear uses an 8192-tap symmetric linear-phase FIR and reports 4096 samples.\n\n"
            "- Auto Gain (hidden): when a curve or FIR is loaded, CalCurve estimates the K-weighted perceived loudness change and applies a clamped -18 dB to +18 dB compensation to the corrected wet path. The Gain knob is not moved.\n\n"
            "- Limiter: optional safety limiter. It only reduces gain when the output peak exceeds 0 dBFS, uses instant attack, smooth release, and a final hard clip as a last safety stage.\n\n"
            "- Bypass: passes the input through. When a latency-producing phase mode is active, the bypass path is delayed to match the current reported latency.\n\n"
            "- Load TXT / CSV / FIR: opens a local calibration file.\n\n"
            "Graph\n\n"
            "- The colored line is the loaded correction curve scaled by Dry/Wet and flattened when Bypass is active.\n\n"
            "- Frequency labels run from 20 Hz to 20 kHz on a logarithmic X axis.\n\n"
            "- The 0 dB line remains the reference. The graph keeps +6 dB at the top and expands downward in 6 dB steps to fit the loaded curve.\n\n"
            "- The readout below Phase Mode shows the current reported latency and the measured FIR peak index.\n\n"
            "Presets and state\n\n"
            "- The Presets menu can select user presets, save the current preset, and delete the active custom preset.\n\n"
            "- User presets are stored as .calcurvepreset files in the user application data folder under Mixomo/CalCurve/UserPresets.\n\n"
            "- A preset stores parameter values, preset name, loaded file path, and loaded file label. Keep calibration files in a stable folder so presets and DAW sessions can reload them.\n\n"
            "- DAW project state also stores the parameters and loaded file path.\n\n"
            "Build\n\n"
            "- The current CMake project builds the VST3 plugin and CalCurveVST3SmokeTest. StandaloneApp.cpp exists in Source, but it is not part of the current CMake target list.\n\n"
            "- The smoke test can instantiate the VST3, create the editor, process audio, test preset state, inspect WAV FIR files, and validate generated FIR peak positions and magnitude error for TXT/CSV curves.\n\n"
            "Credits\n\n"
            "- Development: Ezequiel Casas (Mixomo)\n\n"
            "https://github.com/Mixomo\n\n"
            "- Example curves:\n\n"
            "https://github.com/Mixomo/My-Headphones-Calibration-Files\n\n"
            "- More calibration curves:\n\n"
            "https://autoeq.app/\n\n"
            "- Thanks and credit to Jaakko Pasanen:\n\n"
            "https://github.com/jaakkopasanen\n\n"
            "- AutoEq:\n\n"
            "https://github.com/jaakkopasanen/AutoEq\n\n"
            "- squig.link:\n\n"
            "https://squig.link/\n\n"
            "- MeldaProduction:\n\n"
            "https://www.meldaproduction.com/\n\n"
            "- Steinberg / VST3 SDK:\n\n"
            "https://github.com/steinbergmedia/vst3sdk\n\n"
            "- JUCE:\n\n"
            "https://juce.com/\n\n"
            "- Microsoft MSVC / Visual Studio C++ toolchain\n\n"
            "https://microsoft.com/\n\n"
            "- C++ language created by Bjarne Stroustrup\n\n"
            "License\n\n"
            "- CalCurve: GNU General Public License v3.0 (GPLv3)\n\n"
            "- VST3 SDK components: MIT License, copyright Steinberg Media Technologies GmbH\n\n"
            "- JUCE framework: AGPLv3/commercial licensing";
    }

    class HelpContent final : public juce::Component
    {
    public:
        HelpContent()
        {
            text.setMultiLine (true);
            text.setReadOnly (true);
            text.setScrollbarsShown (true);
            text.setCaretVisible (false);
            text.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff12151a));
            text.setColour (juce::TextEditor::textColourId, inkColour());
            text.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff38424f));
            text.setFont (juce::FontOptions (18.0f));
            text.setText (helpText(), false);
            addAndMakeVisible (text);
            setSize (820, 680);
        }

        void resized() override
        {
            text.setBounds (getLocalBounds().reduced (18));
        }

    private:
        juce::TextEditor text;
    };

    int curveHash (const std::vector<CurvePoint>& points)
    {
        int hash = static_cast<int> (points.size());
        if (! points.empty())
        {
            hash ^= static_cast<int> (points.front().frequency * 11.0);
            hash ^= static_cast<int> (points.back().frequency * 17.0);
            hash ^= static_cast<int> (points.front().db * 101.0);
            hash ^= static_cast<int> (points.back().db * 131.0);
        }
        return hash;
    }

    juce::String frequencyLabel (double hz)
    {
        if (hz >= 1000.0)
        {
            if (hz >= 10000.0)
                return juce::String (juce::roundToInt (hz / 1000.0)) + "k";

            auto val = juce::String (hz / 1000.0, 1);
            if (val.endsWith (".0"))
                val = val.dropLastCharacters (2);
            return val + "k";
        }

        return juce::String (juce::roundToInt (hz));
    }

    double chooseMinDb (const std::vector<CurvePoint>& points, float scale)
    {
        double minVal = 0.0;

        for (const auto& point : points)
            minVal = juce::jmin (minVal, point.db * static_cast<double> (scale));

        // Add 6.0 dB safety margin so that the curve has breathing room at the bottom
        minVal -= 6.0;

        if (minVal >= -12.0)
            return -12.0;

        return std::floor (minVal / 6.0) * 6.0;
    }

}

CurveDisplay::CurveDisplay (CalCurveAudioProcessor& p)
    : processor (p)
{
}

void CurveDisplay::restartDraw()
{
    drawProgress = 0.0f;
}

juce::Point<float> CurveDisplay::mapPoint (double frequency,
                                           double db,
                                           juce::Rectangle<float> graph,
                                           double minDb,
                                           double maxDb) const
{
    const auto minHz = std::log10 (20.0);
    const auto maxHz = std::log10 (20000.0);
    const auto xNorm = (std::log10 (juce::jlimit (20.0, 20000.0, frequency)) - minHz) / (maxHz - minHz);
    const auto yNorm = juce::jmap (juce::jlimit (minDb, maxDb, db), minDb, maxDb, 1.0, 0.0);

    return { graph.getX() + static_cast<float> (xNorm) * graph.getWidth(),
             graph.getY() + static_cast<float> (yNorm) * graph.getHeight() };
}

juce::Path CurveDisplay::buildPath (const std::vector<CurvePoint>& points,
                                    juce::Rectangle<float> graph,
                                    float scale,
                                    double minDb,
                                    double maxDb) const
{
    juce::Path path;

    if (points.empty())
    {
        const auto y = mapPoint (1000.0, 0.0, graph, minDb, maxDb).y;
        path.startNewSubPath (graph.getX(), y);
        path.lineTo (graph.getRight(), y);
        return path;
    }

    bool started = false;
    for (const auto& point : points)
    {
        const auto mapped = mapPoint (point.frequency, point.db * scale, graph, minDb, maxDb);
        if (! started)
        {
            path.startNewSubPath (mapped);
            started = true;
        }
        else
        {
            path.lineTo (mapped);
        }
    }

    return path;
}

void CurveDisplay::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    auto graph = bounds.reduced (18.0f, 14.0f);
    graph.removeFromBottom (32.0f);
    const auto correction = processor.getCorrectionCurve();
    const auto dryWet = processor.parameters.getRawParameterValue ("drywet")->load();
    const auto minDb = chooseMinDb (correction, dryWet);
    const auto maxDb = 6.0;

    const auto hash = curveHash (correction);
    if (hash != lastCurveHash)
    {
        lastCurveHash = hash;
        restartDraw();
    }

    drawProgress = juce::jmin (1.0f, drawProgress + 0.035f);

    g.setColour (juce::Colour (0xff0c0f13));
    g.fillRoundedRectangle (bounds, 8.0f);

    g.setColour (juce::Colour (0xff252b34));
    for (auto hz : { 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
    {
        const auto x = mapPoint (hz, 0.0, graph, minDb, maxDb).x;
        g.drawVerticalLine (static_cast<int> (std::round (x)), graph.getY(), graph.getBottom());
    }

    const auto dbStep = 6.0;
    for (auto db = minDb; db <= maxDb + 0.001; db += dbStep)
    {
        const auto y = mapPoint (1000.0, db, graph, minDb, maxDb).y;
        g.drawHorizontalLine (static_cast<int> (std::round (y)), graph.getX(), graph.getRight());
    }

    g.setColour (juce::Colour (0xff707b89));
    g.setFont (juce::FontOptions (10.5f));

    for (auto hz : { 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
    {
        const auto x = mapPoint (hz, 0.0, graph, minDb, maxDb).x;
        const auto labelWidth = hz == 20000.0 ? 42.0f : 34.0f;
        g.drawText (frequencyLabel (hz),
                    juce::Rectangle<float> (x - labelWidth * 0.5f, graph.getBottom() + 6.0f, labelWidth, 14.0f),
                    juce::Justification::centred);
    }

    for (auto db = minDb; db <= maxDb + 0.001; db += dbStep)
    {
        const auto y = mapPoint (1000.0, db, graph, minDb, maxDb).y;
        g.drawText (juce::String (juce::roundToInt (db)) + " dB",
                    juce::Rectangle<float> (graph.getX() + 4.0f, y - 7.0f, 48.0f, 14.0f),
                    juce::Justification::centredLeft);
    }

    g.setColour (juce::Colour (0xff596270));
    g.drawRect (graph, 1.0f);

    const auto bypass = processor.parameters.getRawParameterValue ("bypass")->load() > 0.5f;
    const auto scale = bypass ? 0.0f : dryWet;
    juce::Path correctionPath = buildPath (correction, graph, scale, minDb, maxDb);

    g.saveState();
    g.reduceClipRegion (graph.withWidth (graph.getWidth() * drawProgress).toNearestInt());
    juce::PathStrokeType correctionStroke (2.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    g.setColour (accentColour());
    g.strokePath (correctionPath, correctionStroke);
    g.restoreState();

}

CalCurveAudioProcessorEditor::CalCurveAudioProcessorEditor (CalCurveAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p), display (p)
{
    setSize (840, 740);
    setResizable (true, true);
    getConstrainer()->setMinimumWidth (640);
    getConstrainer()->setMinimumHeight (540);
    getConstrainer()->setMaximumWidth (1600);
    getConstrainer()->setMaximumHeight (1200);

    title.setText ("CalCurve", juce::dontSendNotification);
    title.setFont (juce::FontOptions (25.0f, juce::Font::bold));
    title.setColour (juce::Label::textColourId, inkColour());
    addAndMakeVisible (title);

    loadedFile.setText (processor.getLoadedName(), juce::dontSendNotification);
    loadedFile.setJustificationType (juce::Justification::centredLeft);
    loadedFile.setColour (juce::Label::textColourId, juce::Colour (0xffaeb8c5));
    addAndMakeVisible (loadedFile);

    addAndMakeVisible (display);

    styleSlider (dryWet, "");
    styleSlider (crossfeed, "");
    styleSlider (gain, " dB");

    dryWet.setName ("Dry/Wet");
    crossfeed.setName ("Crossfeed");
    gain.setName ("Gain");

    limiter.setButtonText ("Limiter");
    limiter.setColour (juce::ToggleButton::textColourId, inkColour());
    addAndMakeVisible (limiter);

    bypassButton.setButtonText ("Bypass");
    bypassButton.setColour (juce::ToggleButton::textColourId, inkColour());
    addAndMakeVisible (bypassButton);

    auto setupLabel = [this] (juce::Label& label, const juce::String& text)
    {
        label.setText (text, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setColour (juce::Label::textColourId, juce::Colour (0xffdbe4ef));
        label.setFont (juce::FontOptions (13.0f));
        label.setMinimumHorizontalScale (0.82f);
        addAndMakeVisible (label);
    };

    setupLabel (dryWetLabel, "Dry/Wet");
    setupLabel (crossfeedLabel, "Crossfeed");
    setupLabel (gainLabel, "Gain");
    setupLabel (phaseModeLabel, "Phase Mode");

    latencyLabel.setJustificationType (juce::Justification::centredLeft);
    latencyLabel.setColour (juce::Label::textColourId, juce::Colour (0xffaeb8c5));
    latencyLabel.setFont (juce::FontOptions (12.5f));
    latencyLabel.setMinimumHorizontalScale (0.78f);
    addAndMakeVisible (latencyLabel);

    phaseMode.addItem ("Minimum", 1);
    phaseMode.addItem ("Natural", 2);
    phaseMode.addItem ("Linear", 3);
    phaseMode.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff1f2630));
    phaseMode.setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff4a5664));
    phaseMode.setColour (juce::ComboBox::textColourId, inkColour());
    phaseMode.setColour (juce::ComboBox::arrowColourId, accentColour());
    addAndMakeVisible (phaseMode);

    loadFile.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f2630));
    loadFile.setColour (juce::TextButton::buttonOnColourId, accentColour().darker());
    loadFile.setColour (juce::TextButton::textColourOffId, inkColour());
    addAndMakeVisible (loadFile);

    loadFile.onClick = [this] { openFileChooser(); };
    help.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f2630));
    help.setColour (juce::TextButton::textColourOffId, inkColour());
    help.onClick = [this] { showHelp(); };
    addAndMakeVisible (help);

    presetCombo.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff1f2630));
    presetCombo.setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff4a5664));
    presetCombo.setColour (juce::ComboBox::textColourId, inkColour());
    presetCombo.setColour (juce::ComboBox::arrowColourId, accentColour());
    addAndMakeVisible (presetCombo);
    presetCombo.onChange = [this] { handlePresetComboChange(); };
    updatePresetCombo();


    dryWetAttachment = std::make_unique<SliderAttachment> (processor.parameters, "drywet", dryWet);
    crossfeedAttachment = std::make_unique<SliderAttachment> (processor.parameters, "crossfeed", crossfeed);
    gainAttachment = std::make_unique<SliderAttachment> (processor.parameters, "gain", gain);
    phaseModeAttachment = std::make_unique<ComboBoxAttachment> (processor.parameters, "phasemode", phaseMode);
    limiterAttachment = std::make_unique<ButtonAttachment> (processor.parameters, "limiter", limiter);
    bypassAttachment = std::make_unique<ButtonAttachment> (processor.parameters, "bypass", bypassButton);

    startTimerHz (30);
}

void CalCurveAudioProcessorEditor::styleSlider (juce::Slider& slider, const juce::String& suffix)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 98, 26);
    slider.setTextValueSuffix (suffix);
    slider.setColour (juce::Slider::thumbColourId, juce::Colour (0xffe9f6f1));
    slider.setColour (juce::Slider::rotarySliderFillColourId, accentColour().brighter (0.25f));
    slider.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff465360));
    slider.setColour (juce::Slider::textBoxTextColourId, inkColour());
    slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    addAndMakeVisible (slider);
}

void CalCurveAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (panelColour());

    auto header = getLocalBounds().removeFromTop (86).toFloat();
    juce::ColourGradient glow (juce::Colour (0xff16251f), header.getX(), header.getY(),
                               juce::Colour (0xff111820), header.getRight(), header.getBottom(), false);
    g.setGradientFill (glow);
    g.fillRect (header);

    g.setColour (juce::Colour (0xff27313b));
    g.drawHorizontalLine (85, 24.0f, static_cast<float> (getWidth() - 24));
}

void CalCurveAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (24);
    auto header = area.removeFromTop (48);
    title.setBounds (header.removeFromLeft (140));
    header.removeFromLeft (12);
    presetCombo.setBounds (header.removeFromLeft (160).withHeight (36).translated (0, 2));

    help.setBounds (header.removeFromRight (50).withHeight (36).translated (0, 2));
    header.removeFromRight (8);
    loadFile.setBounds (header.removeFromRight (180).withHeight (36).translated (0, 2));
    loadedFile.setBounds (area.removeFromTop (24));

    // Allocate bottom controls area from the bottom of 'area'
    auto controls = area.removeFromBottom (176);

    // Remaining 'area' in the middle belongs entirely to the graph!
    area.removeFromTop (14);
    area.removeFromBottom (14);
    display.setBounds (area);

    // Position bottom controls reactively
    auto dryWetArea = controls.removeFromLeft (170);
    controls.removeFromLeft (24);
    auto crossfeedArea = controls.removeFromLeft (170);
    controls.removeFromLeft (24);
    auto gainArea = controls.removeFromLeft (170);

    auto placeControl = [] (juce::Rectangle<int> area, juce::Slider& slider, juce::Label& label)
    {
        slider.setBounds (area.removeFromTop (108));
        area.removeFromTop (8);
        label.setBounds (area.removeFromTop (28));
    };

    placeControl (dryWetArea, dryWet, dryWetLabel);
    placeControl (crossfeedArea, crossfeed, crossfeedLabel);
    placeControl (gainArea, gain, gainLabel);

    controls.removeFromLeft (32);
    auto phaseArea = controls.removeFromTop (96).withWidth (190).translated (0, 8);
    phaseModeLabel.setBounds (phaseArea.removeFromTop (24));
    phaseMode.setBounds (phaseArea.removeFromTop (32));
    phaseArea.removeFromTop (6);
    latencyLabel.setBounds (phaseArea.removeFromTop (26));
    controls.removeFromTop (8);
    limiter.setBounds (controls.removeFromTop (32).withWidth (160).translated (0, 12));
    controls.removeFromTop (8);
    bypassButton.setBounds (controls.removeFromTop (32).withWidth (160).translated (0, 12));
}

void CalCurveAudioProcessorEditor::timerCallback()
{
    loadedFile.setText (processor.getLoadedName(), juce::dontSendNotification);
    const auto samples = processor.getActiveLatencySamples();
    const auto sampleRate = processor.getCurrentSampleRate() > 1.0 ? processor.getCurrentSampleRate() : 48000.0;
    const auto ms = 1000.0 * static_cast<double> (samples) / sampleRate;
    latencyLabel.setText ("Latency: " + juce::String (samples) + " samples / " + juce::String (ms, 1) + " ms"
                          + " | IR peak: " + juce::String (processor.getActiveImpulsePeakSamples()),
                          juce::dontSendNotification);
    display.repaint();
}

void CalCurveAudioProcessorEditor::openFileChooser()
{
    chooser = std::make_unique<juce::FileChooser> ("Load TXT / CSV correction curve or FIR WAV",
                                                   juce::File{},
                                                   "*.txt;*.csv;*.wav;*.fir");
    const juce::Component::SafePointer<CalCurveAudioProcessorEditor> safeThis (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safeThis] (const juce::FileChooser& fc)
        {
            if (safeThis == nullptr)
                return;

            auto file = fc.getResult();
            if (file.existsAsFile())
            {
                if (file.hasFileExtension ("wav"))
                    safeThis->processor.loadImpulseResponse (file);
                else
                    safeThis->processor.loadCorrectionCurve (file);
            }
        });
}



void CalCurveAudioProcessorEditor::showHelp()
{
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (new HelpContent());
    options.dialogTitle = "CalCurve Help";
    options.dialogBackgroundColour = panelColour();
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    options.launchAsync();
}

void CalCurveAudioProcessorEditor::updatePresetCombo()
{
    presetCombo.clear (juce::dontSendNotification);
    presetFiles.clear();

    presetCombo.addItem ("Select user preset...", 1);
    presetCombo.addSeparator();

    auto dir = processor.getPresetsDirectory();
    auto files = dir.findChildFiles (juce::File::TypesOfFileToFind::findFiles, false, "*.calcurvepreset;*.xml");
    
    int itemID = 3;
    for (const auto& file : files)
    {
        presetCombo.addItem (file.getFileNameWithoutExtension(), itemID);
        presetFiles.push_back (file);
        
        if (processor.getPresetName().trim().equalsIgnoreCase (file.getFileNameWithoutExtension().trim()))
            presetCombo.setSelectedId (itemID, juce::dontSendNotification);

        ++itemID;
    }

    presetCombo.addSeparator();
    presetCombo.addItem ("Save current preset...", 9999);
    presetCombo.addItem ("Delete current preset...", 10000);
    
    if (presetCombo.getSelectedId() == 0)
        presetCombo.setSelectedId (1, juce::dontSendNotification);
}

void CalCurveAudioProcessorEditor::handlePresetComboChange()
{
    const int selectedId = presetCombo.getSelectedId();

    if (selectedId == 1)
    {
        return;
    }
    else if (selectedId == 9999)
    {
        presetCombo.setSelectedId (1, juce::dontSendNotification);

        auto* w = new juce::AlertWindow ("Save Preset", "Enter a name for the new preset:", juce::AlertWindow::QuestionIcon);
        w->addTextEditor ("presetName", "My Preset", "Preset Name:");
        w->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey, 0, 0));
        w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey, 0, 0));
        
        w->enterModalState (true, juce::ModalCallbackFunction::create ([this, w] (int result)
        {
            std::unique_ptr<juce::AlertWindow> windowToDelete (w);
            if (result == 1)
            {
                auto name = w->getTextEditorContents ("presetName").trim();
                if (name.isNotEmpty())
                {
                    auto safeName = name.replaceCharacters ("\\/:*?\"<>|", "_________");
                    auto file = processor.getPresetsDirectory().getChildFile (safeName + ".calcurvepreset");
                    
                    if (processor.savePresetToFile (file, safeName))
                    {
                        updatePresetCombo();
                    }
                }
            }
        }), true);
    }
    else if (selectedId == 10000)
    {
        presetCombo.setSelectedId (1, juce::dontSendNotification);

        auto activeName = processor.getPresetName().trim();
        if (activeName.isEmpty() || activeName == "Untitled")
        {
            juce::AlertWindow::showMessageBoxAsync (
                juce::AlertWindow::WarningIcon,
                "Delete Preset",
                "No custom preset is currently loaded."
            );
            return;
        }

        auto file = processor.getPresetsDirectory().getChildFile (activeName + ".calcurvepreset");
        if (file.existsAsFile())
        {
            juce::AlertWindow::showOkCancelBox (
                juce::AlertWindow::QuestionIcon,
                "Delete Preset",
                "Are you sure you want to delete the preset \"" + activeName + "\"",
                "Yes",
                "No",
                nullptr,
                juce::ModalCallbackFunction::create ([this, file] (int result)
                {
                    if (result != 0)
                    {
                        const_cast<juce::File&>(file).deleteFile();
                        updatePresetCombo();
                    }
                })
            );
        }
    }
    else
    {
        const int index = selectedId - 3;
        if (index >= 0 && index < static_cast<int> (presetFiles.size()))
        {
            const auto& file = presetFiles[static_cast<size_t> (index)];
            if (file.existsAsFile())
            {
                if (processor.loadPresetFromFile (file))
                {
                    updatePresetCombo();
                    display.repaint();
                }
            }
        }
    }
}
