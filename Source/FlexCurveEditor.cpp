#include "FlexCurveEditor.h"
#include <limits>
#include <numeric>

namespace
{
    juce::Colour panelColour() { return juce::Colour (0xff101419); }
    juce::Colour surfaceColour() { return juce::Colour (0xff151b22); }
    juce::Colour inkColour() { return juce::Colour (0xffeff5f3); }
    juce::Colour mutedColour() { return juce::Colour (0xff9eabb8); }
    juce::Colour accentColour() { return juce::Colour (0xff27d7a4); }
    juce::Colour scrollThumbColour() { return accentColour().darker (0.42f).withAlpha (0.85f); }
    juce::Colour scrollTrackColour() { return juce::Colour (0xff111820); }

    void styleScrollBar (juce::ScrollBar& scrollBar)
    {
        scrollBar.setColour (juce::ScrollBar::thumbColourId, scrollThumbColour());
        scrollBar.setColour (juce::ScrollBar::trackColourId, scrollTrackColour());
        scrollBar.setColour (juce::ScrollBar::backgroundColourId, juce::Colours::transparentBlack);
    }

    juce::String frequencyLabel (double hz)
    {
        if (hz >= 1000.0)
        {
            const auto khz = hz / 1000.0;
            const auto wholeKhz = std::round (khz);
            return (std::abs (khz - wholeKhz) < 0.001
                        ? juce::String (static_cast<int> (wholeKhz))
                        : juce::String (khz, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd ("."))
                 + "k";
        }
        return juce::String (juce::roundToInt (hz));
    }

    bool curvesDiffer (const std::vector<CurvePoint>& left,
                       const std::vector<CurvePoint>& right,
                       double toleranceDb = 1.0e-4)
    {
        if (left.size() != right.size())
            return true;
        for (size_t i = 0; i < left.size(); ++i)
            if (std::abs (left[i].db - right[i].db) > toleranceDb)
                return true;
        return false;
    }

    float displayBalanceOffsetDb (FlexCurveAudioProcessor& processor, FlexChannelSelection channel)
    {
        const auto balanceDb = processor.parameters.getRawParameterValue ("globalbalance")->load();
        const auto limited = juce::jlimit (-24.0f, 24.0f, balanceDb);
        return channel == FlexChannelSelection::right ? limited * 0.5f : -limited * 0.5f;
    }

    float balanceDbToPanPercent (float balanceDb)
    {
        return juce::jlimit (-100.0f, 100.0f, balanceDb * (100.0f / 24.0f));
    }

    float panPercentToBalanceDb (float panPercent)
    {
        return juce::jlimit (-24.0f, 24.0f, panPercent * (24.0f / 100.0f));
    }

    void applyDisplayAudioTransform (FlexCurveAudioProcessor& processor,
                                     std::vector<CurvePoint>& points,
                                     FlexChannelSelection channel)
    {
        const auto dryWet = processor.parameters.getRawParameterValue ("drywet")->load();
        const auto gainDb = processor.parameters.getRawParameterValue ("gain")->load();
        const auto bypassed = processor.parameters.getRawParameterValue ("bypass")->load() > 0.5f;
        const auto balanceOffset = displayBalanceOffsetDb (processor, channel);
        for (auto& point : points)
            point.db = bypassed ? 0.0 : point.db * dryWet + gainDb + balanceOffset;
    }

    std::vector<CurvePoint> graphScalePointsFor (FlexCurveAudioProcessor& processor)
    {
        auto displayAudioCurve = [&processor] (std::vector<CurvePoint> points,
                                                FlexChannelSelection channel)
        {
            applyDisplayAudioTransform (processor, points, channel);
            return points;
        };

        auto points = displayAudioCurve (processor.getFinalCurve(), FlexChannelSelection::left);
        const auto layers = processor.getLayers();
        for (const auto& layer : layers)
        {
            if (! layer.visible)
                continue;
            if (processor.isLayerTypeVisible (layer.type))
            {
                const auto curve = layer.type == FlexCurveLayerType::eq
                    ? displayAudioCurve (processor.getLayerCurve (layer.id), FlexChannelSelection::left)
                    : processor.getLayerCurveForDisplay (layer.id);
                points.insert (points.end(), curve.begin(), curve.end());
                if (! layer.channelsLinked)
                {
                    const auto rightCurve = layer.type == FlexCurveLayerType::eq
                        ? displayAudioCurve (processor.getLayerCurve (
                            layer.id, FlexChannelSelection::right), FlexChannelSelection::right)
                        : processor.getLayerCurveForDisplay (
                            layer.id, FlexChannelSelection::right);
                    points.insert (points.end(), rightCurve.begin(), rightCurve.end());
                }
            }
            if (processor.areCorrectedMeasurementsVisible()
                && layer.type == FlexCurveLayerType::eq && layer.autoEqRawLayerId > 0)
            {
                const auto corrected = processor.getCorrectedMeasurementCurve (layer.id);
                points.insert (points.end(), corrected.begin(), corrected.end());
                if (! layer.channelsLinked)
                {
                    const auto correctedRight = processor.getCorrectedMeasurementCurve (
                        layer.id, FlexChannelSelection::right);
                    points.insert (points.end(), correctedRight.begin(), correctedRight.end());
                }
            }
        }
        if (processor.isAverageVisible())
        {
            const auto average = displayAudioCurve (processor.getAverageCurve(), FlexChannelSelection::left);
            points.insert (points.end(), average.begin(), average.end());
            if (processor.averageChannelsDiffer()
                || std::abs (processor.parameters.getRawParameterValue ("globalbalance")->load()) > 0.001f)
            {
                const auto rightAverage = displayAudioCurve (
                    processor.getAverageCurve (FlexChannelSelection::right),
                    FlexChannelSelection::right);
                points.insert (points.end(), rightAverage.begin(), rightAverage.end());
            }
        }
        for (const auto type : { FlexCurveLayerType::target, FlexCurveLayerType::raw })
        {
            const auto count = std::count_if (layers.begin(), layers.end(),
                                              [&processor, type] (const auto& layer)
                                              {
                                                  return layer.type == type && layer.visible
                                                      && processor.isLayerTypeVisible (type);
            });
            if (count >= 2)
            {
                const auto averageLeft = processor.getAverageCurveForTypeForDisplay (
                    type, FlexChannelSelection::left);
                const auto averageRight = processor.getAverageCurveForTypeForDisplay (
                    type, FlexChannelSelection::right);
                points.insert (points.end(), averageLeft.begin(), averageLeft.end());
                if (curvesDiffer (averageLeft, averageRight))
                    points.insert (points.end(), averageRight.begin(), averageRight.end());
            }
        }
        return points;
    }

    std::vector<CurvePoint> graphDisplayAudioCurveFor (FlexCurveAudioProcessor& processor,
                                                        std::vector<CurvePoint> points,
                                                        FlexChannelSelection channel = FlexChannelSelection::left)
    {
        applyDisplayAudioTransform (processor, points, channel);
        return points;
    }

    void styleButton (juce::TextButton& button)
    {
        button.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff202832));
        button.setColour (juce::TextButton::buttonOnColourId, accentColour().darker());
        button.setColour (juce::TextButton::textColourOffId, inkColour());
        button.setColour (juce::TextButton::textColourOnId, inkColour());
    }

    void styleCombo (juce::ComboBox& combo)
    {
        combo.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff202832));
        combo.setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff4a5664));
        combo.setColour (juce::ComboBox::textColourId, inkColour());
        combo.setColour (juce::ComboBox::arrowColourId, accentColour());
    }

    void installSliderReset (FlexCurveResettableSlider& slider, double defaultValue)
    {
        slider.setResetValue (defaultValue);
    }

    void styleToggle (juce::ToggleButton& button)
    {
        button.setColour (juce::ToggleButton::textColourId, inkColour());
        button.setColour (juce::ToggleButton::tickColourId, accentColour());
    }

    class CrossfeedAdvancedComponent final : public juce::Component
    {
    public:
        explicit CrossfeedAdvancedComponent (FlexCurveAudioProcessor& p) : processor (p)
        {
            algorithm.addItem ("Natural", 1);
            algorithm.addItem ("BS2B / RME style", 2);
            styleCombo (algorithm);
            addAndMakeVisible (algorithm);

            preset.addItem ("Average Male", 1);
            preset.addItem ("Average Female", 2);
            preset.addItem ("Wide / relaxed", 3);
            preset.addItem ("Narrow / strong", 4);
            preset.setTextWhenNothingSelected ("Geometry preset...");
            styleCombo (preset);
            preset.onChange = [this] { applyPreset (preset.getSelectedId()); };
            addAndMakeVisible (preset);

            addSlider (circumference, " cm", "Head Circumference");
            addSlider (headWidth, " cm", "Head Width");
            addSlider (headLength, " cm", "Head Length");
            addSlider (angle, juce::String::fromUTF8 (" \xc2\xb0"), "Speaker Angle");
            addSlider (cutoff, " Hz", "Crossfeed Cutoff");
            addSlider (direct, " %", "Direct Level");

            reset.setButtonText ("Reset");
            styleButton (reset);
            reset.onClick = [this] { resetDefaults(); };
            addAndMakeVisible (reset);

            algorithmAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
                processor.parameters, "crossfeedalgorithm", algorithm);
            circumferenceAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
                processor.parameters, "crossfeedcircumference", circumference);
            widthAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
                processor.parameters, "crossfeedheadwidth", headWidth);
            lengthAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
                processor.parameters, "crossfeedheadlength", headLength);
            angleAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
                processor.parameters, "crossfeedangle", angle);
            cutoffAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
                processor.parameters, "crossfeedcutoff", cutoff);
            directAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
                processor.parameters, "crossfeeddirect", direct);

            setSize (420, 330);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (panelColour());
            g.setColour (inkColour());
            g.setFont (juce::FontOptions (18.0f, juce::Font::bold));
            g.drawText ("Advanced Crossfeed", getLocalBounds().removeFromTop (38).reduced (14, 0),
                        juce::Justification::centredLeft);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (16, 44);
            auto top = area.removeFromTop (30);
            algorithm.setBounds (top.removeFromLeft (180));
            top.removeFromLeft (10);
            preset.setBounds (top.removeFromLeft (190));
            area.removeFromTop (10);

            auto place = [&area] (juce::Label& label, juce::Slider& slider)
            {
                auto row = area.removeFromTop (36);
                label.setBounds (row.removeFromLeft (142));
                slider.setBounds (row);
                area.removeFromTop (5);
            };

            place (circumferenceLabel, circumference);
            place (headWidthLabel, headWidth);
            place (headLengthLabel, headLength);
            place (angleLabel, angle);
            place (cutoffLabel, cutoff);
            place (directLabel, direct);
            reset.setBounds (area.removeFromTop (32).removeFromRight (96));
        }

    private:
        void addSlider (juce::Slider& slider, const juce::String& suffix, const juce::String& labelText)
        {
            slider.setSliderStyle (juce::Slider::LinearHorizontal);
            slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 74, 22);
            slider.setTextValueSuffix (suffix);
            slider.setColour (juce::Slider::thumbColourId, accentColour());
            slider.setColour (juce::Slider::trackColourId, juce::Colour (0xff33404c));
            slider.setColour (juce::Slider::textBoxTextColourId, inkColour());
            slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
            addAndMakeVisible (slider);

            auto* label = labelFor (slider);
            label->setText (labelText, juce::dontSendNotification);
            label->setColour (juce::Label::textColourId, inkColour());
            label->setJustificationType (juce::Justification::centredLeft);
            addAndMakeVisible (*label);
        }

        juce::Label* labelFor (juce::Slider& slider)
        {
            if (&slider == &circumference) return &circumferenceLabel;
            if (&slider == &headWidth) return &headWidthLabel;
            if (&slider == &headLength) return &headLengthLabel;
            if (&slider == &angle) return &angleLabel;
            if (&slider == &cutoff) return &cutoffLabel;
            return &directLabel;
        }

        void setChoice (const juce::String& id, int index)
        {
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (processor.parameters.getParameter (id)))
                ranged->setValueNotifyingHost (ranged->convertTo0to1 (static_cast<float> (index)));
        }

        void setFloat (const juce::String& id, float value)
        {
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (processor.parameters.getParameter (id)))
                ranged->setValueNotifyingHost (ranged->convertTo0to1 (value));
        }

        void applyGeometry (float circumferenceCm, float widthCm, float lengthCm,
                            float speakerAngle, float cutoffHz, float directPercent)
        {
            setFloat ("crossfeedcircumference", circumferenceCm);
            setFloat ("crossfeedheadwidth", widthCm);
            setFloat ("crossfeedheadlength", lengthCm);
            setFloat ("crossfeedangle", speakerAngle);
            setFloat ("crossfeedcutoff", cutoffHz);
            setFloat ("crossfeeddirect", directPercent);
        }

        void applyPreset (int id)
        {
            if (id == 1) applyGeometry (57.0f, 15.0f, 19.0f, 60.0f, 700.0f, 100.0f);
            else if (id == 2) applyGeometry (54.0f, 14.0f, 18.0f, 60.0f, 760.0f, 100.0f);
            else if (id == 3) applyGeometry (61.0f, 17.0f, 21.0f, 75.0f, 650.0f, 105.0f);
            else if (id == 4) applyGeometry (52.0f, 13.0f, 17.0f, 45.0f, 950.0f, 95.0f);
        }

        void resetDefaults()
        {
            setChoice ("crossfeedalgorithm", 0);
            applyGeometry (57.0f, 15.0f, 19.0f, 60.0f, 700.0f, 100.0f);
            preset.setSelectedId (1, juce::dontSendNotification);
        }

        FlexCurveAudioProcessor& processor;
        juce::ComboBox algorithm, preset;
        juce::Slider circumference, headWidth, headLength, angle, cutoff, direct;
        juce::Label circumferenceLabel, headWidthLabel, headLengthLabel, angleLabel, cutoffLabel, directLabel;
        juce::TextButton reset;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> algorithmAttachment;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> circumferenceAttachment;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> widthAttachment;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> lengthAttachment;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> angleAttachment;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> cutoffAttachment;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> directAttachment;
    };

    juce::String flexCurveHelpText()
    {
        return
            "FlexCurve is a non-destructive multi-layer correction-curve editor and FIR renderer with blending, Graphic EQ, Variable point editing, Parametric EQ, live preview, and project-style presets.\n\n"
            "FIR AND CONVOLUTION\n\n"
            "An FIR (finite impulse response) is a filter stored as a short audio-like impulse. Convolution applies that impulse to the audio. TXT and CSV curves are converted internally; WAV FIR files are read as magnitude responses and regenerated at the current sample rate. A rendered FIR can represent detailed correction continuously and can use Minimum, Natural, or Linear phase.\n\n"
            "SUPPORTED IMPORTS\n\n"
            "- WAV FIR impulse responses\n"
            "- Frequency/dB TXT separated by tabs, spaces, commas, or semicolons\n"
            "- Equalizer APO / Wavelet GraphicEQ text\n"
            "- APO CSV correction curves\n"
            "- Melda FreeForm EQ CSV exports\n"
            "- Equalizer APO parametric EQ text\n"
            "- Stereo frequency/left dB/right dB rows and separate L/R gain directives\n\n"
            "LAYERS AND BLEND\n\n"
            "- Import Curve as a New Layer loads CSV, TXT, or WAV FIR.\n"
            "- Explicit Preamp/global gain from text curves is separated into the layer Gain slider instead of being hidden in curve points. FlexCurve FIR exports preserve that gain in WAV metadata; external FIRs without explicit metadata keep their measured response and default to 0.0 dB layer gain.\n"
            "- Add Curve as a New Layer creates a flat editable layer.\n"
            "- Up to six user layers can coexist. Each has an embedded source curve, color, custom name, visibility, mute, solo, gain, Blend settings, Graphic/Variable EQ, and Parametric EQ.\n"
            "- EQ layers also have a Balance control. It applies a differential L/R dB offset to that layer's correction, so headphone channel-balance fixes are visible, audible, rendered, exported, and saved in presets.\n"
            "- Clone duplicates a layer's embedded curve, EQ banks, Variable points, Parametric filters, Blend, smoothing, inversion, and visibility state under a new name and color.\n"
            "- The persistent color rack is available in every tab: V controls graph visibility, M excludes a layer from audio and Average, and S restricts audio and Average to soloed layers. Click the color circle to make that layer active.\n"
            "- Average is the read-only white result derived live from audible layers and is used by preview and Render FIR.\n"
            "- Per Layer Blend applies Bass, Mid, Treble, and crossover settings to the active EQ layer. Global Blend keeps a separate regional-trim state and applies it to all EQ layers. Layer Gain always remains an independent per-layer control.\n"
            "- Invert reverses a layer correction around 0 dB.\n\n"
            "STEREO L/R LAYERS\n\n"
            "- Every layer stores independent Left and Right source geometry, gain, Blend, Graphic/Variable EQ, Parametric EQ, smoothing, inversion, normalization, and AutoEQ reference state.\n"
            "- Mono imports and new flat layers start Linked L+R. Stereo WAV FIRs and frequency/left dB/right dB text curves preserve independent channels.\n"
            "- Select L, R, or L+R from a layer row or the persistent rack. Clicking an L/R graph legend also selects that channel.\n"
            "- Editing only L or R splits a linked layer without changing the other side. L+R applies the next edit to both sides but does not silently relink an already split layer.\n"
            "- Link is explicit: it copies the currently selected side to both channels and returns the layer to Linked L+R.\n"
            "- Split channels use darker/lighter variants of the layer colour and remain independent through Average, Tracking, preview, FIR render, presets, and export.\n"
            "- AutoEQ uses a shared safe preamp based on the larger L/R peak by default, preserving stereo balance. Independent L/R AutoEQ Preamp is an advanced option that may intentionally alter channel balance.\n\n"
            "GRAPHIC AND VARIABLE EQ\n\n"
            "- Graphic EQ provides editable 15-band and 31-band modes.\n"
            "- The 15-band and 31-band modes have independent gain banks. Switching modes never reinterprets or overwrites the other mode's faders.\n"
            "- Variable mode uses points on the active layer. Double-click adds a point; drag changes frequency/gain; Ctrl-click and drag paints the curve continuously; Shift-click and drag-box select several; Delete/Backspace removes them; arrows and mouse wheel adjust gain.\n"
            "- Numeric Frequency/Gain fields are scrollable and synchronized with graph selection.\n"
            "- Variable editing and processing are enabled only while Graphic EQ is enabled and Variable mode is selected. Switching to 15/31 bands bypasses stored Variable points unless Preserve Variable Shape is enabled explicitly.\n"
            "- Smooth Curve applies non-destructive smoothing to the active layer's Graphic/Variable contribution. Smooth All Layers enables smoothing across the project.\n"
            "- Copy EQ, Paste EQ, and Paste Inverted EQ work within the same layer or across layers. 15-band EQ can paste into 15, 31, or Variable mode; 31-band EQ can paste into 31 or Variable; Variable EQ can paste only into Variable mode. Ctrl-click toggles individual fixed bands, Shift-click selects a continuous range, and double-click resets the selected bands to 0 dB.\n\n"
            "- Paste is blocked only when both source layer and source channel exactly match the destination. Copying L to R in the same layer is allowed.\n\n"
            "TARGET, RAW, AND AUTOEQ\n\n"
            "- Import Target and Import RAW use the same TXT, CSV, and WAV import engine. Each reference category supports up to six layers.\n"
            "- Target curves are dotted references. RAW measurements use thinner translucent lines. Neither category affects audio or the EQ Average.\n"
            "- Layer Visibility independently shows or hides EQ layers, Targets, RAW measurements, corrected-measurement Tracking curves, and Average. These switches are visual only.\n"
            "- Target and RAW Gain sliders are reference offsets: they move the comparison and affect AutoEQ, but never process audio directly.\n"
            "- Normalize RAW + Target @ Frequency applies reversible per-layer offsets so both selected references read exactly 0 dB at the chosen frequency. The editable field accepts 20 to 20000 Hz and defaults to 500 Hz.\n"
            "- The ? button explains why curves from different measurement sources may need manual visual alignment before normalization and links to squig.link, AutoEq, and usyless.uk/trace.\n"
            "- Normalize Layers to 0 dB shifts every audible EQ layer so its complete edited response peaks at exactly 0 dB. This prevents a positively offset layer from producing an already-clipped FIR export.\n"
            "- With two or more visible references, read-only Average Target and Average RAW curves appear automatically.\n"
            "- Show EQ, Show Targets, and Show RAW change graph visibility without deleting or muting data.\n"
            "- Generate AutoEQ computes Target minus RAW, clamps the initial correction to +/-12 dB, smooths it, and creates a new editable EQ layer. Its automatic preamp places the correction peak exactly at 0 dB. Stored RAW and Target data remain unchanged. A separate offset owned by the AutoEQ layer shifts only their analysis display into the preamped comparison context.\n"
            "- Auto mode uses Parametric EQ when its approximation is compact and accurate; otherwise it uses Graphic EQ Variable. The method is stored with the layer and preset.\n"
            "- Each read-only Corrected Measurement Tracking line is recalculated live as stored RAW plus its active AutoEQ correction and uses a darker shade of that EQ layer's colour. Muting, solo-excluding, or bypassing that EQ returns Tracking to stored RAW. The Blend tab reports RMS error against the analysis-adjusted Target from 20 Hz to 16 kHz. Later RAW/Target changes mark the AutoEQ source as outdated instead of silently overwriting user EQ edits; regenerate AutoEQ when ready.\n"
            "- Smooth on a layer row smooths its imported/source geometry non-destructively. Graphic-tab Smooth affects that layer's Graphic/Variable contribution; Smooth All Layers enables both across all layers.\n\n"
            "PARAMETRIC EQ\n\n"
            "- Add as many scrollable filters as needed.\n"
            "- Available filters: Peak, Low Shelf, High Shelf, Low Pass, High Pass, and Notch.\n"
            "- Frequency, Gain, Q, enable, type, reset, copy, paste, inverted paste, and removal are available per active layer.\n\n"
            "GLOBAL CONTROLS\n\n"
            "- Dry/Wet blends latency-aligned dry audio with the correction and visually flattens audible EQ/Average curves toward 0%. RAW, Target, and Tracking references remain in their analysis frame.\n"
            "- Crossfeed reduces hard headphone stereo separation. Advanced opens the algorithm selector: Natural uses geometry-based delay/head-shadow style crossfeed; BS2B/RME style uses a filtered crossfeed topology. Geometry presets adjust head dimensions, speaker angle, cutoff, and direct level.\n"
            "- Balance is a final stereo trim. Negative values shift the corrected output left, positive values shift it right, and audible EQ/Average curves show the same L/R offset.\n"
            "- Gain is a bipolar final output trim and moves audible EQ/Average curves. It does not rewrite or reposition RAW, Target, or Tracking references.\n"
            "- Input Gain is applied before correction and before the Input meter.\n"
            "- Output Gain is applied after correction and Auto Gain, before the final Global Gain stage.\n"
            "- Auto Gain is an output trim for honest level-matched A/B monitoring. It does not change Input Gain, compress, reshape transients, or alter the FIR.\n"
            "- Match Output to Input follows the RMS difference between corrected and input audio. It may boost or cut, but every audio block constrains positive compensation to its current peak headroom so previously learned gain cannot clip OUT after a curve or layer change.\n"
            "- Downward Match never boosts. It also remembers the greatest required peak reduction and only ratchets downward until Auto Gain is disabled, its mode changes, or the project state resets it.\n"
            "- The meter shows live IN, PRE, and OUT RMS/Peak. PRE is measured after correction and manual output stages but before Auto Gain, so correction-induced clipping remains visible even when the final output is attenuated safely.\n"
            "- Meter bars, top peak numbers, and lower IN/AUTO/OUT readouts are dynamic. They do not latch held clip states; red/orange status follows the current audio block and Auto Gain amount.\n"
            "- Limiter is optional and only reduces peaks that exceed 0 dBFS.\n"
            "- Bypass passes the input and flattens audible EQ/Average curves. RAW and Target remain visible, while Tracking returns to the stored RAW response.\n"
            "- The padlock protects project-changing controls after rendering. Visual visibility and active-layer selection remain available; unlock before further edits.\n"
            "- Undo and Redo use only the header arrow buttons. FlexCurve does not capture the DAW's Ctrl+Z, Ctrl+Y, or Ctrl+Shift+Z shortcuts.\n"
            "- Right-click or double-click sliders/knobs to restore defaults. RESET ALL clears the project; Reset to Flat Curves clears all layer corrections.\n\n"
            "GRAPH SCALE AND ZOOM\n\n"
            "- Global dB Scale sets the bipolar gain range used by layer gain, Blend, Graphic EQ, Variable points, Parametric EQ, the global Gain control, and the graph. Presets are +/-12, +/-24, +/-36, and +/-48 dB; an editable custom value is also accepted.\n"
            "- Use + and - to zoom the graph vertically and 1:1 to reset it. Ctrl+mouse-wheel zooms. The mouse wheel scrolls vertically, with Shift for faster movement. A wheel directly over a Variable node edits that node instead. Curves outside the viewport are clipped rather than flattened against its borders.\n\n"
            "PREVIEW, FIR, AND PHASE\n\n"
            "- Before rendering, a causal minimum-phase FIR preview follows the current Average in real time. Curve changes are rebuilt outside the audio thread and swapped into partitioned convolution. The preview FIR itself contains no hidden normalization; the separate Auto Gain stage runs only when its visible switch is enabled, and limiting occurs only when Limiter is enabled.\n"
            "- Render FIR commits the current Average to convolution and collapses the project into the rendered correction.\n"
            "- The same transparent RMS Auto Gain stage is available in Preview and rendered FIR playback, keeping A/B behavior consistent.\n"
            "- Global Gain is monitoring-only and is never included in FIR export. Per-layer EQ gain is part of the correction. Output Gain and Auto Gain are included only when their explicit FIR-export switches are enabled.\n"
            "- Phase Mode is locked to Minimum during preview. After render it unlocks: Minimum is causal with zero reported latency; Natural is mixed-phase with moderate latency and reduced pre-ringing; Linear is symmetric with full latency and flat phase.\n"
            "- Any edit marks the FIR outdated, returns to Minimum preview, and requires a new render.\n"
            "- FIR tap counts and latency scale from a 44.1 kHz reference so responses remain stable at 44.1, 48, 88.2, 96, 176.4, 192 kHz, and other rates.\n"
            "- The top Export FIR button is enabled only for a current rendered FIR and writes a 32-bit WAV at the host sample rate. Linked channels export as mono; split channels export as a true stereo FIR.\n"
            "- Each Blend layer and Average can be exported independently as FIR WAV, frequency/dB TXT or CSV, GraphicEQ/APO text, Melda CSV, or APO parametric text. Parametric export approximates the complete displayed curve with a practical filter set.\n"
            "- When a text format cannot contain independent channels in one file, FlexCurve writes separate _L and _R files.\n"
            "- GraphicEQ/APO exports preserve layer gain as Preamp. FlexCurve FIR WAV exports preserve it as metadata. Other curve formats bake the gain into their exported dB values, so it is never lost or duplicated.\n\n"
            "PRESETS\n\n"
            "- The Presets menu loads, saves, and deletes .flexcurvepreset files in %APPDATA%\\Mixomo\\FlexCurve\\UserPresets\\.\n"
            "- Compatible .calcurvepreset and XML state files can be loaded for migration from CalCurve.\n"
            "- Presets embed every source curve plus names, colors, V/M/S, complete L/R link/selection/edit state, layer gains, normalization offsets, source smoothing, Blend, Graphic/Variable EQ, Parametric filters, active layer, Average state, Input/Output Gain, Auto Gain mode, FIR-export options, global controls, phase, and rendered state where available.\n"
            "- Embedded curves keep presets usable if original imported files move or disappear.\n\n"
            "CREDITS\n\n"
            "Development: Ezequiel Casas (Mixomo)\nhttps://github.com/Mixomo\n\n"
            "Example curves:\nhttps://github.com/Mixomo/My-Headphones-Calibration-Files\n\n"
            "More calibration curves:\nhttps://autoeq.app/\n\n"
            "Thanks and credit to Jaakko Pasanen:\nhttps://github.com/jaakkopasanen\n\n"
            "AutoEq:\nhttps://github.com/jaakkopasanen/AutoEq\n\n"
            "squig.link:\nhttps://squig.link/\n\n"
            "MeldaProduction:\nhttps://www.meldaproduction.com/\n\n"
            "Steinberg / VST3 SDK:\nhttps://github.com/steinbergmedia/vst3sdk\n\n"
            "JUCE:\nhttps://juce.com/\n\n"
            "Microsoft MSVC / Visual Studio C++ toolchain:\nhttps://visualstudio.microsoft.com/\n\n"
            "C++ language created by Bjarne Stroustrup.\n\n"
            "LICENSE\n\n"
            "FlexCurve: GNU General Public License v3.0 (GPLv3).\n"
            "VST3 SDK components: MIT License, copyright Steinberg Media Technologies GmbH.\n"
            "JUCE framework: AGPLv3/commercial licensing.\n";
    }

    class HelpContent final : public juce::Component
    {
    public:
        HelpContent()
        {
            text.setMultiLine (true);
            text.setReadOnly (true);
            text.setScrollbarsShown (true);
            text.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff12171d));
            text.setColour (juce::TextEditor::textColourId, inkColour());
            text.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff38424f));
            text.setFont (juce::FontOptions (16.0f));
            text.setText (flexCurveHelpText(), false);
            addAndMakeVisible (text);
            setSize (900, 760);
        }

        void resized() override
        {
            text.setBounds (getLocalBounds().reduced (12));
        }

    private:
        juce::TextEditor text;
    };
}

FlexCurveGraph::FlexCurveGraph (FlexCurveAudioProcessor& p)
    : processor (p)
{
    setWantsKeyboardFocus (true);
}

double FlexCurveGraph::getMinDb() const
{
    return viewCentreDb - viewHalfRangeDb / viewZoom;
}

double FlexCurveGraph::getMaxDb() const
{
    return viewCentreDb + viewHalfRangeDb / viewZoom;
}

int FlexCurveGraph::getLegendRowCount() const
{
    auto count = processor.isAverageVisible() && ! processor.getAverageCurve().empty() ? 1 : 0;
    int targetCount = 0;
    int rawCount = 0;
    for (const auto& layer : processor.getLayers())
        if (layer.visible && processor.isLayerTypeVisible (layer.type))
        {
            ++count;
            if (! layer.channelsLinked) ++count;
            if (layer.type == FlexCurveLayerType::target) ++targetCount;
            if (layer.type == FlexCurveLayerType::raw) ++rawCount;
            if (layer.type == FlexCurveLayerType::eq && layer.autoEqRawLayerId > 0)
                count += layer.channelsLinked ? 1 : 2;
        }
    if (targetCount >= 2) ++count;
    if (rawCount >= 2) ++count;
    return count > 0 ? (count + 5) / 6 : 0;
}

juce::Rectangle<float> FlexCurveGraph::getPlotBounds() const
{
    auto graph = getLocalBounds().toFloat().reduced (16.0f, 12.0f);
    graph.removeFromBottom (26.0f);
    return graph;
}

void FlexCurveGraph::zoomIn()
{
    viewZoom = juce::jlimit (0.25, 16.0, viewZoom * 1.35);
    repaint();
}

void FlexCurveGraph::zoomOut()
{
    viewZoom = juce::jlimit (0.25, 16.0, viewZoom / 1.35);
    repaint();
}

void FlexCurveGraph::resetZoom()
{
    viewZoom = 1.0;
    viewCentreDb = 0.0;
    repaint();
}

void FlexCurveGraph::setViewRange (double halfRangeDb)
{
    viewHalfRangeDb = juce::jlimit (1.0, 192.0, halfRangeDb);
    resetZoom();
}

juce::Point<float> FlexCurveGraph::mapPoint (double frequency, double db, juce::Rectangle<float> graph, double minDb, double maxDb) const
{
    const auto xNorm = (std::log10 (juce::jlimit (20.0, 20000.0, frequency)) - std::log10 (20.0)) / (std::log10 (20000.0) - std::log10 (20.0));
    const auto yNorm = juce::jmap (db, minDb, maxDb, 1.0, 0.0);
    return { graph.getX() + static_cast<float> (xNorm) * graph.getWidth(),
             graph.getY() + static_cast<float> (yNorm) * graph.getHeight() };
}

CurvePoint FlexCurveGraph::unmapPoint (juce::Point<float> point, juce::Rectangle<float> graph, double minDb, double maxDb) const
{
    const auto xNorm = juce::jlimit (0.0, 1.0, static_cast<double> ((point.x - graph.getX()) / graph.getWidth()));
    const auto yNorm = juce::jlimit (0.0, 1.0, static_cast<double> ((point.y - graph.getY()) / graph.getHeight()));
    const auto frequency = 20.0 * std::pow (1000.0, xNorm);
    const auto db = juce::jmap (yNorm, 1.0, 0.0, minDb, maxDb);
    return { frequency, db };
}

juce::Path FlexCurveGraph::buildPath (const std::vector<CurvePoint>& points, juce::Rectangle<float> graph, double minDb, double maxDb) const
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
        const auto mapped = mapPoint (point.frequency, point.db, graph, minDb, maxDb);
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

int FlexCurveGraph::findFreeformPointNear (juce::Point<float> point, juce::Rectangle<float> graph, double minDb, double maxDb) const
{
    const auto freeform = processor.getFreeformPoints();
    const auto activeCurve = graphDisplayAudioCurveFor (processor, processor.getLayerCurve (processor.getActiveLayerId()));
    for (int i = 0; i < static_cast<int> (freeform.size()); ++i)
    {
        const auto frequency = freeform[static_cast<size_t> (i)].frequency;
        const auto displayDb = CurveFIR::interpolateDb (activeCurve, frequency);
        if (mapPoint (frequency, displayDb, graph, minDb, maxDb).getDistanceFrom (point) < 12.0f)
            return i;
    }
    return -1;
}

bool FlexCurveGraph::isVariableEditingActive() const
{
    return variableEditingAllowed
        && processor.hasEditableActiveLayer()
        && processor.isGraphicEnabled()
        && processor.getGraphicMode() == 0
        && processor.parameters.getRawParameterValue ("bypass")->load() < 0.5f;
}

void FlexCurveGraph::setVariableEditingAllowed (bool allowed)
{
    if (variableEditingAllowed == allowed)
        return;
    variableEditingAllowed = allowed;
    if (! allowed)
    {
        marqueeSelecting = false;
        draggingFreeformIndex = -1;
        paintingFreeform = false;
    }
    repaint();
}

bool FlexCurveGraph::isSelected (int index) const
{
    return std::find (selectedFreeformIndices.begin(), selectedFreeformIndices.end(), index) != selectedFreeformIndices.end();
}

void FlexCurveGraph::setSingleSelection (int index)
{
    selectedFreeformIndices.clear();
    if (index >= 0)
        selectedFreeformIndices.push_back (index);
    selectedFreeformIndex = index;
}

void FlexCurveGraph::toggleSelection (int index)
{
    if (index < 0)
        return;

    const auto it = std::find (selectedFreeformIndices.begin(), selectedFreeformIndices.end(), index);
    if (it == selectedFreeformIndices.end())
    {
        selectedFreeformIndices.push_back (index);
        selectedFreeformIndex = index;
    }
    else
    {
        selectedFreeformIndices.erase (it);
        selectedFreeformIndex = selectedFreeformIndices.empty() ? -1 : selectedFreeformIndices.back();
    }
}

std::vector<int> FlexCurveGraph::getSelectedFreeformIndices() const
{
    return selectedFreeformIndices;
}

void FlexCurveGraph::selectFreeformPoint (int index, bool additive)
{
    if (additive)
        toggleSelection (index);
    else
        setSingleSelection (index);
    grabKeyboardFocus();
    repaint();
}

void FlexCurveGraph::deleteSelectedPoints()
{
    if (selectedFreeformIndices.empty())
        return;

    auto points = processor.getFreeformPoints();
    std::sort (selectedFreeformIndices.begin(), selectedFreeformIndices.end(), std::greater<int>());
    selectedFreeformIndices.erase (std::unique (selectedFreeformIndices.begin(), selectedFreeformIndices.end()), selectedFreeformIndices.end());

    for (const auto index : selectedFreeformIndices)
        if (index >= 0 && index < static_cast<int> (points.size()))
            points.erase (points.begin() + index);

    selectedFreeformIndices.clear();
    selectedFreeformIndex = -1;
    draggingFreeformIndex = -1;
    processor.setFreeformPoints (std::move (points));
    repaint();
}

void FlexCurveGraph::paintFreeformAt (juce::Point<float> position)
{
    const auto graph = getPlotBounds();
    if (! graph.contains (position))
        return;

    auto segmentStart = lastPaintPosition;
    auto segmentLength = segmentStart.getDistanceFrom (position);
    if (segmentLength <= 0.001f)
        return;

    const auto baseCurve = graphDisplayAudioCurveFor (
        processor, processor.getLayerCurveWithoutFreeform (processor.getActiveLayerId()));
    const auto dryWet = juce::jmax (0.05f, processor.parameters.getRawParameterValue ("drywet")->load());
    const auto layers = processor.getLayers();
    const auto activeId = processor.getActiveLayerId();
    const auto active = std::find_if (layers.begin(), layers.end(), [activeId] (const auto& layer)
    {
        return layer.id == activeId;
    });
    const auto range = static_cast<double> (processor.getGlobalDbRange());
    auto points = processor.getFreeformPoints();
    std::vector<juce::Point<float>> emittedPositions;
    constexpr float pointSpacing = 6.0f;

    while (segmentLength + 0.001f >= paintDistanceToNextPoint)
    {
        const auto direction = (position - segmentStart) / segmentLength;
        const auto strokePosition = segmentStart + direction * paintDistanceToNextPoint;
        emittedPositions.push_back (strokePosition);
        segmentStart = strokePosition;
        segmentLength = segmentStart.getDistanceFrom (position);
        paintDistanceToNextPoint = pointSpacing;
    }

    paintDistanceToNextPoint -= segmentLength;
    lastPaintPosition = position;

    if (emittedPositions.empty())
        return;

    for (const auto strokePosition : emittedPositions)
    {
        const auto mapped = unmapPoint (strokePosition, graph, getMinDb(), getMaxDb());
        const auto baseDb = CurveFIR::interpolateDb (baseCurve, mapped.frequency);
        auto contributionDb = (mapped.db - baseDb) / dryWet;
        if (active != layers.end() && active->inverted)
            contributionDb = -contributionDb;
        contributionDb = juce::jlimit (-range, range, contributionDb);

        points.erase (std::remove_if (points.begin(), points.end(), [&] (const auto& point)
        {
            const auto existingX = mapPoint (point.frequency, mapped.db, graph, getMinDb(), getMaxDb()).x;
            return std::abs (existingX - strokePosition.x) < pointSpacing * 0.45f;
        }), points.end());
        points.push_back ({ mapped.frequency, contributionDb });
    }

    processor.setFreeformPoints (std::move (points));
    const auto updated = processor.getFreeformPoints();
    int selected = -1;
    if (! updated.empty())
    {
        const auto finalMapped = unmapPoint (position, graph, getMinDb(), getMaxDb());
        auto closestFrequency = std::numeric_limits<double>::max();
        for (int i = 0; i < static_cast<int> (updated.size()); ++i)
        {
            const auto frequencyDistance = std::abs (
                std::log (updated[static_cast<size_t> (i)].frequency / finalMapped.frequency));
            if (frequencyDistance < closestFrequency)
            {
                closestFrequency = frequencyDistance;
                selected = i;
            }
        }
    }
    setSingleSelection (selected);
    repaint();
}

void FlexCurveGraph::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    auto graph = getPlotBounds();

    const auto layers = processor.getLayers();
    const auto freeform = processor.getFreeformPoints();
    const auto activeLayerId = processor.getActiveLayerId();
    const auto minDb = getMinDb();
    const auto maxDb = getMaxDb();
    legendHitBoxes.clear();

    g.setColour (juce::Colour (0xff0c0f13));
    g.fillRoundedRectangle (bounds, 8.0f);

    g.setColour (juce::Colour (0xff252d37));
    for (auto hz : { 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
    {
        const auto x = mapPoint (hz, 0.0, graph, minDb, maxDb).x;
        g.drawVerticalLine (static_cast<int> (std::round (x)), graph.getY(), graph.getBottom());
    }

    const auto dbStep = juce::jmax (1.0, std::pow (2.0, std::floor (std::log2 ((maxDb - minDb) / 8.0))));
    for (auto db = std::ceil (minDb / dbStep) * dbStep; db <= maxDb + 0.1; db += dbStep)
    {
        const auto y = mapPoint (1000.0, db, graph, minDb, maxDb).y;
        g.drawHorizontalLine (static_cast<int> (std::round (y)), graph.getX(), graph.getRight());
    }

    g.setFont (juce::FontOptions (10.5f));
    g.setColour (mutedColour());
    for (auto hz : { 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
    {
        const auto x = mapPoint (hz, 0.0, graph, minDb, maxDb).x;
        g.drawText (frequencyLabel (hz), juce::Rectangle<float> (x - 22.0f, graph.getBottom() + 6.0f, 44.0f, 14.0f), juce::Justification::centred);
    }

    for (auto db = std::ceil (minDb / dbStep) * dbStep; db <= maxDb + 0.1; db += dbStep)
    {
        const auto y = mapPoint (1000.0, db, graph, minDb, maxDb).y;
        g.drawText (juce::String (juce::roundToInt (db)) + " dB",
                    juce::Rectangle<float> (graph.getX() + 4.0f, y - 8.0f, 48.0f, 14.0f),
                    juce::Justification::left);
    }

    g.saveState();
    g.reduceClipRegion (graph.toNearestInt());

    auto drawLayerPath = [&] (const FlexCurveLayer& layer, const std::vector<CurvePoint>& curve, float width)
    {
        const auto path = buildPath (curve, graph, minDb, maxDb);
        if (layer.type == FlexCurveLayerType::target)
        {
            juce::Path dashed;
            const float pattern[] { 6.0f, 4.0f };
            juce::PathStrokeType (width).createDashedStroke (dashed, path, pattern, 2);
            g.fillPath (dashed);
        }
        else
        {
            g.strokePath (path, juce::PathStrokeType (layer.type == FlexCurveLayerType::raw ? 1.0f : width));
        }
    };

    for (const auto& layer : layers)
    {
        if (layer.id == activeLayerId)
            continue;
        if (! layer.visible || ! processor.isLayerTypeVisible (layer.type))
            continue;
        const auto alpha = layer.type == FlexCurveLayerType::raw ? 0.46f : 1.0f;
        auto drawChannel = [&] (FlexChannelSelection channel, juce::Colour colour)
        {
            const auto curve = layer.type == FlexCurveLayerType::eq
                ? graphDisplayAudioCurveFor (processor, processor.getLayerCurve (layer.id, channel), channel)
                : processor.getLayerCurveForDisplay (layer.id, channel);
            if (! curve.empty())
            {
                g.setColour ((layer.muted ? colour.darker (0.55f) : colour).withAlpha (alpha));
                drawLayerPath (layer, curve, 1.55f);
            }
        };
        drawChannel (FlexChannelSelection::left,
                     layer.channelsLinked ? layer.colour : layer.colour.darker (0.32f));
        if (! layer.channelsLinked)
            drawChannel (FlexChannelSelection::right, layer.colour.brighter (0.30f));
    }

    if (processor.isAverageVisible())
    {
        const auto averageLeft = graphDisplayAudioCurveFor (
            processor, processor.getAverageCurve (FlexChannelSelection::left),
            FlexChannelSelection::left);
        const auto globalBalanceVisible = std::abs (
            processor.parameters.getRawParameterValue ("globalbalance")->load()) > 0.001f;
        if (! averageLeft.empty())
        {
            g.setColour ((processor.averageChannelsDiffer() || globalBalanceVisible)
                             ? juce::Colours::white.darker (0.28f) : juce::Colours::white);
            g.strokePath (buildPath (averageLeft, graph, minDb, maxDb),
                          juce::PathStrokeType (2.0f));
            if (processor.averageChannelsDiffer() || globalBalanceVisible)
            {
                const auto averageRight = graphDisplayAudioCurveFor (
                    processor, processor.getAverageCurve (FlexChannelSelection::right),
                    FlexChannelSelection::right);
                g.setColour (juce::Colours::white.brighter (0.12f));
                g.strokePath (buildPath (averageRight, graph, minDb, maxDb),
                              juce::PathStrokeType (2.0f));
            }
        }
    }

    const auto visibleTargets = std::count_if (layers.begin(), layers.end(), [this] (const auto& layer)
    {
        return layer.type == FlexCurveLayerType::target && layer.visible
            && processor.isLayerTypeVisible (FlexCurveLayerType::target);
    });
    const auto visibleRaws = std::count_if (layers.begin(), layers.end(), [this] (const auto& layer)
    {
        return layer.type == FlexCurveLayerType::raw && layer.visible
            && processor.isLayerTypeVisible (FlexCurveLayerType::raw);
    });
    if (visibleTargets >= 2)
    {
        const auto average = processor.getAverageCurveForTypeForDisplay (
            FlexCurveLayerType::target, FlexChannelSelection::left);
        const auto averageRight = processor.getAverageCurveForTypeForDisplay (
            FlexCurveLayerType::target, FlexChannelSelection::right);
        juce::Path dashed;
        const float pattern[] { 7.0f, 4.0f };
        juce::PathStrokeType (1.8f).createDashedStroke (dashed, buildPath (average, graph, minDb, maxDb), pattern, 2);
        g.setColour (juce::Colours::white.withAlpha (0.72f));
        g.fillPath (dashed);
        if (curvesDiffer (average, averageRight))
        {
            juce::Path rightDashed;
            juce::PathStrokeType (1.8f).createDashedStroke (
                rightDashed, buildPath (averageRight, graph, minDb, maxDb), pattern, 2);
            g.setColour (juce::Colours::white.withAlpha (0.42f));
            g.fillPath (rightDashed);
        }
    }
    if (visibleRaws >= 2)
    {
        const auto average = processor.getAverageCurveForTypeForDisplay (
            FlexCurveLayerType::raw, FlexChannelSelection::left);
        const auto averageRight = processor.getAverageCurveForTypeForDisplay (
            FlexCurveLayerType::raw, FlexChannelSelection::right);
        g.setColour (juce::Colours::lightgrey.withAlpha (0.56f));
        g.strokePath (buildPath (average, graph, minDb, maxDb), juce::PathStrokeType (1.15f));
        if (curvesDiffer (average, averageRight))
        {
            g.setColour (juce::Colours::lightgrey.withAlpha (0.32f));
            g.strokePath (buildPath (averageRight, graph, minDb, maxDb),
                          juce::PathStrokeType (1.15f));
        }
    }

    for (const auto& layer : layers)
    {
        if (! processor.areCorrectedMeasurementsVisible()
            || layer.type != FlexCurveLayerType::eq || layer.autoEqRawLayerId <= 0 || ! layer.visible)
            continue;
        auto drawTracking = [&] (FlexChannelSelection channel, juce::Colour colour)
        {
            const auto corrected = processor.getCorrectedMeasurementCurve (layer.id, channel);
            if (corrected.empty())
                return;
            const auto path = buildPath (corrected, graph, minDb, maxDb);
            juce::Path dashed;
            const float pattern[] { 3.0f, 3.0f };
            juce::PathStrokeType (1.7f).createDashedStroke (dashed, path, pattern, 2);
            g.setColour (colour);
            g.fillPath (dashed);
        };
        drawTracking (FlexChannelSelection::left, layer.colour.darker (0.62f));
        if (! layer.channelsLinked)
            drawTracking (FlexChannelSelection::right, layer.colour.darker (0.38f));
    }

    if (activeLayerId > 0)
    {
        const auto visible = std::any_of (layers.begin(), layers.end(), [this, activeLayerId] (const auto& layer)
        {
            return layer.id == activeLayerId && layer.visible && processor.isLayerTypeVisible (layer.type);
        });

        if (visible)
        {
            const auto activeIt = std::find_if (layers.begin(), layers.end(), [activeLayerId] (const auto& layer)
            {
                return layer.id == activeLayerId;
            });
            if (activeIt != layers.end())
            {
                auto drawActiveChannel = [&] (FlexChannelSelection channel, juce::Colour colour,
                                               float width)
                {
                    const auto curve = activeIt->type == FlexCurveLayerType::eq
                        ? graphDisplayAudioCurveFor (
                            processor, processor.getLayerCurve (activeLayerId, channel), channel)
                        : processor.getLayerCurveForDisplay (activeLayerId, channel);
                    if (! curve.empty())
                    {
                        g.setColour (colour.withAlpha (
                            activeIt->type == FlexCurveLayerType::raw ? 0.78f : 1.0f));
                        drawLayerPath (*activeIt, curve, width);
                    }
                };
                const auto selected = activeIt->selectedChannel;
                drawActiveChannel (FlexChannelSelection::left,
                                   activeIt->channelsLinked ? activeIt->colour
                                                            : activeIt->colour.darker (0.32f),
                                   selected == FlexChannelSelection::right ? 2.0f : 2.8f);
                if (! activeIt->channelsLinked)
                    drawActiveChannel (FlexChannelSelection::right,
                                       activeIt->colour.brighter (0.30f),
                                       selected == FlexChannelSelection::left ? 2.0f : 2.8f);
            }
        }
    }

    if (isVariableEditingActive() && ! freeform.empty())
    {
        const auto nodeColour = juce::Colour (0xffff5aa8);
        std::vector<CurvePoint> displayedNodes;
        const auto activeCurve = graphDisplayAudioCurveFor (processor, processor.getLayerCurve (activeLayerId));
        displayedNodes.reserve (freeform.size());
        for (const auto& point : freeform)
            displayedNodes.push_back ({ point.frequency, CurveFIR::interpolateDb (activeCurve, point.frequency) });

        g.setColour (nodeColour.withAlpha (0.92f));
        g.strokePath (buildPath (displayedNodes, graph, minDb, maxDb), juce::PathStrokeType (1.2f));
        for (int i = 0; i < static_cast<int> (displayedNodes.size()); ++i)
        {
            const auto& point = displayedNodes[static_cast<size_t> (i)];
            const auto centre = mapPoint (point.frequency, point.db, graph, minDb, maxDb);
            const auto selected = isSelected (i);
            const auto size = selected ? 9.0f : 7.0f;
            auto dot = juce::Rectangle<float> (size, size).withCentre (centre);
            g.setColour (selected ? juce::Colour (0xff2a87ff) : nodeColour);
            g.fillEllipse (dot);
            g.setColour (inkColour());
            g.drawEllipse (dot, 1.0f);
            g.setFont (juce::FontOptions (8.0f));
            g.setColour (juce::Colour (0xff0c0f13));
            g.drawText (juce::String (i + 1), juce::Rectangle<float> (centre.x - 10.0f, centre.y - 5.0f, 20.0f, 10.0f), juce::Justification::centred);
        }
    }

    g.restoreState();

    std::vector<const FlexCurveLayer*> legendLayers;
    std::vector<const FlexCurveLayer*> trackingLegendLayers;
    for (const auto& layer : layers)
    {
        if (layer.visible && processor.isLayerTypeVisible (layer.type))
            legendLayers.push_back (&layer);
        if (processor.areCorrectedMeasurementsVisible() && layer.visible
            && layer.type == FlexCurveLayerType::eq && layer.autoEqRawLayerId > 0)
            trackingLegendLayers.push_back (&layer);
    }

    const auto showAverageLegend = processor.isAverageVisible() && ! processor.getAverageCurve().empty();
    const auto channelLegendCount = std::accumulate (
        legendLayers.begin(), legendLayers.end(), 0,
        [] (int total, const auto* layer) { return total + (layer->channelsLinked ? 1 : 2); });
    const auto trackingChannelLegendCount = std::accumulate (
        trackingLegendLayers.begin(), trackingLegendLayers.end(), 0,
        [] (int total, const auto* layer) { return total + (layer->channelsLinked ? 1 : 2); });
    const auto legendCount = channelLegendCount
                           + (showAverageLegend ? (processor.averageChannelsDiffer() ? 2 : 1) : 0)
                           + (visibleTargets >= 2 ? 1 : 0) + (visibleRaws >= 2 ? 1 : 0)
                           + trackingChannelLegendCount;
    auto legendArea = bounds.reduced (80.0f, 12.0f);
    legendArea = legendArea.removeFromTop (static_cast<float> (getLegendRowCount() * 22));
    g.setFont (juce::FontOptions (10.5f));
    const auto columns = juce::jmin (6, juce::jmax (1, legendCount));
    const auto itemWidth = legendArea.getWidth() / static_cast<float> (columns);
    if (legendCount > 0)
    {
        g.setColour (juce::Colour (0xff0c0f13).withAlpha (0.72f));
        g.fillRoundedRectangle (legendArea.expanded (6.0f, 3.0f), 5.0f);
    }
    int legendIndex = 0;
    auto drawLegendItem = [&] (juce::String text, juce::Colour colour, int id,
                               FlexChannelSelection channel, bool active, bool muted)
    {
        const auto row = legendIndex / 6;
        const auto column = legendIndex % 6;
        auto item = juce::Rectangle<float> (legendArea.getX() + column * itemWidth,
                                            legendArea.getY() + row * 22.0f,
                                            itemWidth,
                                            20.0f);
        legendHitBoxes.push_back ({ item, id, channel });
        g.setColour (colour);
        auto swatch = item.removeFromLeft (10.0f).withSizeKeepingCentre (8.0f, 8.0f);
        g.fillRoundedRectangle (swatch, 2.0f);
        g.setColour (active ? inkColour() : inkColour().withAlpha (muted ? 0.55f : 0.86f));
        g.drawFittedText ((active ? "> " : "") + text,
                          item.reduced (4.0f, 0.0f).toNearestInt(),
                          juce::Justification::centredLeft,
                          1,
                          0.55f);
        ++legendIndex;
    };

    for (const auto* layer : legendLayers)
    {
        const auto prefix = layer->type == FlexCurveLayerType::target ? "Target: "
                          : layer->type == FlexCurveLayerType::raw ? "RAW: " : "";
        if (layer->channelsLinked)
            drawLegendItem (prefix + layer->name + " [L+R]", layer->colour, layer->id,
                            FlexChannelSelection::stereo, layer->id == activeLayerId, layer->muted);
        else
        {
            drawLegendItem (prefix + layer->name + " [L]", layer->colour.darker (0.32f),
                            layer->id, FlexChannelSelection::left, layer->id == activeLayerId
                                && layer->selectedChannel != FlexChannelSelection::right,
                            layer->muted);
            drawLegendItem (prefix + layer->name + " [R]", layer->colour.brighter (0.30f),
                            layer->id, FlexChannelSelection::right, layer->id == activeLayerId
                                && layer->selectedChannel == FlexChannelSelection::right,
                            layer->muted);
        }
    }
    for (const auto* layer : trackingLegendLayers)
    {
        const auto suffix = layer->autoEqSourcesOutdated ? " (source changed)" : "";
        if (layer->channelsLinked)
            drawLegendItem ("Corrected: " + layer->name + " [L+R]" + suffix,
                            layer->colour.darker (0.55f), 0, FlexChannelSelection::stereo,
                            false, layer->muted);
        else
        {
            drawLegendItem ("Corrected: " + layer->name + " [L]" + suffix,
                            layer->colour.darker (0.68f), 0, FlexChannelSelection::left,
                            false, layer->muted);
            drawLegendItem ("Corrected: " + layer->name + " [R]" + suffix,
                            layer->colour.darker (0.38f), 0, FlexChannelSelection::right,
                            false, layer->muted);
        }
    }
    if (showAverageLegend)
    {
        if (processor.averageChannelsDiffer())
        {
            drawLegendItem ("Average [L]", juce::Colours::white.darker (0.28f),
                            -1, FlexChannelSelection::left, activeLayerId == -1, false);
            drawLegendItem ("Average [R]", juce::Colours::white.brighter (0.12f),
                            -1, FlexChannelSelection::right, activeLayerId == -1, false);
        }
        else
            drawLegendItem ("Average [L+R]", juce::Colours::white,
                            -1, FlexChannelSelection::stereo, activeLayerId == -1, false);
    }
    if (visibleTargets >= 2)
        drawLegendItem ("Average Target", juce::Colours::white.withAlpha (0.72f), 0,
                        FlexChannelSelection::stereo, false, false);
    if (visibleRaws >= 2)
        drawLegendItem ("Average RAW", juce::Colours::lightgrey.withAlpha (0.70f), 0,
                        FlexChannelSelection::stereo, false, false);

    if (marqueeSelecting)
    {
        g.setColour (accentColour().withAlpha (0.14f));
        g.fillRect (marqueeBounds);
        g.setColour (accentColour().withAlpha (0.85f));
        g.drawRect (marqueeBounds, 1.0f);
    }
}

void FlexCurveGraph::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (! isVariableEditingActive())
        return;

    const auto minDb = getMinDb();
    const auto maxDb = getMaxDb();
    auto graph = getPlotBounds();
    auto points = processor.getFreeformPoints();
    const auto clicked = unmapPoint (e.position, graph, minDb, maxDb);
    points.push_back ({ clicked.frequency, 0.0 });
    processor.setFreeformPoints (std::move (points));
    setSingleSelection (static_cast<int> (processor.getFreeformPoints().size()) - 1);
    grabKeyboardFocus();
    repaint();
}

void FlexCurveGraph::mouseDown (const juce::MouseEvent& e)
{
    for (const auto& hit : legendHitBoxes)
        if (hit.bounds.contains (e.position))
        {
            processor.setActiveLayerId (hit.layerId);
            if (hit.layerId > 0)
                processor.setLayerChannelSelection (hit.layerId, hit.channel);
            repaint();
            return;
        }

    if (! isVariableEditingActive())
        return;

    if (e.mods.isCtrlDown() && e.mods.isLeftButtonDown())
    {
        paintingFreeform = true;
        marqueeSelecting = false;
        draggingFreeformIndex = -1;
        lastPaintPosition = e.position;
        paintDistanceToNextPoint = 6.0f;
        grabKeyboardFocus();
        return;
    }

    const auto minDb = getMinDb();
    const auto maxDb = getMaxDb();
    auto graph = getPlotBounds();
    draggingFreeformIndex = findFreeformPointNear (e.position, graph, minDb, maxDb);
    grabKeyboardFocus();

    if (e.mods.isRightButtonDown() && draggingFreeformIndex >= 0)
    {
        if (! isSelected (draggingFreeformIndex))
            setSingleSelection (draggingFreeformIndex);
        deleteSelectedPoints();
        return;
    }

    if (draggingFreeformIndex >= 0)
    {
        if (e.mods.isShiftDown())
            toggleSelection (draggingFreeformIndex);
        else if (! isSelected (draggingFreeformIndex))
            setSingleSelection (draggingFreeformIndex);

        lastDragPoint = unmapPoint (e.position, graph, minDb, maxDb);
        repaint();
        return;
    }

    if (! e.mods.isShiftDown())
        setSingleSelection (-1);

    marqueeSelecting = true;
    marqueeStart = e.position;
    marqueeBounds = juce::Rectangle<float> (marqueeStart, marqueeStart);
    repaint();
}

void FlexCurveGraph::mouseDrag (const juce::MouseEvent& e)
{
    if (! isVariableEditingActive())
        return;

    if (paintingFreeform)
    {
        if (e.mods.isCtrlDown() && e.mods.isLeftButtonDown())
            paintFreeformAt (e.position);
        return;
    }

    const auto minDb = getMinDb();
    const auto maxDb = getMaxDb();
    auto graph = getPlotBounds();

    if (marqueeSelecting)
    {
        marqueeBounds = juce::Rectangle<float> (marqueeStart, e.position).getSmallestIntegerContainer().toFloat();
        repaint();
        return;
    }

    if (draggingFreeformIndex < 0)
        return;

    auto points = processor.getFreeformPoints();
    if (draggingFreeformIndex < static_cast<int> (points.size()))
    {
        const auto next = unmapPoint (e.position, graph, minDb, maxDb);
        const auto ratio = next.frequency / juce::jmax (20.0, lastDragPoint.frequency);
        const auto dryWet = juce::jmax (0.05f, processor.parameters.getRawParameterValue ("drywet")->load());
        auto dbDelta = (next.db - lastDragPoint.db) / dryWet;
        const auto layers = processor.getLayers();
        const auto activeId = processor.getActiveLayerId();
        const auto active = std::find_if (layers.begin(), layers.end(), [activeId] (const auto& layer) { return layer.id == activeId; });
        if (active != layers.end() && active->inverted)
            dbDelta = -dbDelta;

        if (selectedFreeformIndices.empty())
            setSingleSelection (draggingFreeformIndex);

        for (const auto index : selectedFreeformIndices)
        {
            if (index < 0 || index >= static_cast<int> (points.size()))
                continue;
            auto& point = points[static_cast<size_t> (index)];
            point.frequency = juce::jlimit (20.0, 20000.0, point.frequency * ratio);
            const auto range = static_cast<double> (processor.getGlobalDbRange());
            point.db = juce::jlimit (-range, range, point.db + dbDelta);
        }

        processor.setFreeformPoints (std::move (points));
        selectedFreeformIndex = draggingFreeformIndex;
        lastDragPoint = next;
        repaint();
    }
}

void FlexCurveGraph::mouseUp (const juce::MouseEvent& e)
{
    if (paintingFreeform)
    {
        paintingFreeform = false;
        draggingFreeformIndex = -1;
        return;
    }

    if (! isVariableEditingActive())
    {
        marqueeSelecting = false;
        draggingFreeformIndex = -1;
        return;
    }

    if (! marqueeSelecting)
    {
        draggingFreeformIndex = -1;
        return;
    }

    const auto minDb = getMinDb();
    const auto maxDb = getMaxDb();
    auto graph = getPlotBounds();

    if (! e.mods.isShiftDown())
        selectedFreeformIndices.clear();

    const auto points = processor.getFreeformPoints();
    const auto activeCurve = graphDisplayAudioCurveFor (processor, processor.getLayerCurve (processor.getActiveLayerId()));
    for (int i = 0; i < static_cast<int> (points.size()); ++i)
    {
        const auto frequency = points[static_cast<size_t> (i)].frequency;
        const auto mapped = mapPoint (frequency, CurveFIR::interpolateDb (activeCurve, frequency), graph, minDb, maxDb);
        if (marqueeBounds.contains (mapped))
            if (! isSelected (i))
                selectedFreeformIndices.push_back (i);
    }

    selectedFreeformIndex = selectedFreeformIndices.empty() ? -1 : selectedFreeformIndices.back();
    marqueeSelecting = false;
    draggingFreeformIndex = -1;
    repaint();
}

bool FlexCurveGraph::keyPressed (const juce::KeyPress& key)
{
    if (! isVariableEditingActive())
        return false;

    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
    {
        deleteSelectedPoints();
        return true;
    }

    if (selectedFreeformIndex < 0)
        return false;

    auto points = processor.getFreeformPoints();
    if (selectedFreeformIndex >= static_cast<int> (points.size()))
        return false;

    const auto step = key.getModifiers().isShiftDown() ? 1.0 : 0.25;
    const auto delta = key == juce::KeyPress::upKey ? step : (key == juce::KeyPress::downKey ? -step : 0.0);
    if (delta == 0.0)
        return false;

    if (selectedFreeformIndices.empty())
        selectedFreeformIndices.push_back (selectedFreeformIndex);

    for (const auto index : selectedFreeformIndices)
        if (index >= 0 && index < static_cast<int> (points.size()))
        {
            const auto range = static_cast<double> (processor.getGlobalDbRange());
            points[static_cast<size_t> (index)].db = juce::jlimit (-range, range, points[static_cast<size_t> (index)].db + delta);
        }

    processor.setFreeformPoints (std::move (points));
    repaint();
    return true;
}

void FlexCurveGraph::mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    if (event.mods.isCtrlDown())
    {
        if (wheel.deltaY > 0.0f) zoomIn(); else zoomOut();
        return;
    }

    const auto graph = getPlotBounds();
    const auto hoveredPoint = isVariableEditingActive()
        ? findFreeformPointNear (event.position, graph, getMinDb(), getMaxDb())
        : -1;
    if (hoveredPoint < 0)
    {
        const auto speed = event.mods.isShiftDown() ? 10.0 : 4.0;
        viewCentreDb = juce::jlimit (-192.0, 192.0,
                                     viewCentreDb + static_cast<double> (wheel.deltaY) * speed / viewZoom);
        repaint();
        return;
    }

    if (std::abs (wheel.deltaY) <= 0.0f)
        return;

    auto points = processor.getFreeformPoints();
    if (hoveredPoint >= static_cast<int> (points.size()))
        return;

    if (! isSelected (hoveredPoint))
        setSingleSelection (hoveredPoint);

    for (const auto index : selectedFreeformIndices)
        if (index >= 0 && index < static_cast<int> (points.size()))
        {
            auto& point = points[static_cast<size_t> (index)];
            const auto range = static_cast<double> (processor.getGlobalDbRange());
            point.db = juce::jlimit (-range, range, point.db + static_cast<double> (wheel.deltaY) * 4.0);
        }

    processor.setFreeformPoints (std::move (points));
    repaint();
}

class FlexCurveAudioProcessorEditor::BlendTab final : public juce::Component,
                                                      private juce::Timer
{
public:
    explicit BlendTab (FlexCurveAudioProcessor& p) : processor (p)
    {
        viewport.setViewedComponent (&rowsContent, false);
        viewport.setScrollBarsShown (true, false);
        styleScrollBar (viewport.getVerticalScrollBar());
        addAndMakeVisible (viewport);

        layerVisibilityLabel.setText ("Layer Visibility:", juce::dontSendNotification);
        layerVisibilityLabel.setColour (juce::Label::textColourId, inkColour());
        layerVisibilityLabel.setFont (juce::FontOptions (13.0f));
        addAndMakeVisible (layerVisibilityLabel);
        showEq.setButtonText ("EQ Layers");
        showTargets.setButtonText ("Targets");
        showRaw.setButtonText ("RAW");
        showTracking.setButtonText ("Tracking");
        showAverage.setButtonText ("Average");
        for (auto* toggle : { &showEq, &showTargets, &showRaw, &showTracking, &showAverage })
        {
            styleToggle (*toggle);
            addAndMakeVisible (*toggle);
        }
        showEq.setTooltip ("Show or hide audible EQ layers on the graph");
        showTargets.setTooltip ("Show or hide Target reference curves");
        showRaw.setTooltip ("Show or hide RAW measurement curves");
        showTracking.setTooltip ("Show or hide corrected-measurement curves that track each AutoEQ layer");
        showAverage.setTooltip ("Show or hide the EQ Average curve");
        showEq.onClick = [this]
        {
            processor.setLayerTypeVisible (FlexCurveLayerType::eq, showEq.getToggleState());
        };
        showTargets.onClick = [this]
        {
            processor.setLayerTypeVisible (FlexCurveLayerType::target, showTargets.getToggleState());
        };
        showRaw.onClick = [this]
        {
            processor.setLayerTypeVisible (FlexCurveLayerType::raw, showRaw.getToggleState());
        };
        showTracking.onClick = [this]
        {
            processor.setCorrectedMeasurementsVisible (showTracking.getToggleState());
        };
        showAverage.onClick = [this]
        {
            processor.setAverageVisible (showAverage.getToggleState());
        };

        reset.setButtonText ("Reset");
        styleButton (reset);
        reset.onClick = [this]
        {
            processor.resetActiveLayerBlendToFlat();
        };
        addAndMakeVisible (reset);

        blendModeLabel.setText ("|  Blend Apply Mode:", juce::dontSendNotification);
        blendModeLabel.setColour (juce::Label::textColourId, inkColour());
        blendModeLabel.setFont (juce::FontOptions (13.0f));
        addAndMakeVisible (blendModeLabel);
        perLayer.setButtonText ("Per Layer");
        globalAverage.setButtonText ("Global (All Curves)");
        styleToggle (perLayer);
        styleToggle (globalAverage);
        perLayer.onClick = [this] { processor.setBlendPerLayerMode (true); };
        globalAverage.onClick = [this] { processor.setBlendPerLayerMode (false); };
        addAndMakeVisible (perLayer);
        addAndMakeVisible (globalAverage);
        for (auto* button : { &importTarget, &importRaw, &normalizeReferences, &normalizeReferencesHelp,
                              &normalizeLayersToZero, &autoEq })
        {
            styleButton (*button);
            addAndMakeVisible (*button);
        }
        importTarget.setButtonText ("Import Target");
        importRaw.setButtonText ("Import RAW");
        normalizeReferences.setButtonText ("Normalize RAW + Target @");
        normalizeReferencesHelp.setButtonText ("?");
        normalizeLayersToZero.setButtonText ("Normalize Layers to 0 dB");
        autoEq.setButtonText ("Generate AutoEQ");
        importTarget.setTooltip ("Import a read-only Target reference curve");
        importRaw.setTooltip ("Import a read-only RAW measurement reference");
        normalizeReferences.setTooltip ("Apply reversible offsets so the selected RAW and Target both read 0 dB at the chosen frequency");
        normalizeReferencesHelp.setTooltip ("Why RAW and Target references may need manual alignment");
        normalizeLayersToZero.setTooltip ("Shift every EQ layer so its complete edited response peaks at 0 dB");
        autoEq.setTooltip ("Create an editable correction from Target minus RAW");
        normalizeFrequency.setText ("500", juce::dontSendNotification);
        normalizeFrequency.setInputRestrictions (7, "0123456789.");
        normalizeFrequency.setJustification (juce::Justification::centred);
        normalizeFrequency.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff0c0f13));
        normalizeFrequency.setColour (juce::TextEditor::textColourId, inkColour());
        normalizeFrequency.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff4a5664));
        normalizeFrequency.setTooltip ("Normalization frequency, from 20 to 20000 Hz");
        addAndMakeVisible (normalizeFrequency);
        normalizeHzLabel.setText ("Hz", juce::dontSendNotification);
        normalizeHzLabel.setColour (juce::Label::textColourId, mutedColour());
        normalizeHzLabel.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (normalizeHzLabel);
        auto clampNormalizeFrequency = [this]
        {
            const auto value = juce::jlimit (20.0, 20000.0, normalizeFrequency.getText().getDoubleValue());
            normalizeFrequency.setText (juce::String (value, value == std::round (value) ? 0 : 1),
                                        juce::dontSendNotification);
        };
        normalizeFrequency.onReturnKey = clampNormalizeFrequency;
        normalizeFrequency.onFocusLost = clampNormalizeFrequency;
        styleCombo (rawSource);
        styleCombo (targetDestination);
        styleCombo (autoEqMode);
        autoEqMode.addItem ("Auto", 1);
        autoEqMode.addItem ("Parametric", 2);
        autoEqMode.addItem ("Variable", 3);
        autoEqMode.setSelectedId (1);
        addAndMakeVisible (rawSource);
        addAndMakeVisible (targetDestination);
        addAndMakeVisible (autoEqMode);
        importTarget.onClick = [this] { openReferenceChooser (FlexCurveLayerType::target); };
        importRaw.onClick = [this] { openReferenceChooser (FlexCurveLayerType::raw); };
        normalizeReferences.onClick = [this]
        {
            const auto frequency = juce::jlimit (20.0, 20000.0,
                                                  normalizeFrequency.getText().getDoubleValue());
            normalizeFrequency.setText (juce::String (frequency, frequency == std::round (frequency) ? 0 : 1),
                                        juce::dontSendNotification);
            processor.normalizeReferencesAtFrequency (rawSource.getSelectedId(),
                                                       targetDestination.getSelectedId(),
                                                       frequency);
        };
        normalizeLayersToZero.onClick = [this] { processor.normalizeEqLayersToZeroDb(); };
        normalizeReferencesHelp.onClick = []
        {
            auto* window = new juce::AlertWindow (
                "RAW / Target Normalization",
                "RAW measurements and Target curves obtained from different sites may use different "
                "normalization references. If they do not align, use their layer offset sliders to "
                "match them visually to the original measurement graph before generating AutoEQ.\n\n"
                "The normalization frequency only aligns the selected RAW and Target at that point; "
                "it does not determine which source normalization is correct.\n\n"
                "Curve and measurement resources:\n"
                "squig.link - measurements and comparisons\n"
                "autoeq.app - headphone measurements and EQ curves\n"
                "usyless.uk/trace - extract curves from graph images",
                juce::AlertWindow::InfoIcon);
            window->addButton ("Close", 0);
            window->addButton ("squig.link", 1);
            window->addButton ("AutoEq", 2);
            window->addButton ("Trace", 3);
            window->enterModalState (
                true,
                juce::ModalCallbackFunction::create ([window] (int result)
                {
                    std::unique_ptr<juce::AlertWindow> windowToDelete (window);
                    if (result == 1)
                        juce::URL ("https://squig.link/").launchInDefaultBrowser();
                    else if (result == 2)
                        juce::URL ("https://autoeq.app/").launchInDefaultBrowser();
                    else if (result == 3)
                        juce::URL ("https://usyless.uk/trace/").launchInDefaultBrowser();
                }),
                false);
        };
        autoEq.onClick = [this]
        {
            const auto mode = autoEqMode.getSelectedId() == 2 ? FlexCurveAudioProcessor::AutoEqMode::parametric
                            : autoEqMode.getSelectedId() == 3 ? FlexCurveAudioProcessor::AutoEqMode::variable
                                                             : FlexCurveAudioProcessor::AutoEqMode::automatic;
            processor.generateAutoEq (rawSource.getSelectedId(), targetDestination.getSelectedId(), mode);
        };

        autoEqFeedback.setColour (juce::Label::textColourId, mutedColour());
        autoEqFeedback.setJustificationType (juce::Justification::centredLeft);
        autoEqFeedback.setFont (juce::FontOptions (12.0f));
        addAndMakeVisible (autoEqFeedback);

        for (auto* s : { &bass, &mid, &treble })
        {
            s->setSliderStyle (juce::Slider::LinearHorizontal);
            s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 22);
            const auto range = static_cast<double> (processor.getGlobalDbRange());
            s->setRange (-range, range, 0.1);
            s->setColour (juce::Slider::thumbColourId, accentColour());
            rowsContent.addAndMakeVisible (*s);
        }
        for (auto* s : { &lowMid, &midHigh })
        {
            s->setSliderStyle (juce::Slider::LinearHorizontal);
            s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 78, 22);
            s->setColour (juce::Slider::thumbColourId, accentColour());
            rowsContent.addAndMakeVisible (*s);
        }
        const std::array<std::pair<juce::Label*, juce::String>, 5> regionLabels {{
            { &bassLabel, "Bass gain" }, { &midLabel, "Mid gain" }, { &trebleLabel, "Treble gain" },
            { &lowMidLabel, "Bass / Mid crossover" }, { &midHighLabel, "Mid / Treble crossover" }
        }};
        for (const auto& [label, text] : regionLabels)
        {
            label->setText (text, juce::dontSendNotification);
            label->setColour (juce::Label::textColourId, mutedColour());
            label->setFont (juce::FontOptions (12.5f));
            label->setJustificationType (juce::Justification::centredLeft);
            rowsContent.addAndMakeVisible (*label);
        }
        lowMid.setRange (80.0, 1200.0, 1.0);
        midHigh.setRange (1200.0, 12000.0, 1.0);
        bass.setTextValueSuffix (" dB");
        mid.setTextValueSuffix (" dB");
        treble.setTextValueSuffix (" dB");
        lowMid.setTextValueSuffix (" Hz");
        midHigh.setTextValueSuffix (" Hz");
        installSliderReset (bass, 0.0);
        installSliderReset (mid, 0.0);
        installSliderReset (treble, 0.0);
        installSliderReset (lowMid, 250.0);
        installSliderReset (midHigh, 4000.0);

        auto changed = [this]
        {
            processor.setRegionSettings (static_cast<float> (bass.getValue()),
                                         static_cast<float> (mid.getValue()),
                                         static_cast<float> (treble.getValue()),
                                         static_cast<float> (lowMid.getValue()),
                                         static_cast<float> (midHigh.getValue()));
        };
        bass.onValueChange = changed;
        mid.onValueChange = changed;
        treble.onValueChange = changed;
        lowMid.onValueChange = changed;
        midHigh.onValueChange = changed;
        rebuildRows();
        startTimerHz (10);
    }

    ~BlendTab() override
    {
        stopTimer();
        exportChooser.reset();
        importChooser.reset();
        viewport.setViewedComponent (nullptr, false);
        rowsContent.removeAllChildren();
    }

    void paint (juce::Graphics& g) override
    {
        juce::ignoreUnused (g);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12);
        auto visibilityRow = area.removeFromTop (32);
        layerVisibilityLabel.setBounds (visibilityRow.removeFromLeft (116));
        showEq.setBounds (visibilityRow.removeFromLeft (106));
        showTargets.setBounds (visibilityRow.removeFromLeft (92));
        showRaw.setBounds (visibilityRow.removeFromLeft (72));
        showTracking.setBounds (visibilityRow.removeFromLeft (100));
        showAverage.setBounds (visibilityRow.removeFromLeft (96));
        visibilityRow.removeFromLeft (26);
        blendModeLabel.setBounds (visibilityRow.removeFromLeft (158));
        perLayer.setBounds (visibilityRow.removeFromLeft (100));
        globalAverage.setBounds (visibilityRow.removeFromLeft (178));

        area.removeFromTop (6);
        auto importRow = area.removeFromTop (34);
        importTarget.setBounds (importRow.removeFromLeft (150));
        importRow.removeFromLeft (10);
        importRaw.setBounds (importRow.removeFromLeft (150));
        importRow.removeFromLeft (16);
        normalizeReferences.setBounds (importRow.removeFromLeft (218));
        importRow.removeFromLeft (6);
        normalizeFrequency.setBounds (importRow.removeFromLeft (72));
        normalizeHzLabel.setBounds (importRow.removeFromLeft (30));
        normalizeReferencesHelp.setBounds (importRow.removeFromLeft (34));
        importRow.removeFromLeft (12);
        normalizeLayersToZero.setBounds (importRow.removeFromLeft (210));
        reset.setBounds (importRow.removeFromRight (90));

        area.removeFromTop (6);
        auto selectorRow = area.removeFromTop (34);
        const auto selectorWidth = juce::jmax (190, (selectorRow.getWidth() - 410) / 2);
        rawSource.setBounds (selectorRow.removeFromLeft (selectorWidth));
        selectorRow.removeFromLeft (10);
        targetDestination.setBounds (selectorRow.removeFromLeft (selectorWidth));
        selectorRow.removeFromLeft (10);
        autoEqMode.setBounds (selectorRow.removeFromLeft (120));
        selectorRow.removeFromLeft (10);
        autoEq.setBounds (selectorRow.removeFromLeft (150));

        autoEqFeedback.setBounds (area.removeFromTop (30).reduced (2, 0));
        area.removeFromTop (6);
        layerArea = area;
        viewport.setBounds (layerArea);

        const auto rowHeight = 42;
        const auto layerRowsHeight = rowHeight * (static_cast<int> (rows.size()) + (averageRow != nullptr ? 1 : 0));
        const auto controlsHeight = 5 * 42 + 22;
        auto rowsArea = juce::Rectangle<int> (0, 0, juce::jmax (300, layerArea.getWidth() - 16),
                                               juce::jmax (layerArea.getHeight(), layerRowsHeight + controlsHeight + 14));
        rowsContent.setBounds (rowsArea);
        rowsArea = rowsContent.getLocalBounds().reduced (2, 4);
        for (int i = 0; i < static_cast<int> (rows.size()); ++i)
            rows[static_cast<size_t> (i)]->setBounds (rowsArea.removeFromTop (rowHeight).reduced (0, 2));
        if (averageRow != nullptr)
            averageRow->setBounds (rowsArea.removeFromTop (rowHeight).reduced (0, 2));

        rowsArea.removeFromTop (10);
        auto placeRegion = [&rowsArea] (juce::Label& label, juce::Slider& slider)
        {
            auto row = rowsArea.removeFromTop (42).reduced (8, 4);
            label.setBounds (row.removeFromLeft (170));
            slider.setBounds (row);
        };
        placeRegion (bassLabel, bass);
        placeRegion (midLabel, mid);
        placeRegion (trebleLabel, treble);
        placeRegion (lowMidLabel, lowMid);
        placeRegion (midHighLabel, midHigh);
    }

private:
    struct LayerRow final : juce::Component
    {
        LayerRow (FlexCurveAudioProcessor& p, int layerIdIn, bool isAverageIn = false)
            : processor (p), layerId (layerIdIn), isAverage (isAverageIn)
        {
            name.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff0c0f13));
            name.setColour (juce::TextEditor::textColourId, inkColour());
            name.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff34404d));
            name.onReturnKey = [this] { pushName(); };
            name.onFocusLost = [this] { pushName(); };
            addAndMakeVisible (name);

            mute.setButtonText ("M");
            solo.setButtonText ("S");
            visible.setButtonText ("V");
            styleToggle (mute);
            styleToggle (solo);
            styleToggle (visible);
            addAndMakeVisible (mute);
            addAndMakeVisible (solo);
            addAndMakeVisible (visible);
            channel.addItem ("L+R", 1);
            channel.addItem ("L", 2);
            channel.addItem ("R", 3);
            styleCombo (channel);
            channel.setTooltip ("Choose which channel this layer edits");
            channel.onChange = [this]
            {
                if (! isAverage && channel.getSelectedId() > 0)
                    processor.setLayerChannelSelection (
                        layerId,
                        static_cast<FlexChannelSelection> (channel.getSelectedId() - 1));
            };
            addAndMakeVisible (channel);
            linkChannels.setButtonText ("Link");
            styleButton (linkChannels);
            linkChannels.setTooltip (
                "Link L/R explicitly. R is copied to L when R is selected; otherwise L is copied to R.");
            linkChannels.onClick = [this] { processor.linkLayerChannels (layerId); };
            addAndMakeVisible (linkChannels);
            gain.setSliderStyle (juce::Slider::LinearHorizontal);
            gain.setTextBoxStyle (juce::Slider::TextBoxRight, false, 74, 22);
            const auto range = static_cast<double> (processor.getGlobalDbRange());
            gain.setRange (-range, range, 0.1);
            gain.setTextValueSuffix (" dB");
            gain.setColour (juce::Slider::thumbColourId, accentColour());
            gain.setColour (juce::Slider::trackColourId, juce::Colour (0xff33404c));
            addAndMakeVisible (gain);
            installSliderReset (gain, 0.0);
            balance.setSliderStyle (juce::Slider::LinearHorizontal);
            balance.setTextBoxStyle (juce::Slider::TextBoxRight, false, 72, 22);
            balance.setRange (-100.0, 100.0, 1.0);
            balance.setTextValueSuffix ("");
            balance.setColour (juce::Slider::thumbColourId, inkColour());
            balance.setColour (juce::Slider::trackColourId, juce::Colour (0xff33404c));
            balance.setTooltip ("Layer balance trim: -100 L, 0 center, +100 R");
            addAndMakeVisible (balance);
            installSliderReset (balance, 0.0);
            remove.setButtonText ("x");
            styleButton (remove);
            addAndMakeVisible (remove);
            reset.setButtonText ("Reset");
            styleButton (reset);
            addAndMakeVisible (reset);
            invert.setButtonText ("Invert");
            styleButton (invert);
            addAndMakeVisible (invert);
            exportCurve.setButtonText ("Export");
            styleButton (exportCurve);
            addAndMakeVisible (exportCurve);
            clone.setButtonText ("Clone");
            styleButton (clone);
            clone.setTooltip ("Clone this layer with all curve data, EQ, Blend, and layer state");
            addAndMakeVisible (clone);
            smooth.setButtonText ("Smooth");
            smooth.setClickingTogglesState (true);
            styleToggle (smooth);
            smooth.setTooltip ("Smooth this layer non-destructively");
            addAndMakeVisible (smooth);

            mute.onClick = [this]
            {
                if (! isAverage)
                {
                    processor.setActiveLayerId (layerId);
                    processor.setLayerMuted (layerId, mute.getToggleState());
                }
            };
            solo.onClick = [this]
            {
                if (! isAverage)
                {
                    processor.setActiveLayerId (layerId);
                    processor.setLayerSolo (layerId, solo.getToggleState());
                }
            };
            visible.onClick = [this]
            {
                if (isAverage) processor.setAverageVisible (visible.getToggleState());
                else processor.setLayerVisible (layerId, visible.getToggleState());
            };
            gain.onValueChange = [this]
            {
                if (! isAverage)
                {
                    processor.setActiveLayerId (layerId);
                    processor.setLayerGain (layerId, static_cast<float> (gain.getValue()));
                }
            };
            balance.onValueChange = [this]
            {
                if (! isAverage)
                {
                    processor.setActiveLayerId (layerId);
                    processor.setLayerBalance (layerId, panPercentToBalanceDb (static_cast<float> (balance.getValue())));
                }
            };
            remove.onClick = [this]
            {
                if (isAverage) processor.setAverageEnabled (false);
                else processor.removeLayer (layerId);
                if (onChanged != nullptr)
                    onChanged();
            };
            reset.onClick = [this]
            {
                if (! isAverage)
                {
                    processor.setActiveLayerId (layerId);
                    if (layerType == FlexCurveLayerType::eq)
                        processor.resetLayerToFlat (layerId);
                    else
                        processor.resetLayerEdits (layerId);
                }
            };
            invert.onClick = [this]
            {
                if (! isAverage)
                {
                    processor.setActiveLayerId (layerId);
                    processor.toggleLayerInverted (layerId);
                }
            };
            exportCurve.onClick = [this]
            {
                if (onExport != nullptr)
                    onExport (layerId, isAverage, name.getText());
            };
            clone.onClick = [this]
            {
                if (! isAverage && processor.cloneLayer (layerId) && onChanged != nullptr)
                    onChanged();
            };
            smooth.onClick = [this]
            {
                if (! isAverage)
                    processor.setLayerSourceSmoothing (layerId, smooth.getToggleState());
            };
            sync();
        }

        void sync()
        {
            const auto editable = ! processor.isEditLocked();
            if (isAverage)
            {
                colour = juce::Colours::white;
                if (! name.hasKeyboardFocus (true))
                    name.setText ("Average", false);
                name.setReadOnly (true);
                gain.setEnabled (false);
                balance.setEnabled (false);
                balance.setVisible (false);
                reset.setEnabled (false);
                reset.setVisible (false);
                invert.setVisible (false);
                remove.setVisible (false);
                mute.setVisible (false);
                solo.setVisible (false);
                visible.setToggleState (processor.isAverageVisible(), juce::dontSendNotification);
                visible.setEnabled (true);
                exportCurve.setEnabled (true);
                clone.setVisible (false);
                smooth.setVisible (false);
                channel.setVisible (false);
                linkChannels.setVisible (false);
                return;
            }

            const auto layers = processor.getLayers();
            const auto it = std::find_if (layers.begin(), layers.end(), [this] (const auto& l) { return l.id == layerId; });
            if (it == layers.end())
                return;
            colour = it->colour;
            layerType = it->type;
            if (! name.hasKeyboardFocus (true))
                name.setText (it->name, false);
            name.setTooltip (it->sourceFile != juce::File{} ? it->sourceFile.getFullPathName() : it->name);
            mute.setToggleState (it->muted, juce::dontSendNotification);
            solo.setToggleState (it->solo, juce::dontSendNotification);
            visible.setToggleState (it->visible, juce::dontSendNotification);
            smooth.setToggleState (
                it->selectedChannel == FlexChannelSelection::right && ! it->channelsLinked
                    ? it->right.smoothSourceCurve : it->smoothSourceCurve,
                juce::dontSendNotification);
            channel.setVisible (true);
            channel.setSelectedId (static_cast<int> (it->selectedChannel) + 1,
                                   juce::dontSendNotification);
            linkChannels.setVisible (true);
            linkChannels.setButtonText (it->channelsLinked ? "Linked" : "Link");
            if (! gain.isMouseButtonDown())
            {
                const auto range = processor.getGlobalDbRange();
                gain.setRange (-range, range, 0.1);
                const auto channelGain = it->selectedChannel == FlexChannelSelection::right
                                      && ! it->channelsLinked
                    ? it->right.gainDb : it->gainDb;
                gain.setValue (channelGain, juce::dontSendNotification);
            }
            if (! balance.isMouseButtonDown())
                balance.setValue (balanceDbToPanPercent (it->balanceDb), juce::dontSendNotification);
            const auto isEq = layerType == FlexCurveLayerType::eq;
            mute.setVisible (isEq);
            solo.setVisible (isEq);
            gain.setVisible (true);
            balance.setVisible (isEq);
            invert.setVisible (isEq);
            reset.setVisible (true);
            invert.setToggleState (it->inverted, juce::dontSendNotification);
            gain.setTooltip (isEq ? "Layer gain: affects preview audio, render, and export"
                                  : layerType == FlexCurveLayerType::target
                                      ? "Target offset: affects its graph position and AutoEQ comparison"
                                      : "RAW measurement offset: affects its graph position and AutoEQ comparison");
            name.setEnabled (editable);
            mute.setEnabled (editable && isEq);
            solo.setEnabled (editable && isEq);
            gain.setEnabled (editable);
            balance.setEnabled (editable && isEq);
            reset.setEnabled (editable);
            invert.setEnabled (editable && isEq);
            remove.setEnabled (editable);
            clone.setVisible (true);
            clone.setEnabled (editable && processor.canAddLayerType (layerType));
            smooth.setVisible (true);
            smooth.setEnabled (editable);
            channel.setEnabled (true);
            linkChannels.setEnabled (editable && ! it->channelsLinked);
            visible.setEnabled (true);
            exportCurve.setEnabled (true);
        }

        void pushName()
        {
            if (! isAverage)
                processor.renameLayer (layerId, name.getText());
        }

        void mouseDown (const juce::MouseEvent&) override
        {
            processor.setActiveLayerId (isAverage ? -1 : layerId);
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            const auto active = processor.getActiveLayerId() == (isAverage ? -1 : layerId);
            auto b = getLocalBounds().toFloat();
            g.setColour (active ? juce::Colour (0xff202832) : surfaceColour());
            g.fillRoundedRectangle (b, 5.0f);
            g.setColour (colour);
            g.fillEllipse (b.getX() + 8.0f, b.getCentreY() - 5.0f, 10.0f, 10.0f);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (6, 4);
            area.removeFromLeft (18);
            const auto isEq = ! isAverage && layerType == FlexCurveLayerType::eq;

            const auto actionWidth = (isEq ? 62 : 0)
                                   + (! isAverage ? 62 : 0)
                                   + 70
                                   + (! isAverage ? 62 : 0)
                                   + (! isAverage ? 82 : 0)
                                   + (! isAverage ? 36 : 0);
            auto actions = area.removeFromRight (juce::jmin (actionWidth, area.getWidth() / 2));

            visible.setBounds (area.removeFromLeft (34));
            if (isEq)
            {
                mute.setBounds (area.removeFromLeft (38));
                solo.setBounds (area.removeFromLeft (38));
            }
            if (! isAverage)
            {
                channel.setBounds (area.removeFromRight (118).reduced (2, 0));
                area.removeFromRight (4);
                linkChannels.setBounds (area.removeFromRight (82).reduced (2, 0));
                area.removeFromRight (6);
            }

            const auto nameWidth = isAverage ? 190 : juce::jlimit (180, 260, area.getWidth() / 3);
            name.setBounds (area.removeFromLeft (nameWidth));
            area.removeFromLeft (8);
            if (isAverage || ! isEq)
                gain.setBounds (area);
            else
            {
                auto gainArea = area.removeFromLeft (juce::jmax (230, area.getWidth() / 2));
                gain.setBounds (gainArea.reduced (2, 0));
                balance.setBounds (area.reduced (8, 0));
            }
            if (isEq)
                invert.setBounds (actions.removeFromLeft (62).reduced (2, 0));
            if (! isAverage)
                clone.setBounds (actions.removeFromLeft (62).reduced (2, 0));
            exportCurve.setBounds (actions.removeFromLeft (70).reduced (2, 0));
            if (! isAverage)
                reset.setBounds (actions.removeFromLeft (62).reduced (2, 0));
            if (! isAverage)
                smooth.setBounds (actions.removeFromLeft (82).reduced (2, 0));
            if (! isAverage)
                remove.setBounds (actions.removeFromRight (32));
        }

        FlexCurveAudioProcessor& processor;
        int layerId;
        bool isAverage;
        FlexCurveLayerType layerType = FlexCurveLayerType::eq;
        juce::Colour colour;
        juce::TextEditor name;
        juce::ToggleButton visible, mute, solo;
        juce::ComboBox channel;
        FlexCurveResettableSlider gain, balance;
        juce::TextButton reset, invert, clone, exportCurve, remove, linkChannels;
        juce::ToggleButton smooth;
        std::function<void()> onChanged;
        std::function<void(int, bool, juce::String)> onExport;
    };

    void rebuildRows()
    {
        rows.clear();
        averageRow.reset();
        for (const auto& layer : processor.getLayers())
        {
            auto row = std::make_unique<LayerRow> (processor, layer.id);
            row->onExport = [this] (int id, bool average, const juce::String& name)
            {
                openLayerExportMenu (id, average, name);
            };
            row->onChanged = [this] { rebuildRows(); };
            rowsContent.addAndMakeVisible (*row);
            rows.push_back (std::move (row));
        }
        averageRow = std::make_unique<LayerRow> (processor, -1, true);
        averageRow->onExport = [this] (int id, bool average, const juce::String& name)
        {
            openLayerExportMenu (id, average, name);
        };
        rowsContent.addAndMakeVisible (*averageRow);
        resized();
    }

    void openLayerExportMenu (int layerId, bool average, juce::String layerName)
    {
        juce::PopupMenu menu;
        menu.addItem (1, "FIR WAV (current phase)");
        menu.addItem (2, "Frequency / dB TXT");
        menu.addItem (3, "Frequency / dB CSV");
        menu.addItem (4, "GraphicEQ / APO TXT");
        menu.addItem (5, "Melda CSV");
        menu.addItem (6, "APO Parametric TXT");

        const juce::Component::SafePointer<BlendTab> safeThis (this);
        menu.showMenuAsync ({}, [safeThis, layerId, average, layerName = std::move (layerName)] (int result)
        {
            if (safeThis == nullptr || result <= 0)
                return;

            auto format = FlexCurveAudioProcessor::CurveExportFormat::frequencyText;
            juce::String extension = ".txt";
            switch (result)
            {
                case 1: format = FlexCurveAudioProcessor::CurveExportFormat::firWav; extension = ".wav"; break;
                case 2: format = FlexCurveAudioProcessor::CurveExportFormat::frequencyText; break;
                case 3: format = FlexCurveAudioProcessor::CurveExportFormat::frequencyCsv; extension = ".csv"; break;
                case 4: format = FlexCurveAudioProcessor::CurveExportFormat::graphicEq; break;
                case 5: format = FlexCurveAudioProcessor::CurveExportFormat::meldaCsv; extension = ".csv"; break;
                case 6: format = FlexCurveAudioProcessor::CurveExportFormat::apoParametric; break;
                default: return;
            }

            auto safeName = layerName.isNotEmpty() ? layerName : (average ? "Average" : "FlexCurve_Layer");
            safeName = safeName.replaceCharacters ("\\/:*?\"<>|", "_________");
            safeThis->exportChooser = std::make_unique<juce::FileChooser> (
                "Export " + safeName,
                juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile (safeName + extension),
                "*" + extension);
            safeThis->exportChooser->launchAsync (
                juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                    | juce::FileBrowserComponent::warnAboutOverwriting,
                [safeThis, layerId, average, format, extension] (const juce::FileChooser& chooser)
                {
                    if (safeThis == nullptr)
                        return;
                    auto file = chooser.getResult();
                    if (file == juce::File{})
                        return;
                    if (! file.hasFileExtension (extension.substring (1)))
                        file = file.withFileExtension (extension.substring (1));
                    if (! safeThis->processor.exportLayerToFile (layerId, average, format, file))
                        juce::AlertWindow::showMessageBoxAsync (
                            juce::AlertWindow::WarningIcon, "Export Curve",
                            "This layer could not be exported in the selected format.");
                });
        });
    }

    void timerCallback() override
    {
        syncAutoEqSelectors();
        const auto range = static_cast<double> (processor.getGlobalDbRange());
        for (auto* s : { &bass, &mid, &treble })
            s->setRange (-range, range, 0.1);
        float b, m, t, lm, mh;
        processor.getRegionSettings (b, m, t, lm, mh);
        if (! bass.isMouseButtonDown()) bass.setValue (b, juce::dontSendNotification);
        if (! mid.isMouseButtonDown()) mid.setValue (m, juce::dontSendNotification);
        if (! treble.isMouseButtonDown()) treble.setValue (t, juce::dontSendNotification);
        if (! lowMid.isMouseButtonDown()) lowMid.setValue (lm, juce::dontSendNotification);
        if (! midHigh.isMouseButtonDown()) midHigh.setValue (mh, juce::dontSendNotification);
        const auto count = static_cast<int> (processor.getLayers().size()) + 1;
        if (count != static_cast<int> (rows.size()) + (averageRow != nullptr ? 1 : 0))
            rebuildRows();
        for (auto& row : rows)
            row->sync();
        if (averageRow != nullptr)
            averageRow->sync();
        const auto perLayerMode = processor.isBlendPerLayerMode();
        perLayer.setToggleState (perLayerMode, juce::dontSendNotification);
        globalAverage.setToggleState (! perLayerMode, juce::dontSendNotification);
        showEq.setToggleState (processor.isLayerTypeVisible (FlexCurveLayerType::eq), juce::dontSendNotification);
        showTargets.setToggleState (processor.isLayerTypeVisible (FlexCurveLayerType::target), juce::dontSendNotification);
        showRaw.setToggleState (processor.isLayerTypeVisible (FlexCurveLayerType::raw), juce::dontSendNotification);
        showTracking.setToggleState (processor.areCorrectedMeasurementsVisible(), juce::dontSendNotification);
        showAverage.setToggleState (processor.isAverageVisible(), juce::dontSendNotification);
        const auto hasEditableLayer = processor.hasEditableActiveLayer();
        const auto controlsEnabled = (! perLayerMode || hasEditableLayer) && ! processor.isEditLocked();
        for (auto* s : { &bass, &mid, &treble, &lowMid, &midHigh })
            s->setEnabled (controlsEnabled);
        reset.setEnabled (controlsEnabled);
        const auto locked = processor.isEditLocked();
        importTarget.setEnabled (! locked && processor.canAddLayerType (FlexCurveLayerType::target));
        importRaw.setEnabled (! locked && processor.canAddLayerType (FlexCurveLayerType::raw));
        autoEq.setEnabled (! locked && processor.canAddUserLayer()
                           && rawSource.getSelectedId() > 0 && targetDestination.getSelectedId() > 0);
        normalizeReferences.setEnabled (! locked
                                        && rawSource.getSelectedId() > 0
                                        && targetDestination.getSelectedId() > 0);
        normalizeFrequency.setEnabled (normalizeReferences.isEnabled());
        const auto layers = processor.getLayers();
        normalizeLayersToZero.setEnabled (! locked
            && std::any_of (layers.begin(), layers.end(), [] (const auto& layer)
            {
                return layer.type == FlexCurveLayerType::eq;
            }));
        const auto active = processor.getActiveLayerId();
        const auto activeIt = std::find_if (layers.begin(), layers.end(), [active] (const auto& layer)
        {
            return layer.id == active && layer.autoEqRawLayerId > 0 && layer.autoEqTargetLayerId > 0;
        });
        autoEqFeedback.setText (activeIt != layers.end()
            ? juce::String (activeIt->autoEqSourcesOutdated ? "RAW/Target changed - regenerate AutoEQ.  " : "")
                + "Corrected RMS error: " + juce::String (processor.getAutoEqResidualRmsDb (active), 2) + " dB"
            : "Select RAW + Target, normalize if needed, then Generate AutoEQ.",
            juce::dontSendNotification);
        repaint();
    }

    void openReferenceChooser (FlexCurveLayerType type)
    {
        importChooser = std::make_unique<juce::FileChooser> (
            type == FlexCurveLayerType::target ? "Import Target Curve" : "Import RAW Measurement",
            juce::File(),
            "*.txt;*.csv;*.wav");
        const juce::Component::SafePointer<BlendTab> safeThis (this);
        importChooser->launchAsync (juce::FileBrowserComponent::openMode
                                        | juce::FileBrowserComponent::canSelectFiles
                                        | juce::FileBrowserComponent::canSelectMultipleItems,
                                    [safeThis, type] (const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr)
                return;
            for (const auto& file : chooser.getResults())
                if (safeThis->processor.canAddLayerType (type))
                    safeThis->processor.addReferenceCurveFile (file, type);
            safeThis->rebuildRows();
            safeThis->syncAutoEqSelectors (true);
        });
    }

    void syncAutoEqSelectors (bool force = false)
    {
        const auto layers = processor.getLayers();
        const auto rawCount = std::count_if (layers.begin(), layers.end(), [] (const auto& layer)
        {
            return layer.type == FlexCurveLayerType::raw;
        });
        const auto targetCount = std::count_if (layers.begin(), layers.end(), [] (const auto& layer)
        {
            return layer.type == FlexCurveLayerType::target;
        });
        if (! force && rawSource.getNumItems() == rawCount && targetDestination.getNumItems() == targetCount)
        {
            const auto rawLayer = std::find_if (layers.begin(), layers.end(), [this] (const auto& layer)
            {
                return layer.id == rawSource.getSelectedId();
            });
            const auto targetLayer = std::find_if (layers.begin(), layers.end(), [this] (const auto& layer)
            {
                return layer.id == targetDestination.getSelectedId();
            });
            rawSource.setTooltip (rawLayer != layers.end() && rawLayer->sourceFile != juce::File{}
                ? rawLayer->sourceFile.getFullPathName() : rawSource.getText());
            targetDestination.setTooltip (targetLayer != layers.end() && targetLayer->sourceFile != juce::File{}
                ? targetLayer->sourceFile.getFullPathName() : targetDestination.getText());
            return;
        }

        const auto previousRaw = rawSource.getSelectedId();
        const auto previousTarget = targetDestination.getSelectedId();
        rawSource.clear (juce::dontSendNotification);
        targetDestination.clear (juce::dontSendNotification);
        for (const auto& layer : layers)
        {
            if (layer.type == FlexCurveLayerType::raw)
                rawSource.addItem ("RAW: " + layer.name, layer.id);
            else if (layer.type == FlexCurveLayerType::target)
                targetDestination.addItem ("Target: " + layer.name, layer.id);
        }
        rawSource.setSelectedId (previousRaw > 0 ? previousRaw : rawSource.getItemId (0), juce::dontSendNotification);
        targetDestination.setSelectedId (previousTarget > 0 ? previousTarget : targetDestination.getItemId (0),
                                         juce::dontSendNotification);
        const auto rawLayer = std::find_if (layers.begin(), layers.end(), [this] (const auto& layer)
        {
            return layer.id == rawSource.getSelectedId();
        });
        const auto targetLayer = std::find_if (layers.begin(), layers.end(), [this] (const auto& layer)
        {
            return layer.id == targetDestination.getSelectedId();
        });
        rawSource.setTooltip (rawLayer != layers.end() && rawLayer->sourceFile != juce::File{}
            ? rawLayer->sourceFile.getFullPathName() : rawSource.getText());
        targetDestination.setTooltip (targetLayer != layers.end() && targetLayer->sourceFile != juce::File{}
            ? targetLayer->sourceFile.getFullPathName() : targetDestination.getText());
    }

    FlexCurveAudioProcessor& processor;
    juce::Rectangle<int> layerArea;
    FlexCurveResettableSlider bass, mid, treble, lowMid, midHigh;
    juce::Label bassLabel, midLabel, trebleLabel, lowMidLabel, midHighLabel;
    juce::Label layerVisibilityLabel;
    juce::Viewport viewport;
    juce::Component rowsContent;
    juce::TextButton reset;
    juce::TextButton importTarget, importRaw, normalizeReferences, normalizeReferencesHelp;
    juce::TextButton normalizeLayersToZero, autoEq;
    juce::TextEditor normalizeFrequency;
    juce::Label normalizeHzLabel, autoEqFeedback;
    juce::ComboBox rawSource, targetDestination, autoEqMode;
    juce::Label blendModeLabel;
    juce::ToggleButton perLayer, globalAverage;
    juce::ToggleButton showEq, showTargets, showRaw, showTracking, showAverage;
    std::vector<std::unique_ptr<LayerRow>> rows;
    std::unique_ptr<LayerRow> averageRow;
    std::unique_ptr<juce::FileChooser> exportChooser;
    std::unique_ptr<juce::FileChooser> importChooser;
};

class FlexCurveAudioProcessorEditor::GraphicTab final : public juce::Component,
                                                       private juce::Timer
{
public:
    explicit GraphicTab (FlexCurveAudioProcessor& p, FlexCurveGraph& g) : processor (p), graph (g)
    {
        enabled.setButtonText ("Graphic EQ");
        styleToggle (enabled);
        enabled.onClick = [this] { processor.setGraphicEnabled (enabled.getToggleState()); };
        addAndMakeVisible (enabled);

        smooth.setButtonText ("Smooth Curve");
        preserveVariable.setButtonText ("Preserve Variable Shape");
        styleToggle (smooth);
        styleToggle (preserveVariable);
        smooth.onClick = [this] { processor.setGraphicSmoothing (smooth.getToggleState()); };
        preserveVariable.onClick = [this]
        {
            processor.setPreserveVariableShapeAcrossModes (preserveVariable.getToggleState());
        };
        smooth.setTooltip ("Smooth the active layer's Graphic/Variable EQ geometry");
        preserveVariable.setTooltip ("Keep stored Variable points active when switching to 15/31-band mode");
        addAndMakeVisible (smooth);
        addAndMakeVisible (preserveVariable);

        mode.addItem ("31 bands", 1);
        mode.addItem ("15 bands", 2);
        mode.addItem ("Variable", 3);
        styleCombo (mode);
        mode.setSelectedId (processor.getGraphicMode() == 15 ? 2 : (processor.getGraphicMode() == 0 ? 3 : 1));
        mode.onChange = [this]
        {
            processor.setGraphicMode (mode.getSelectedId() == 3 ? 0 : (mode.getSelectedId() == 2 ? 15 : 31));
            rebuildControls();
        };
        addAndMakeVisible (mode);

        addPoint.setButtonText ("Add Point");
        styleButton (addPoint);
        addPoint.onClick = [this]
        {
            auto points = processor.getFreeformPoints();
            points.push_back ({ 1000.0, 0.0 });
            processor.setFreeformPoints (std::move (points));
            rebuildPointRows();
        };
        addAndMakeVisible (addPoint);

        reset.setButtonText ("Reset");
        styleButton (reset);
        reset.onClick = [this]
        {
            processor.resetGraphic();
            syncSliders();
            rebuildPointRows();
        };
        addAndMakeVisible (reset);

        copyEq.setButtonText ("Copy EQ");
        pasteEq.setButtonText ("Paste EQ");
        pasteInvertedEq.setButtonText ("Paste Inverted EQ");
        styleButton (copyEq);
        styleButton (pasteEq);
        styleButton (pasteInvertedEq);
        copyEq.onClick = [this] { processor.copyCurrentGraphicEq(); };
        pasteEq.onClick = [this]
        {
            processor.pasteCurrentGraphicEq();
            rebuildControls();
        };
        pasteInvertedEq.onClick = [this]
        {
            processor.pasteCurrentGraphicEq (true);
            rebuildControls();
        };
        addAndMakeVisible (copyEq);
        addAndMakeVisible (pasteEq);
        addAndMakeVisible (pasteInvertedEq);

        pointViewport.setViewedComponent (&pointRowsContent, false);
        pointViewport.setScrollBarsShown (true, false);
        styleScrollBar (pointViewport.getVerticalScrollBar());
        addAndMakeVisible (pointViewport);

        hint.setColour (juce::Label::textColourId, mutedColour());
        hint.setFont (juce::FontOptions (15.0f));
        hint.setJustificationType (juce::Justification::centred);
        hint.setText ({}, juce::dontSendNotification);

        rebuildControls();
        startTimerHz (10);
    }

    ~GraphicTab() override
    {
        stopTimer();
        pointViewport.setViewedComponent (nullptr, false);
        pointRowsContent.removeAllChildren();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (10);
        auto header = area.removeFromTop (32);
        enabled.setBounds (header.removeFromLeft (160));
        mode.setBounds (header.removeFromLeft (120));
        addPoint.setVisible (mode.getSelectedId() == 3);
        addPoint.setBounds (header.removeFromLeft (100));
        reset.setBounds (header.removeFromLeft (80));
        smooth.setBounds (header.removeFromLeft (130));
        preserveVariable.setBounds (header.removeFromLeft (180));
        copyEq.setBounds (header.removeFromLeft (170).reduced (4, 0));
        pasteEq.setBounds (header.removeFromLeft (130).reduced (4, 0));
        pasteInvertedEq.setBounds (header.removeFromLeft (190).reduced (4, 0));
        area.removeFromTop (10);
        hint.setBounds (area);

        if (! processor.hasEditableActiveLayer())
            return;

        if (mode.getSelectedId() == 3)
        {
            auto list = area.removeFromRight (260);
            pointHeader.setBounds (list.removeFromTop (22));
            pointViewport.setBounds (list);
            pointRowsContent.setBounds (0, 0, juce::jmax (220, list.getWidth() - 12),
                                        juce::jmax (list.getHeight(), static_cast<int> (pointRows.size()) * 28));
            auto rowsArea = pointRowsContent.getLocalBounds();
            for (auto& row : pointRows)
                row->setBounds (rowsArea.removeFromTop (28).reduced (0, 2));
            return;
        }
        pointViewport.setVisible (false);

        const auto count = processor.getGraphicBandCount();
        const auto width = juce::jmax (18, area.getWidth() / juce::jmax (1, count));
        auto sliderArea = area;
        auto labelArea = sliderArea.removeFromBottom (22);
        for (int i = 0; i < static_cast<int> (sliders.size()); ++i)
        {
            sliders[static_cast<size_t> (i)]->setBounds (sliderArea.getX() + i * width, sliderArea.getY(), width - 2, sliderArea.getHeight());
            frequencyLabels[static_cast<size_t> (i)]->setBounds (labelArea.getX() + i * width, labelArea.getY(), width - 2, labelArea.getHeight());
        }
    }

    void paint (juce::Graphics& g) override
    {
        if (mode.getSelectedId() != 3)
            return;
        if (! processor.hasEditableActiveLayer())
            return;

        auto area = getLocalBounds().reduced (10);
        area.removeFromTop (54);
        area.removeFromRight (280);
        g.setColour (mutedColour().withAlpha (0.82f));
        g.setFont (juce::FontOptions (15.0f));
        g.drawFittedText ("Variable mode: double click the graph to add points. Hold Ctrl and click-drag to paint the curve. Drag points to shape it. Shift-click or drag empty space to select multiple points. Delete removes the selection. Arrow keys and mouse wheel adjust gain.",
                          area.reduced (14), juce::Justification::topLeft, 5);
    }

private:
    struct PointRow final : juce::Component
    {
        PointRow (FlexCurveAudioProcessor& p, FlexCurveGraph& g, int idx) : processor (p), graph (g), index (idx)
        {
            freq.setInputRestrictions (0, "0123456789.");
            gain.setInputRestrictions (0, "-0123456789.");
            for (auto* e : { &freq, &gain })
            {
                e->setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff0c0f13));
                e->setColour (juce::TextEditor::textColourId, inkColour());
                e->setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff34404d));
                addAndMakeVisible (*e);
            }
            del.setButtonText ("x");
            styleButton (del);
            addAndMakeVisible (del);
            sync();
            freq.onReturnKey = [this] { push(); };
            gain.onReturnKey = [this] { push(); };
            freq.onFocusLost = [this] { push(); };
            gain.onFocusLost = [this] { push(); };
            del.onClick = [this]
            {
                auto points = processor.getFreeformPoints();
                if (index >= 0 && index < static_cast<int> (points.size()))
                {
                    points.erase (points.begin() + index);
                    processor.setFreeformPoints (std::move (points));
                    if (onDeleted != nullptr)
                        onDeleted();
                }
            };
        }

        void sync()
        {
            const auto points = processor.getFreeformPoints();
            if (index < 0 || index >= static_cast<int> (points.size()))
                return;
            const auto& point = points[static_cast<size_t> (index)];
            freq.setText (juce::String (point.frequency, 1), false);
            gain.setText (juce::String (point.db, 2), false);
        }

        void push()
        {
            auto points = processor.getFreeformPoints();
            if (index < 0 || index >= static_cast<int> (points.size()))
                return;
            points[static_cast<size_t> (index)].frequency = juce::jlimit (20.0, 20000.0, freq.getText().getDoubleValue());
            const auto range = static_cast<double> (processor.getGlobalDbRange());
            points[static_cast<size_t> (index)].db = juce::jlimit (-range, range, gain.getText().getDoubleValue());
            processor.setFreeformPoints (std::move (points));
        }

        bool keyPressed (const juce::KeyPress& key) override
        {
            if (key != juce::KeyPress::upKey && key != juce::KeyPress::downKey)
                return false;
            auto points = processor.getFreeformPoints();
            if (index < 0 || index >= static_cast<int> (points.size()))
                return false;
            const auto step = key.getModifiers().isShiftDown() ? 1.0 : 0.25;
            auto& point = points[static_cast<size_t> (index)];
            const auto range = static_cast<double> (processor.getGlobalDbRange());
            point.db = juce::jlimit (-range, range, point.db + (key == juce::KeyPress::upKey ? step : -step));
            processor.setFreeformPoints (std::move (points));
            sync();
            return true;
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            graph.selectFreeformPoint (index, e.mods.isShiftDown());
            repaint();
        }

        void setSelected (bool shouldBeSelected)
        {
            if (selected != shouldBeSelected)
            {
                selected = shouldBeSelected;
                repaint();
            }
        }

        void paint (juce::Graphics& g) override
        {
            if (selected)
            {
                g.setColour (juce::Colour (0xff202832));
                g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
            }
        }

        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
        {
            auto points = processor.getFreeformPoints();
            if (index < 0 || index >= static_cast<int> (points.size()))
                return;
            auto& point = points[static_cast<size_t> (index)];
            const auto range = static_cast<double> (processor.getGlobalDbRange());
            point.db = juce::jlimit (-range, range, point.db + static_cast<double> (wheel.deltaY) * 4.0);
            processor.setFreeformPoints (std::move (points));
            sync();
        }

        void resized() override
        {
            auto area = getLocalBounds();
            freq.setBounds (area.removeFromLeft (94).reduced (2));
            gain.setBounds (area.removeFromLeft (82).reduced (2));
            del.setBounds (area.removeFromLeft (30).reduced (2));
        }

        FlexCurveAudioProcessor& processor;
        FlexCurveGraph& graph;
        int index;
        bool selected = false;
        juce::TextEditor freq, gain;
        juce::TextButton del;
        std::function<void()> onDeleted;
    };

    void rebuildControls()
    {
        lastActiveLayerId = processor.getActiveLayerId();
        lastMode = processor.getGraphicMode();
        selectedBands.clear();
        selectionAnchor = -1;
        sliders.clear();
        frequencyLabels.clear();
        pointRows.clear();
        removeAllChildren();
        addAndMakeVisible (enabled);
        addAndMakeVisible (mode);
        addAndMakeVisible (addPoint);
        addAndMakeVisible (reset);
        addAndMakeVisible (smooth);
        addAndMakeVisible (preserveVariable);
        addAndMakeVisible (copyEq);
        addAndMakeVisible (pasteEq);
        addAndMakeVisible (pasteInvertedEq);
        addAndMakeVisible (pointViewport);

        if (mode.getSelectedId() == 3)
        {
            pointViewport.setVisible (true);
            pointHeader.setText ("Frequency        Gain", juce::dontSendNotification);
            pointHeader.setColour (juce::Label::textColourId, mutedColour());
            addAndMakeVisible (pointHeader);
            rebuildPointRows();
            resized();
            return;
        }

        const auto count = processor.getGraphicBandCount();
        for (int i = 0; i < count; ++i)
        {
            auto slider = std::make_unique<FlexCurveSelectableSlider>();
            slider->setSliderStyle (juce::Slider::LinearVertical);
            slider->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 52, 20);
            const auto range = static_cast<double> (processor.getGlobalDbRange());
            slider->setRange (-range, range, 0.1);
            slider->setValue (processor.getGraphicGain (i));
            slider->setTooltip (frequencyLabel (processor.getGraphicBandFrequency (i)) + " Hz");
            slider->setWantsKeyboardFocus (true);
            slider->setScrollWheelEnabled (true);
            slider->setColour (juce::Slider::thumbColourId, accentColour());
            installSliderReset (*slider, 0.0);
            const int index = i;
            slider->onSelect = [this, index] (bool additive, bool rangeSelection)
            {
                const auto it = std::find (selectedBands.begin(), selectedBands.end(), index);
                if (rangeSelection)
                {
                    if (selectionAnchor < 0)
                        selectionAnchor = selectedBands.empty() ? index : selectedBands.back();
                    selectedBands.clear();
                    const auto first = juce::jmin (selectionAnchor, index);
                    const auto last = juce::jmax (selectionAnchor, index);
                    for (int band = first; band <= last; ++band)
                        selectedBands.push_back (band);
                }
                else if (additive)
                {
                    if (it == selectedBands.end()) selectedBands.push_back (index);
                    else selectedBands.erase (it);
                    selectionAnchor = index;
                }
                else if (it == selectedBands.end())
                {
                    selectedBands = { index };
                    selectionAnchor = index;
                }
                updateGraphicSelection();
            };
            slider->onResetSelection = [this, index]
            {
                if (std::find (selectedBands.begin(), selectedBands.end(), index) == selectedBands.end())
                    selectedBands = { index };
                syncingGraphic = true;
                for (const auto selected : selectedBands)
                {
                    processor.setGraphicGain (selected, 0.0f);
                    if (selected >= 0 && selected < static_cast<int> (sliders.size()))
                        sliders[static_cast<size_t> (selected)]->setValue (0.0, juce::dontSendNotification);
                    if (selected >= 0 && selected < static_cast<int> (lastGraphicValues.size()))
                        lastGraphicValues[static_cast<size_t> (selected)] = 0.0f;
                }
                syncingGraphic = false;
                updateGraphicSelection();
            };
            slider->onValueChange = [this, index, s = slider.get()]
            {
                if (syncingGraphic)
                    return;
                const auto oldValue = lastGraphicValues[static_cast<size_t> (index)];
                const auto nextValue = static_cast<float> (s->getValue());
                const auto delta = nextValue - oldValue;
                const auto range = processor.getGlobalDbRange();
                if (std::find (selectedBands.begin(), selectedBands.end(), index) == selectedBands.end())
                    selectedBands = { index };
                syncingGraphic = true;
                for (const auto selected : selectedBands)
                {
                    const auto value = juce::jlimit (-range, range, lastGraphicValues[static_cast<size_t> (selected)] + delta);
                    processor.setGraphicGain (selected, value);
                    if (selected < static_cast<int> (sliders.size()))
                        sliders[static_cast<size_t> (selected)]->setValue (value, juce::dontSendNotification);
                    lastGraphicValues[static_cast<size_t> (selected)] = value;
                }
                syncingGraphic = false;
                updateGraphicSelection();
            };
            addAndMakeVisible (*slider);
            sliders.push_back (std::move (slider));

            auto label = std::make_unique<juce::Label>();
            label->setText (frequencyLabel (processor.getGraphicBandFrequency (i)), juce::dontSendNotification);
            label->setJustificationType (juce::Justification::centred);
            label->setColour (juce::Label::textColourId, mutedColour());
            label->setFont (juce::FontOptions (10.0f));
            addAndMakeVisible (*label);
            frequencyLabels.push_back (std::move (label));
        }
        resized();
    }

    void rebuildPointRows()
    {
        pointRows.clear();
        const auto points = processor.getFreeformPoints();
        for (int i = 0; i < static_cast<int> (points.size()); ++i)
        {
            auto row = std::make_unique<PointRow> (processor, graph, i);
            pointRowsContent.addAndMakeVisible (*row);
            pointRows.push_back (std::move (row));
        }
        resized();
    }

    void syncSliders()
    {
        for (int i = 0; i < static_cast<int> (sliders.size()); ++i)
        {
            const auto range = static_cast<double> (processor.getGlobalDbRange());
            sliders[static_cast<size_t> (i)]->setRange (-range, range, 0.1);
            const auto value = processor.getGraphicGain (i);
            sliders[static_cast<size_t> (i)]->setValue (value, juce::dontSendNotification);
            lastGraphicValues[static_cast<size_t> (i)] = value;
        }
        updateGraphicSelection();
    }

    void updateGraphicSelection()
    {
        for (int i = 0; i < static_cast<int> (sliders.size()); ++i)
        {
            sliders[static_cast<size_t> (i)]->selected
                = std::find (selectedBands.begin(), selectedBands.end(), i) != selectedBands.end();
            sliders[static_cast<size_t> (i)]->repaint();
        }
    }

    void timerCallback() override
    {
        const auto hasLayer = processor.hasEditableActiveLayer();
        const auto editable = hasLayer && ! processor.isEditLocked();
        const auto currentMode = processor.getGraphicMode();
        const auto currentActiveLayerId = processor.getActiveLayerId();

        if (currentActiveLayerId != lastActiveLayerId || currentMode != lastMode)
        {
            mode.setSelectedId (currentMode == 15 ? 2 : (currentMode == 0 ? 3 : 1), juce::dontSendNotification);
            rebuildControls();
        }

        enabled.setEnabled (editable);
        mode.setEnabled (editable);
        addPoint.setEnabled (editable && mode.getSelectedId() == 3);
        reset.setEnabled (editable);
        smooth.setEnabled (editable);
        preserveVariable.setEnabled (editable);
        copyEq.setEnabled (editable);
        pasteEq.setEnabled (editable && processor.canPasteCurrentGraphicEq());
        pasteInvertedEq.setEnabled (editable && processor.canPasteCurrentGraphicEq());
        hint.setVisible (false);
        for (auto& slider : sliders)
            slider->setEnabled (editable);
        for (auto& row : pointRows)
            row->setEnabled (editable);

        enabled.setToggleState (processor.isGraphicEnabled(), juce::dontSendNotification);
        smooth.setToggleState (processor.isGraphicSmoothingEnabled(), juce::dontSendNotification);
        preserveVariable.setToggleState (processor.isPreserveVariableShapeAcrossModes(), juce::dontSendNotification);
        if (mode.getSelectedId() == 3)
        {
            const auto count = static_cast<int> (processor.getFreeformPoints().size());
            if (count != static_cast<int> (pointRows.size()))
                rebuildPointRows();
            else
                for (auto& row : pointRows)
                    if (! row->freq.hasKeyboardFocus (true) && ! row->gain.hasKeyboardFocus (true))
                        row->sync();

            const auto selected = graph.getSelectedFreeformIndices();
            for (int i = 0; i < static_cast<int> (pointRows.size()); ++i)
                pointRows[static_cast<size_t> (i)]->setSelected (std::find (selected.begin(), selected.end(), i) != selected.end());
        }
        else
        {
            syncSliders();
        }
    }

    FlexCurveAudioProcessor& processor;
    FlexCurveGraph& graph;
    juce::ToggleButton enabled;
    juce::ToggleButton smooth;
    juce::ToggleButton preserveVariable;
    juce::ComboBox mode;
    juce::TextButton addPoint;
    juce::TextButton reset;
    juce::TextButton copyEq, pasteEq, pasteInvertedEq;
    juce::Label hint;
    juce::Label pointHeader;
    std::vector<std::unique_ptr<FlexCurveSelectableSlider>> sliders;
    std::vector<std::unique_ptr<juce::Label>> frequencyLabels;
    std::vector<std::unique_ptr<PointRow>> pointRows;
    juce::Viewport pointViewport;
    juce::Component pointRowsContent;
    int lastActiveLayerId = -999;
    int lastMode = -999;
    std::vector<int> selectedBands;
    int selectionAnchor = -1;
    std::array<float, 31> lastGraphicValues {};
    bool syncingGraphic = false;
};

class FlexCurveAudioProcessorEditor::ParametricTab final : public juce::Component,
                                                         private juce::Timer
{
public:
    explicit ParametricTab (FlexCurveAudioProcessor& p) : processor (p)
    {
        viewport.setViewedComponent (&rowsContent, false);
        viewport.setScrollBarsShown (true, false);
        styleScrollBar (viewport.getVerticalScrollBar());
        addAndMakeVisible (viewport);
        addBand.setButtonText ("+ Filter");
        styleButton (addBand);
        addBand.onClick = [this] { processor.addParamBand(); };
        addAndMakeVisible (addBand);
        reset.setButtonText ("Reset");
        styleButton (reset);
        reset.onClick = [this] { processor.resetParametric(); rebuild(); };
        addAndMakeVisible (reset);
        copyEq.setButtonText ("Copy EQ");
        pasteEq.setButtonText ("Paste EQ");
        pasteInvertedEq.setButtonText ("Paste Inverted EQ");
        styleButton (copyEq);
        styleButton (pasteEq);
        styleButton (pasteInvertedEq);
        copyEq.onClick = [this] { processor.copyCurrentParametricEq(); };
        pasteEq.onClick = [this]
        {
            processor.pasteCurrentParametricEq();
            rebuild();
        };
        pasteInvertedEq.onClick = [this]
        {
            processor.pasteCurrentParametricEq (true);
            rebuild();
        };
        addAndMakeVisible (copyEq);
        addAndMakeVisible (pasteEq);
        addAndMakeVisible (pasteInvertedEq);
        hint.setColour (juce::Label::textColourId, mutedColour());
        hint.setFont (juce::FontOptions (15.0f));
        hint.setJustificationType (juce::Justification::centred);
        hint.setText ({}, juce::dontSendNotification);
        rebuild();
        startTimerHz (10);
    }

    ~ParametricTab() override
    {
        stopTimer();
        viewport.setViewedComponent (nullptr, false);
        rowsContent.removeAllChildren();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (10);
        auto header = area.removeFromTop (30);
        addBand.setBounds (header.removeFromLeft (90));
        reset.setBounds (header.removeFromLeft (80));
        copyEq.setBounds (header.removeFromLeft (170).reduced (4, 0));
        pasteEq.setBounds (header.removeFromLeft (130).reduced (4, 0));
        pasteInvertedEq.setBounds (header.removeFromLeft (190).reduced (4, 0));
        area.removeFromTop (8);
        hint.setBounds (area);
        viewport.setBounds (area);
        rowsContent.setBounds (0, 0, juce::jmax (760, area.getWidth() - 14),
                               juce::jmax (area.getHeight(), static_cast<int> (rows.size()) * 38));
        auto rowsArea = rowsContent.getLocalBounds();
        for (int i = 0; i < static_cast<int> (rows.size()); ++i)
            rows[static_cast<size_t> (i)]->setBounds (rowsArea.removeFromTop (38));
    }

private:
    struct Row final : juce::Component
    {
        Row (FlexCurveAudioProcessor& p, int idx) : processor (p), index (idx)
        {
            addMouseListener (this, true);
            enabled.setButtonText ("");
            addAndMakeVisible (enabled);
            type.addItem ("Peak", 1); type.addItem ("Low Shelf", 2); type.addItem ("High Shelf", 3);
            type.addItem ("Low Pass", 4); type.addItem ("High Pass", 5); type.addItem ("Notch", 6);
            styleCombo (type);
            addAndMakeVisible (type);
            for (auto* s : { &freq, &gain, &q })
            {
                s->setSliderStyle (juce::Slider::LinearHorizontal);
                s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 72, 20);
                s->setColour (juce::Slider::thumbColourId, accentColour());
                addAndMakeVisible (*s);
            }
            freq.setRange (20.0, 20000.0, 1.0);
            const auto range = static_cast<double> (processor.getGlobalDbRange());
            gain.setRange (-range, range, 0.1);
            q.setRange (0.1, 33.3333, 0.01);
            freq.setSkewFactorFromMidPoint (1000.0);
            gain.setTextValueSuffix (" dB");
            q.setTextValueSuffix (" Q");
            installSliderReset (freq, 1000.0);
            installSliderReset (gain, 0.0);
            installSliderReset (q, 1.0);
            sync();
            auto changed = [this] { push(); };
            enabled.onClick = changed; type.onChange = changed; freq.onValueChange = changed; gain.onValueChange = changed; q.onValueChange = changed;
            remove.setButtonText ("x");
            styleButton (remove);
            remove.onClick = [this] { processor.removeParamBand (index); };
            addAndMakeVisible (remove);
        }

        void sync()
        {
            const auto bands = processor.getParamBands();
            if (index >= static_cast<int> (bands.size()))
                return;
            const auto band = bands[static_cast<size_t> (index)];
            const auto range = static_cast<double> (processor.getGlobalDbRange());
            gain.setRange (-range, range, 0.1);
            enabled.setToggleState (band.enabled, juce::dontSendNotification);
            type.setSelectedId (static_cast<int> (band.type) + 1, juce::dontSendNotification);
            freq.setValue (band.frequency, juce::dontSendNotification);
            gain.setValue (band.gainDb, juce::dontSendNotification);
            q.setValue (band.q, juce::dontSendNotification);
        }

        void mouseDown (const juce::MouseEvent& event) override
        {
            if (onSelect != nullptr)
                onSelect (index, event.mods.isCtrlDown());
        }

        void setSelected (bool shouldSelect)
        {
            if (selected != shouldSelect)
            {
                selected = shouldSelect;
                repaint();
            }
        }

        void paint (juce::Graphics& g) override
        {
            if (selected)
            {
                g.setColour (juce::Colour (0xff25364b));
                g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 4.0f);
            }
        }

        void push()
        {
            FlexParamBand band;
            band.enabled = enabled.getToggleState();
            band.type = static_cast<FlexParamBand::Type> (juce::jlimit (0, 5, type.getSelectedId() - 1));
            band.frequency = static_cast<float> (freq.getValue());
            band.gainDb = static_cast<float> (gain.getValue());
            band.q = static_cast<float> (q.getValue());
            processor.setParamBand (index, band);
        }

        void resized() override
        {
            auto area = getLocalBounds();
            enabled.setBounds (area.removeFromLeft (28));
            type.setBounds (area.removeFromLeft (110).reduced (2));
            freq.setBounds (area.removeFromLeft (210).reduced (2));
            gain.setBounds (area.removeFromLeft (180).reduced (2));
            q.setBounds (area.removeFromLeft (160).reduced (2));
            remove.setBounds (area.removeFromLeft (32).reduced (2));
        }

        FlexCurveAudioProcessor& processor;
        int index;
        juce::ToggleButton enabled;
        juce::ComboBox type;
        FlexCurveResettableSlider freq, gain, q;
        juce::TextButton remove;
        bool selected = false;
        std::function<void(int, bool)> onSelect;
    };

    void rebuild()
    {
        lastActiveLayerId = processor.getActiveLayerId();
        rows.clear();
        const auto bandCount = static_cast<int> (processor.getParamBands().size());
        lastBandCount = bandCount;
        for (int i = 0; i < bandCount; ++i)
        {
            auto row = std::make_unique<Row> (processor, i);
            row->onSelect = [this] (int index, bool additive)
            {
                const auto it = std::find (selectedBands.begin(), selectedBands.end(), index);
                if (additive)
                {
                    if (it == selectedBands.end()) selectedBands.push_back (index);
                    else selectedBands.erase (it);
                }
                else if (it == selectedBands.end())
                    selectedBands = { index };
                updateSelection();
            };
            rowsContent.addAndMakeVisible (*row);
            rows.push_back (std::move (row));
        }
        resized();
    }

    void timerCallback() override
    {
        const auto hasLayer = processor.hasEditableActiveLayer();
        const auto editable = hasLayer && ! processor.isEditLocked();
        if (processor.getActiveLayerId() != lastActiveLayerId
            || static_cast<int> (processor.getParamBands().size()) != lastBandCount)
            rebuild();

        addBand.setEnabled (editable);
        reset.setEnabled (editable);
        copyEq.setEnabled (editable);
        pasteEq.setEnabled (editable && processor.canPasteCurrentParametricEq());
        pasteInvertedEq.setEnabled (editable && processor.canPasteCurrentParametricEq());
        hint.setVisible (false);
        for (auto& row : rows)
        {
            row->setEnabled (editable);
            row->sync();
        }
        updateSelection();
    }

    void updateSelection()
    {
        for (int i = 0; i < static_cast<int> (rows.size()); ++i)
            rows[static_cast<size_t> (i)]->setSelected (
                std::find (selectedBands.begin(), selectedBands.end(), i) != selectedBands.end());
    }

    FlexCurveAudioProcessor& processor;
    juce::Viewport viewport;
    juce::Component rowsContent;
    juce::TextButton addBand;
    juce::TextButton reset;
    juce::TextButton copyEq, pasteEq, pasteInvertedEq;
    juce::Label hint;
    std::vector<std::unique_ptr<Row>> rows;
    int lastActiveLayerId = -999;
    int lastBandCount = -1;
    std::vector<int> selectedBands;
};

class FlexCurveAudioProcessorEditor::GlobalLayerRack final : public juce::Component,
                                                             private juce::ScrollBar::Listener
{
public:
    explicit GlobalLayerRack (FlexCurveAudioProcessor& p) : processor (p), horizontalScroll (false)
    {
        horizontalScroll.addListener (this);
        horizontalScroll.setAutoHide (false);
        styleScrollBar (horizontalScroll);
        addAndMakeVisible (horizontalScroll);
    }

    ~GlobalLayerRack() override
    {
        horizontalScroll.removeListener (this);
    }

    bool sync()
    {
        const auto layers = processor.getLayers();
        juce::String nextSignature;
        for (const auto& layer : layers)
            nextSignature << layer.id << ":" << static_cast<int> (layer.type) << ":"
                          << layer.name << ":" << layer.colour.getARGB() << "|";

        const auto structureChanged = nextSignature != signature;
        if (structureChanged)
        {
            signature = nextSignature;
            rows.clear();
            for (const auto& layer : layers)
            {
                auto row = std::make_unique<Row> (processor, layer.id);
                addAndMakeVisible (*row);
                rows.push_back (std::move (row));
            }
            resized();
            repaint();
        }

        for (auto& row : rows)
            row->sync();

        return structureChanged;
    }

    void resized() override
    {
        auto area = getLocalBounds();
        const auto scrollHeight = 8;
        auto scrollArea = area.removeFromBottom (scrollHeight);
        if (rows.empty())
        {
            horizontalScroll.setVisible (false);
            return;
        }

        constexpr int rowWidth = 286;
        constexpr int rowHeight = 30;
        constexpr int gap = 5;
        const auto totalWidth = static_cast<int> (rows.size()) * rowWidth
                              + juce::jmax (0, static_cast<int> (rows.size()) - 1) * gap;
        const auto visibleWidth = area.getWidth();
        const auto canScroll = totalWidth > visibleWidth;
        horizontalScroll.setVisible (canScroll);
        horizontalScroll.setBounds (scrollArea.reduced (0, 1));
        horizontalScroll.setRangeLimits (0.0, static_cast<double> (juce::jmax (totalWidth, visibleWidth)));
        horizontalScroll.setCurrentRange (juce::jlimit (0.0, static_cast<double> (juce::jmax (0, totalWidth - visibleWidth)),
                                                       horizontalScroll.getCurrentRangeStart()),
                                          static_cast<double> (visibleWidth),
                                          juce::dontSendNotification);

        auto x = area.getX() - static_cast<int> (std::round (horizontalScroll.getCurrentRangeStart()));
        auto y = area.getY();
        for (auto& row : rows)
        {
            row->setBounds (x, y, rowWidth, rowHeight);
            x += rowWidth + gap;
        }
    }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
    {
        if (! horizontalScroll.isVisible())
            return;
        const auto delta = (std::abs (wheel.deltaX) > std::abs (wheel.deltaY) ? wheel.deltaX : wheel.deltaY) * -120.0;
        horizontalScroll.setCurrentRangeStart (horizontalScroll.getCurrentRangeStart() + delta);
    }

private:
    class RackToggleButton final : public juce::Button
    {
    public:
        explicit RackToggleButton (juce::String text) : juce::Button (text), label (std::move (text))
        {
            setClickingTogglesState (true);
        }

        void paintButton (juce::Graphics& g, bool highlighted, bool down) override
        {
            auto bounds = getLocalBounds().toFloat().reduced (2.0f);
            auto background = getToggleState() ? activeColour : juce::Colour (0xff151b21);
            if (highlighted)
                background = background.brighter (0.10f);
            if (down)
                background = background.darker (0.10f);

            g.setColour (background);
            g.fillRoundedRectangle (bounds, 5.0f);
            g.setColour (getToggleState() ? juce::Colour (0xfff4f7f8) : juce::Colour (0xffaeb8c2));
            g.drawRoundedRectangle (bounds, 5.0f, 1.0f);
            g.setColour (juce::Colour (0xfff4f7f8));
            g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            g.drawText (label, getLocalBounds(), juce::Justification::centred);
        }

        juce::Colour activeColour { 0xff247a68 };

    private:
        juce::String label;
    };

    class LayerColourButton final : public juce::Button
    {
    public:
        LayerColourButton() : juce::Button ("Select layer") {}

        void paintButton (juce::Graphics& g, bool highlighted, bool down) override
        {
            if (active || highlighted)
            {
                g.setColour (active ? juce::Colour (0xff293440) : juce::Colour (0xff202832));
                g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);
            }

            auto circle = getLocalBounds().toFloat().withSizeKeepingCentre (12.0f, 12.0f);
            g.setColour (down ? colour.darker (0.18f) : colour);
            g.fillEllipse (circle);
        }

        juce::Colour colour { juce::Colours::white };
        bool active = false;
    };

    struct Row final : juce::Component,
                       juce::SettableTooltipClient
    {
        Row (FlexCurveAudioProcessor& p, int id) : processor (p), layerId (id)
        {
            addAndMakeVisible (colourButton);
            for (auto* button : { &visible, &mute, &solo })
            {
                addAndMakeVisible (*button);
            }
            channel.addItem ("L+R", 1);
            channel.addItem ("L", 2);
            channel.addItem ("R", 3);
            styleCombo (channel);
            channel.setTooltip ("Editing channel for this layer");
            addAndMakeVisible (channel);

            colourButton.onClick = [this] { processor.setActiveLayerId (layerId); };

            visible.onClick = [this]
            {
                processor.setActiveLayerId (layerId);
                processor.setLayerVisible (layerId, visible.getToggleState());
            };
            mute.onClick = [this]
            {
                processor.setActiveLayerId (layerId);
                processor.setLayerMuted (layerId, mute.getToggleState());
            };
            solo.onClick = [this]
            {
                processor.setActiveLayerId (layerId);
                processor.setLayerSolo (layerId, solo.getToggleState());
            };
            channel.onChange = [this]
            {
                if (channel.getSelectedId() > 0)
                {
                    processor.setActiveLayerId (layerId);
                    processor.setLayerChannelSelection (
                        layerId,
                        static_cast<FlexChannelSelection> (channel.getSelectedId() - 1));
                }
            };
        }

        void sync()
        {
            const auto layers = processor.getLayers();
            const auto it = std::find_if (layers.begin(), layers.end(), [this] (const auto& layer) { return layer.id == layerId; });
            if (it == layers.end())
                return;

            colour = it->colour;
            layerType = it->type;
            layerName = it->name;
            colourButton.colour = colour;
            colourButton.active = processor.getActiveLayerId() == layerId;
            visible.setToggleState (it->visible, juce::dontSendNotification);
            mute.setToggleState (it->muted, juce::dontSendNotification);
            solo.setToggleState (it->solo, juce::dontSendNotification);
            channel.setSelectedId (static_cast<int> (it->selectedChannel) + 1,
                                   juce::dontSendNotification);
            visible.activeColour = colour.darker (0.35f);
            mute.activeColour = juce::Colour (0xffa33a45);
            solo.activeColour = juce::Colour (0xff387f68);
            setTooltip (layerName);
            colourButton.setTooltip (layerName);
            visible.setTooltip ("V - Show or hide " + layerName + " in the graph");
            mute.setTooltip ("M - Mute " + layerName);
            solo.setTooltip ("S - Solo " + layerName);
            colourButton.setEnabled (true);
            visible.setEnabled (true);
            const auto isEq = layerType == FlexCurveLayerType::eq;
            mute.setVisible (isEq);
            solo.setVisible (isEq);
            mute.setEnabled (isEq && ! processor.isEditLocked());
            solo.setEnabled (isEq && ! processor.isEditLocked());
            channel.setEnabled (true);
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            juce::ignoreUnused (g);
        }

        void resized() override
        {
            auto area = getLocalBounds();
            colourButton.setBounds (area.removeFromLeft (34));
            visible.setBounds (area.removeFromLeft (34).reduced (1, 0));
            mute.setBounds (area.removeFromLeft (34).reduced (1, 0));
            solo.setBounds (area.removeFromLeft (34).reduced (1, 0));
            channel.setBounds (area.removeFromLeft (122).reduced (4, 2));
        }

        FlexCurveAudioProcessor& processor;
        int layerId;
        juce::Colour colour;
        juce::String layerName;
        FlexCurveLayerType layerType = FlexCurveLayerType::eq;
        LayerColourButton colourButton;
        RackToggleButton visible { "V" };
        RackToggleButton mute { "M" };
        RackToggleButton solo { "S" };
        juce::ComboBox channel;
    };

    FlexCurveAudioProcessor& processor;
    juce::String signature;
    std::vector<std::unique_ptr<Row>> rows;
    juce::ScrollBar horizontalScroll;

    void scrollBarMoved (juce::ScrollBar*, double) override
    {
        resized();
    }
};

class FlexCurveAudioProcessorEditor::MeterPanel final : public juce::Component
{
public:
    MeterPanel() = default;

    void setSnapshot (FlexCurveAudioProcessor::MeterSnapshot next)
    {
        snapshot = next;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xff0b1015));
        g.fillRoundedRectangle (bounds, 5.0f);
        g.setColour (juce::Colour (0xff34404d));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 5.0f, 1.0f);

        auto content = bounds.reduced (10.0f, 8.0f);
        g.setColour (inkColour());
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText ("LEVELS", content.removeFromTop (18.0f), juce::Justification::centredLeft);

        content.removeFromTop (3.0f);
        auto meters = content.removeFromTop (juce::jmax (260.0f, content.getHeight() - 52.0f));
        auto bars = meters.withTrimmedLeft (30.0f).withTrimmedRight (4.0f).withTrimmedTop (10.0f).withTrimmedBottom (42.0f);
        const auto groupGap = 9.0f;
        const auto innerGap = 3.0f;
        const auto barWidth = juce::jmax (10.0f, (bars.getWidth() - groupGap * 2.0f - innerGap * 3.0f) / 6.0f);
        const std::array<std::array<FlexCurveAudioProcessor::MeterSnapshot::Channel, 3>, 3> stages {
            snapshot.inputChannels, snapshot.preAutoChannels, snapshot.outputChannels
        };
        const std::array<juce::String, 3> stageLabels { "IN", "PRE", "OUT" };

        g.setFont (juce::FontOptions (8.5f));
        for (const auto db : { 0.0f, -12.0f, -24.0f, -36.0f, -48.0f, -60.0f })
        {
            const auto y = juce::jmap (db, -60.0f, 0.0f, bars.getBottom(), bars.getY());
            g.setColour (juce::Colour (0xff2a333d));
            g.drawHorizontalLine (static_cast<int> (std::round (y)), bars.getX(), bars.getRight());
            g.setColour (mutedColour());
            g.drawText (juce::String (static_cast<int> (db)), static_cast<int> (meters.getX()), static_cast<int> (y - 6.0f),
                        23, 12, juce::Justification::centredRight);
        }

        const auto fmtDb = [] (float value, const juce::String& suffix)
        {
            return value <= -99.0f ? "-inf" + suffix : juce::String (value, 1) + suffix;
        };
        const auto autoGainAbs = std::abs (snapshot.autoGainDb);
        const auto autoColour = autoGainAbs > 12.0f ? juce::Colour (0xffff5a5f)
                              : autoGainAbs > 6.0f  ? juce::Colour (0xffffb84d)
                                                    : juce::Colour (0xff54cdff);
        const auto meterValueColour = [&] (size_t stage)
        {
            if (stage == 0)
                return snapshot.inputClipped ? juce::Colour (0xffff5a5f) : mutedColour();
            if (stage == 1)
                return autoColour;
            return snapshot.outputClipped ? juce::Colour (0xffff5a5f) : mutedColour();
        };

        g.setFont (juce::FontOptions (7.2f, juce::Font::bold));
        for (size_t stage = 0; stage < stages.size(); ++stage)
        {
            const auto clipped = stage == 0 ? snapshot.inputClipped
                               : stage == 1 ? snapshot.preAutoClipped
                                            : snapshot.outputClipped;
            const auto groupX = bars.getX() + static_cast<float> (stage) * (2.0f * barWidth + innerGap + groupGap);
            for (int channel = 0; channel < 2; ++channel)
            {
                const auto x = groupX + static_cast<float> (channel) * (barWidth + innerGap);
                auto bar = juce::Rectangle<float> (x, bars.getY(), barWidth, bars.getHeight());
                g.setColour (juce::Colour (0xff111820));
                g.fillRect (bar);
                g.setColour (juce::Colour (0xff3a4652));
                g.drawRect (bar, 1.0f);

                const auto rmsDb = stages[stage][channel].rmsDb;
                const auto peakDb = stages[stage][channel].peakDb;
                const auto level = juce::jlimit (0.0f, 1.0f, juce::jmap (rmsDb, -60.0f, 0.0f, 0.0f, 1.0f));
                auto fill = bar;
                fill.removeFromTop (fill.getHeight() * (1.0f - level));
                if (fill.getHeight() > 0.5f)
                {
                    juce::ColourGradient gradient (juce::Colour (0xff46d5bd), fill.getBottomLeft(),
                                                   juce::Colour (0xffffd158), fill.getTopLeft(), false);
                    gradient.addColour (0.70, juce::Colour (0xff5ee467));
                    gradient.addColour (0.88, juce::Colour (0xffffa43a));
                    gradient.addColour (1.0, juce::Colour (0xffff4d52));
                    g.setGradientFill (gradient);
                    g.fillRect (fill);
                }

                const auto peakY = juce::jmap (juce::jlimit (-60.0f, 0.0f, peakDb), -60.0f, 0.0f, bar.getBottom(), bar.getY());
                g.setColour (clipped ? juce::Colour (0xffff4d52) : juce::Colour (0xfff4f7f8));
                g.drawHorizontalLine (static_cast<int> (std::round (peakY)), bar.getX(), bar.getRight());

                g.setColour (meterValueColour (stage));
                g.drawFittedText (fmtDb (peakDb, ""),
                                  bar.withY (bars.getY() - 10.0f).withHeight (9.0f).toNearestInt(),
                                  juce::Justification::centred, 1, 0.45f);
                g.setColour (mutedColour());
                g.setFont (juce::FontOptions (8.0f, juce::Font::bold));
                g.drawText (channel == 0 ? "L" : "R",
                            static_cast<int> (bar.getX() - 1.0f), static_cast<int> (bar.getBottom() + 2.0f),
                            static_cast<int> (bar.getWidth() + 2.0f), 12, juce::Justification::centred);
                g.setFont (juce::FontOptions (7.2f, juce::Font::bold));
            }
        }

        g.setColour (inkColour());
        g.setFont (juce::FontOptions (9.5f, juce::Font::bold));
        for (size_t stage = 0; stage < stageLabels.size(); ++stage)
        {
            const auto groupX = bars.getX() + static_cast<float> (stage) * (2.0f * barWidth + innerGap + groupGap);
            g.drawText (stageLabels[stage],
                        static_cast<int> (groupX),
                        static_cast<int> (bars.getBottom() + 17.0f),
                        static_cast<int> (2.0f * barWidth + innerGap), 13, juce::Justification::centred);
        }

        auto readouts = content.removeFromBottom (39.0f);
        auto drawReadout = [&g] (juce::Rectangle<float> row, const juce::String& label,
                                 juce::String value, juce::Colour colour)
        {
            g.setColour (colour);
            g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
            g.drawText (label + " " + value, row, juce::Justification::centred);
        };
        const auto fmtSigned = [] (float value)
        {
            return juce::String (value >= 0.0f ? "+" : "") + juce::String (value, 1) + " dB";
        };
        const auto inputValue = snapshot.inputClipped
            ? fmtSigned (snapshot.inputClipOverDb) + " CLIP"
            : juce::String (snapshot.inputPeakDb, 1) + " dB";
        const auto outputValue = snapshot.outputClipped
            ? fmtSigned (snapshot.outputClipOverDb) + " CLIP"
            : juce::String (snapshot.outputPeakDb, 1) + " dB";
        auto autoValue = juce::String (snapshot.autoGainDb, 1) + " dB";
        if (snapshot.preAutoClipped)
            autoValue += "  PRE " + fmtSigned (snapshot.preAutoClipOverDb);

        drawReadout (readouts.removeFromTop (13.0f), "IN", inputValue,
                     snapshot.inputClipped ? juce::Colour (0xffff5a5f) : mutedColour());
        drawReadout (readouts.removeFromTop (13.0f), "AUTO", autoValue, autoColour);
        drawReadout (readouts.removeFromTop (13.0f), "OUT", outputValue,
                     snapshot.outputClipped ? juce::Colour (0xffff5a5f) : mutedColour());
    }

    void resized() override
    {
    }

private:
    FlexCurveAudioProcessor::MeterSnapshot snapshot;
};

class FlexCurveAudioProcessorEditor::GraphResizeHandle final : public juce::Component,
                                                               public juce::SettableTooltipClient
{
public:
    explicit GraphResizeHandle (FlexCurveAudioProcessorEditor& editor) : owner (editor)
    {
        setMouseCursor (juce::MouseCursor::BottomRightCornerResizeCursor);
        setTooltip ("Drag diagonally to resize only the graph area");
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat().reduced (2.0f);
        g.setColour (juce::Colour (0xff1b242d));
        g.fillRoundedRectangle (bounds, 4.0f);
        g.setColour (juce::Colour (0xff71808e));
        for (int offset = 4; offset <= 12; offset += 4)
            g.drawLine (bounds.getRight() - static_cast<float> (offset), bounds.getBottom(),
                        bounds.getRight(), bounds.getBottom() - static_cast<float> (offset), 1.2f);
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        startHeight = owner.graphSectionHeight;
        startControlsWidth = owner.globalControlsWidth;
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        const auto maxHeight = juce::jmax (410, owner.getHeight() - 320);
        owner.graphSectionHeight = juce::jlimit (410, maxHeight,
                                                 startHeight + event.getDistanceFromDragStartY());
        owner.globalControlsWidth = juce::jlimit (720, juce::jmax (720, owner.getWidth() - 620),
                                                  startControlsWidth - event.getDistanceFromDragStartX());
        owner.resized();
    }

private:
    FlexCurveAudioProcessorEditor& owner;
    int startHeight = 430;
    int startControlsWidth = 720;
};

FlexCurveAudioProcessorEditor::FlexCurveAudioProcessorEditor (FlexCurveAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p), graph (p)
{
    meterPanel = std::make_unique<MeterPanel>();
    graphResizeHandle = std::make_unique<GraphResizeHandle> (*this);
    addAndMakeVisible (scaledContent);
    setResizable (true, true);
    setResizeLimits (900, 560, 3200, 2200);
    setSize (designWidth, designHeight);

    title.setText ("FlexCurve", juce::dontSendNotification);
    title.setFont (juce::FontOptions (30.0f, juce::Font::bold));
    title.setColour (juce::Label::textColourId, inkColour());
    addAndMakeVisible (title);

    status.setColour (juce::Label::textColourId, mutedColour());
    status.setFont (juce::FontOptions (13.0f));
    addAndMakeVisible (status);

    styleCombo (presetCombo);
    presetCombo.onChange = [this] { handlePresetComboChange(); };
    addAndMakeVisible (presetCombo);

    activeLayerLabel.setText ("Active Layer: none", juce::dontSendNotification);
    activeLayerLabel.setColour (juce::Label::textColourId, inkColour());
    activeLayerLabel.setFont (juce::FontOptions (12.5f));
    activeLayerLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (activeLayerLabel);

    globalLayerRack = std::make_unique<GlobalLayerRack> (processor);
    addAndMakeVisible (*globalLayerRack);

    styleButton (addCurve);
    styleButton (newFlat);
    styleButton (renderFir);
    styleButton (exportFir);
    styleButton (resetAll);
    styleButton (resetFlat);
    styleButton (help);
    styleButton (zoomIn);
    styleButton (zoomOut);
    styleButton (zoomReset);
    styleButton (smoothAll);
    smoothAll.setButtonText ("Smooth All Layers");
    smoothAll.setClickingTogglesState (true);
    addCurve.onClick = [this] { openAddCurveChooser(); };
    newFlat.onClick = [this]
    {
        if (! processor.addFlatCurve())
            juce::AlertWindow::showMessageBoxAsync (juce::AlertWindow::InfoIcon, "FlexCurve", "Maximum of 6 user layers reached.");
        updateActiveLayerCombo();
        updateAddButtons();
        if (globalLayerRack != nullptr)
        {
            globalLayerRack->sync();
            globalLayerRack->resized();
            globalLayerRack->repaint();
        }
        resized();
    };
    renderFir.onClick = [this] { processor.renderFir(); };
    exportFir.onClick = [this] { openExportFirChooser(); };
    resetFlat.onClick = [this] { processor.resetAllLayerCurvesToFlat(); };
    resetAll.onClick = [this]
    {
        processor.resetAll();
        updatePresetCombo();
        updateActiveLayerCombo();
        updateAddButtons();
    };
    help.onClick = [this] { showHelp(); };
    zoomIn.onClick = [this] { graph.zoomIn(); };
    zoomOut.onClick = [this] { graph.zoomOut(); };
    zoomReset.onClick = [this] { graph.resetZoom(); };
    lockMode.onClick = [this] { processor.setEditLocked (lockMode.getToggleState()); };
    undoButton.onClick = [this] { processor.undo(); };
    redoButton.onClick = [this] { processor.redo(); };
    smoothAll.onClick = [this] { processor.setAllGraphicSmoothing (smoothAll.getToggleState()); };
    undoButton.setTooltip ("Undo the last FlexCurve edit");
    redoButton.setTooltip ("Redo the last FlexCurve edit");
    smoothAll.setTooltip ("Enable or disable non-destructive source and Graphic/Variable smoothing on every layer");
    addAndMakeVisible (addCurve);
    addAndMakeVisible (newFlat);
    addAndMakeVisible (renderFir);
    addAndMakeVisible (exportFir);
    addAndMakeVisible (resetFlat);
    addAndMakeVisible (resetAll);
    addAndMakeVisible (help);
    addAndMakeVisible (zoomIn);
    addAndMakeVisible (zoomOut);
    addAndMakeVisible (zoomReset);
    addAndMakeVisible (undoButton);
    addAndMakeVisible (redoButton);
    addAndMakeVisible (smoothAll);

    dbScaleLabel.setText ("Global dB Scale", juce::dontSendNotification);
    dbScaleLabel.setColour (juce::Label::textColourId, mutedColour());
    dbScaleLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (dbScaleLabel);
    for (auto value : { 12, 24, 36, 48 })
        dbScale.addItem (juce::String::charToString (0x00b1) + juce::String (value) + " dB", value);
    dbScale.setEditableText (true);
    dbScale.setTextWhenNothingSelected ("Custom dB");
    styleCombo (dbScale);
    dbScale.onChange = [this]
    {
        auto value = static_cast<float> (dbScale.getText().retainCharacters ("0123456789.").getDoubleValue());
        if (value <= 0.0f && dbScale.getSelectedId() > 0)
            value = static_cast<float> (dbScale.getSelectedId());
        if (value > 0.0f)
        {
            processor.setGlobalDbRange (value);
            graph.setViewRange (value);
        }
    };
    addAndMakeVisible (dbScale);

    uiScaleLabel.setText ("UI Scale", juce::dontSendNotification);
    uiScaleLabel.setColour (juce::Label::textColourId, mutedColour());
    uiScaleLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (uiScaleLabel);
    for (const auto percent : { 60, 70, 80, 90, 100, 110, 125, 140 })
        uiScale.addItem (juce::String (percent) + "%", percent);
    styleCombo (uiScale);
    const auto savedUiScale = static_cast<int> (processor.parameters.state.getProperty ("uiScalePercent", 100));
    uiScale.setSelectedId (juce::jlimit (60, 140, savedUiScale), juce::dontSendNotification);
    uiScale.onChange = [this]
    {
        const auto percent = uiScale.getSelectedId() > 0 ? uiScale.getSelectedId() : 100;
        processor.parameters.state.setProperty ("uiScalePercent", percent, nullptr);
        applyUiScalePreset (static_cast<float> (percent) / 100.0f);
    };
    uiScale.setTooltip ("Resize the complete FlexCurve interface proportionally");
    addAndMakeVisible (uiScale);

    addAndMakeVisible (graph);
    graph.setViewRange (processor.getGlobalDbRange());
    globalControlsViewport.setViewedComponent (&globalControlsContent, false);
    globalControlsViewport.setScrollBarsShown (true, false);
    globalControlsViewport.setScrollBarThickness (8);
    styleScrollBar (globalControlsViewport.getVerticalScrollBar());
    addAndMakeVisible (globalControlsViewport);
    levelsViewport.setViewedComponent (&levelsContent, false);
    levelsViewport.setScrollBarsShown (false, false);
    levelsViewport.setScrollBarThickness (8);
    styleScrollBar (levelsViewport.getVerticalScrollBar());
    addAndMakeVisible (levelsViewport);

    styleSlider (dryWet);
    styleSlider (crossfeed);
    styleSlider (globalBalance, " dB");
    styleSlider (gain, " dB");
    styleSlider (inputGain, " dB");
    styleSlider (outputGain, " dB");
    dryWet.setTooltip ("Blend latency-aligned dry audio with the corrected signal");
    crossfeed.setTooltip ("Reduce hard left/right separation for headphone listening");
    globalBalance.setTooltip ("Final stereo balance trim. Negative shifts left, positive shifts right");
    gain.setTooltip ("Final monitoring gain. Never included in FIR rendering or export");
    inputGain.setTooltip ("Gain before correction and Input metering");
    outputGain.setTooltip ("Gain after correction and Auto Gain, before Global Gain");

    for (auto* label : { &dryWetLabel, &crossfeedLabel, &globalBalanceLabel, &gainLabel, &inputGainLabel, &outputGainLabel,
                         &phaseModeLabel, &globalControlsLabel })
    {
        label->setColour (juce::Label::textColourId, inkColour());
        label->setFont (juce::FontOptions (13.5f));
        label->setJustificationType (juce::Justification::centred);
    }
    dryWetLabel.setText ("Dry/Wet", juce::dontSendNotification);
    crossfeedLabel.setText ("Crossfeed", juce::dontSendNotification);
    globalBalanceLabel.setText ("Balance", juce::dontSendNotification);
    gainLabel.setText ("Global Gain", juce::dontSendNotification);
    inputGainLabel.setText ("Input Gain", juce::dontSendNotification);
    outputGainLabel.setText ("Output Gain", juce::dontSendNotification);
    phaseModeLabel.setText ("Phase Mode", juce::dontSendNotification);
    globalControlsLabel.setText ("Global Controls", juce::dontSendNotification);
    globalControlsLabel.setJustificationType (juce::Justification::centredLeft);

    phaseMode.addItem ("Minimum", 1);
    phaseMode.addItem ("Natural", 2);
    phaseMode.addItem ("Linear", 3);
    phaseMode.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff202832));
    phaseMode.setColour (juce::ComboBox::textColourId, inkColour());
    addAndMakeVisible (globalControlsLabel);

    styleToggle (limiter);
    styleToggle (bypass);
    styleToggle (autoGain);
    styleToggle (includeOutputGainFir);
    styleToggle (includeAutoGainFir);
    styleToggle (independentLrPreamp);
    styleCombo (loudnessMatchMode);
    styleButton (crossfeedAdvanced);
    crossfeedAdvanced.setTooltip ("Open advanced crossfeed algorithm and geometry settings");
    crossfeedAdvanced.onClick = [this] { openCrossfeedAdvanced(); };
    loudnessMatchMode.addItem ("Match Output to Input", 1);
    loudnessMatchMode.addItem ("Downward Match", 2);
    autoGain.setTooltip ("Slow transparent RMS level matching without compression or limiting");
    loudnessMatchMode.setTooltip ("Choose bidirectional matching or attenuation-only Downward Match");
    includeOutputGainFir.setTooltip ("Explicitly bake Output Gain into exported FIR files");
    includeAutoGainFir.setTooltip ("Explicitly bake the current learned Auto Gain into exported FIR files");
    independentLrPreamp.setTooltip (
        "Advanced: calculate separate safe AutoEQ preamps for L and R. This can change stereo balance.");
    addAndMakeVisible (*graphResizeHandle);

    blendTab = std::make_unique<BlendTab> (processor);
    graphicTab = std::make_unique<GraphicTab> (processor, graph);
    parametricTab = std::make_unique<ParametricTab> (processor);
    tabs.addTab ("Blend", surfaceColour(), blendTab.get(), false);
    tabs.addTab ("Graphic EQ", surfaceColour(), graphicTab.get(), false);
    tabs.addTab ("Parametric EQ", surfaceColour(), parametricTab.get(), false);
    tabs.setTabBarDepth (38);
    tabs.setOutline (1);
    tabs.setColour (juce::TabbedComponent::backgroundColourId, panelColour());
    tabs.setCurrentTabIndex (processor.getLastOpenTabIndex());
    addAndMakeVisible (tabs);

    dryWetAttachment = std::make_unique<SliderAttachment> (processor.parameters, "drywet", dryWet);
    crossfeedAttachment = std::make_unique<SliderAttachment> (processor.parameters, "crossfeed", crossfeed);
    globalBalanceAttachment = std::make_unique<SliderAttachment> (processor.parameters, "globalbalance", globalBalance);
    gainAttachment = std::make_unique<SliderAttachment> (processor.parameters, "gain", gain);
    inputGainAttachment = std::make_unique<SliderAttachment> (processor.parameters, "inputgain", inputGain);
    outputGainAttachment = std::make_unique<SliderAttachment> (processor.parameters, "outputgain", outputGain);
    phaseModeAttachment = std::make_unique<ComboBoxAttachment> (processor.parameters, "phasemode", phaseMode);
    limiterAttachment = std::make_unique<ButtonAttachment> (processor.parameters, "limiter", limiter);
    bypassAttachment = std::make_unique<ButtonAttachment> (processor.parameters, "bypass", bypass);
    autoGainAttachment = std::make_unique<ButtonAttachment> (processor.parameters, "autogain", autoGain);
    loudnessMatchAttachment = std::make_unique<ComboBoxAttachment> (
        processor.parameters, "loudnessmatchmode", loudnessMatchMode);
    includeOutputGainFirAttachment = std::make_unique<ButtonAttachment> (
        processor.parameters, "includeoutputgainfir", includeOutputGainFir);
    includeAutoGainFirAttachment = std::make_unique<ButtonAttachment> (
        processor.parameters, "includeautogainfir", includeAutoGainFir);
    independentLrPreampAttachment = std::make_unique<ButtonAttachment> (
        processor.parameters, "independentlrpreamp", independentLrPreamp);

    installSliderReset (dryWet, 1.0);
    installSliderReset (crossfeed, 0.0);
    installSliderReset (globalBalance, 0.0);
    installSliderReset (gain, 0.0);
    installSliderReset (inputGain, 0.0);
    installSliderReset (outputGain, 0.0);

    for (auto* child : { static_cast<juce::Component*> (&dryWet),
                         static_cast<juce::Component*> (&crossfeed),
                         static_cast<juce::Component*> (&globalBalance),
                         static_cast<juce::Component*> (&gain),
                         static_cast<juce::Component*> (&inputGain),
                         static_cast<juce::Component*> (&outputGain),
                         static_cast<juce::Component*> (&dryWetLabel),
                         static_cast<juce::Component*> (&crossfeedLabel),
                         static_cast<juce::Component*> (&globalBalanceLabel),
                         static_cast<juce::Component*> (&gainLabel),
                         static_cast<juce::Component*> (&inputGainLabel),
                         static_cast<juce::Component*> (&outputGainLabel),
                         static_cast<juce::Component*> (&phaseModeLabel),
                         static_cast<juce::Component*> (&phaseMode),
                         static_cast<juce::Component*> (&autoGain),
                         static_cast<juce::Component*> (&loudnessMatchMode),
                         static_cast<juce::Component*> (&crossfeedAdvanced),
                         static_cast<juce::Component*> (&lockMode),
                         static_cast<juce::Component*> (&limiter),
                         static_cast<juce::Component*> (&bypass),
                         static_cast<juce::Component*> (&includeOutputGainFir),
                         static_cast<juce::Component*> (&includeAutoGainFir),
                         static_cast<juce::Component*> (&independentLrPreamp) })
        globalControlsContent.addAndMakeVisible (*child);
    levelsContent.addAndMakeVisible (*meterPanel);

    updatePresetCombo();
    updateActiveLayerCombo();
    updateAddButtons();
    moveUiIntoScaledContent();
    applyUiScalePreset (static_cast<float> (juce::jlimit (60, 140, savedUiScale)) / 100.0f);
    startTimerHz (30);
}

FlexCurveAudioProcessorEditor::~FlexCurveAudioProcessorEditor()
{
    stopTimer();

    presetCombo.onChange = nullptr;
    chooser.reset();

    globalControlsViewport.setViewedComponent (nullptr, false);
    levelsViewport.setViewedComponent (nullptr, false);
    globalControlsContent.removeAllChildren();
    levelsContent.removeAllChildren();

    // TabbedComponent does not own these pages. Detach them before the
    // unique_ptrs are destroyed so JUCE never sees stale page pointers.
    tabs.clearTabs();
    parametricTab.reset();
    graphicTab.reset();
    blendTab.reset();
    graphResizeHandle.reset();
    meterPanel.reset();
    globalLayerRack.reset();
}

void FlexCurveAudioProcessorEditor::styleSlider (juce::Slider& slider, const juce::String& suffix)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 76, 22);
    slider.setTextValueSuffix (suffix);
    slider.setColour (juce::Slider::thumbColourId, inkColour());
    slider.setColour (juce::Slider::rotarySliderFillColourId, accentColour());
    slider.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff465360));
    slider.setColour (juce::Slider::textBoxTextColourId, inkColour());
    slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
}

void FlexCurveAudioProcessorEditor::openCrossfeedAdvanced()
{
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (new CrossfeedAdvancedComponent (processor));
    options.dialogTitle = "Advanced Crossfeed";
    options.dialogBackgroundColour = panelColour();
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.launchAsync();
}

void FlexCurveAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (panelColour());
}

void FlexCurveAudioProcessorEditor::moveUiIntoScaledContent()
{
    std::vector<juce::Component*> children;
    for (int i = 0; i < getNumChildComponents(); ++i)
    {
        auto* child = getChildComponent (i);
        if (child != &scaledContent && dynamic_cast<juce::ResizableCornerComponent*> (child) == nullptr)
            children.push_back (child);
    }

    for (auto* child : children)
        scaledContent.addAndMakeVisible (child);
}

void FlexCurveAudioProcessorEditor::applyUiScalePreset (float scale)
{
    scale = juce::jlimit (0.60f, 1.40f, scale);
    setSize (juce::roundToInt (static_cast<float> (designWidth) * scale),
             juce::roundToInt (static_cast<float> (designHeight) * scale));
}

void FlexCurveAudioProcessorEditor::resized()
{
    const auto savedScalePercent = static_cast<int> (processor.parameters.state.getProperty ("uiScalePercent", 100));
    const auto scale = static_cast<float> (juce::jlimit (60, 140, savedScalePercent)) / 100.0f;
    const auto logicalWidth = juce::jmax (designWidth, juce::roundToInt (static_cast<float> (getWidth()) / scale));
    const auto logicalHeight = juce::jmax (designHeight, juce::roundToInt (static_cast<float> (getHeight()) / scale));
    scaledContent.setBounds (0, 0, logicalWidth, logicalHeight);
    scaledContent.setTransform (juce::AffineTransform::scale (scale));

    auto area = juce::Rectangle<int> (0, 0, logicalWidth, logicalHeight).reduced (18);
    auto header = area.removeFromTop (132);
    auto commandRow = header.removeFromTop (42);
    title.setBounds (commandRow.removeFromLeft (210));
    presetCombo.setBounds (commandRow.removeFromLeft (220).withHeight (34).translated (0, 3));
    commandRow.removeFromLeft (8);
    newFlat.setBounds (commandRow.removeFromLeft (190).withHeight (32).translated (0, 3));
    commandRow.removeFromLeft (6);
    addCurve.setBounds (commandRow.removeFromLeft (220).withHeight (32).translated (0, 3));
    commandRow.removeFromLeft (6);
    renderFir.setBounds (commandRow.removeFromLeft (104).withHeight (32).translated (0, 3));
    commandRow.removeFromLeft (6);
    exportFir.setBounds (commandRow.removeFromLeft (104).withHeight (32).translated (0, 3));
    commandRow.removeFromLeft (6);
    resetFlat.setBounds (commandRow.removeFromLeft (150).withHeight (30).translated (0, 4));
    commandRow.removeFromLeft (6);
    resetAll.setBounds (commandRow.removeFromLeft (90).withHeight (30).translated (0, 4));
    commandRow.removeFromLeft (6);
    smoothAll.setBounds (commandRow.removeFromLeft (152).withHeight (30).translated (0, 4));

    auto toolRow = header.removeFromTop (34);
    help.setBounds (toolRow.removeFromRight (74).withHeight (32).translated (0, 1));
    toolRow.removeFromRight (10);
    zoomReset.setBounds (toolRow.removeFromRight (48).withHeight (30).translated (0, 2));
    toolRow.removeFromRight (4);
    zoomIn.setBounds (toolRow.removeFromRight (30).withHeight (30).translated (0, 2));
    toolRow.removeFromRight (4);
    zoomOut.setBounds (toolRow.removeFromRight (30).withHeight (30).translated (0, 2));
    toolRow.removeFromRight (10);
    uiScale.setBounds (toolRow.removeFromRight (82).withHeight (30).translated (0, 2));
    uiScaleLabel.setBounds (toolRow.removeFromRight (58).withHeight (30).translated (0, 2));
    toolRow.removeFromRight (10);
    dbScale.setBounds (toolRow.removeFromRight (108).withHeight (30).translated (0, 2));
    dbScaleLabel.setBounds (toolRow.removeFromRight (92).withHeight (30).translated (0, 2));
    toolRow.removeFromRight (8);
    redoButton.setBounds (toolRow.removeFromRight (32).withHeight (30).translated (0, 2));
    toolRow.removeFromRight (4);
    undoButton.setBounds (toolRow.removeFromRight (32).withHeight (30).translated (0, 2));
    toolRow.removeFromRight (14);
    status.setBounds (toolRow.removeFromRight (juce::jmin (520, juce::jmax (320, toolRow.getWidth() / 2))).withHeight (24).translated (0, 4));
    const auto maxActiveLabelWidth = juce::jmax (260, toolRow.getWidth());
    const auto desiredActiveLabelWidth = 24 + activeLayerLabel.getText().length() * 8;
    const auto activeLabelWidth = juce::jlimit (260, maxActiveLabelWidth, desiredActiveLabelWidth);
    activeLayerLabel.setBounds (toolRow.removeFromLeft (activeLabelWidth).withHeight (24).translated (0, 4));

    header.removeFromTop (4);
    auto rackRow = header.removeFromTop (42);
    if (globalLayerRack != nullptr)
        globalLayerRack->setBounds (rackRow);

    const auto maxGraphHeight = juce::jmax (410, area.getHeight() - 180);
    graphSectionHeight = juce::jlimit (410, maxGraphHeight, graphSectionHeight);
    globalControlsWidth = juce::jlimit (720, juce::jmax (720, area.getWidth() - 620), globalControlsWidth);
    auto top = area.removeFromTop (graphSectionHeight);
    auto rightPanel = top.removeFromRight (globalControlsWidth);
    top.removeFromRight (10);
    const auto graphBounds = top.reduced (0, 6);
    graph.setBounds (graphBounds);
    graphResizeHandle->setBounds (graphBounds.getRight() - 10, graphBounds.getBottom() - 10, 22, 22);
    graphResizeHandle->toFront (false);

    auto levelsArea = rightPanel.removeFromRight (190);
    rightPanel.removeFromRight (8);
    auto controls = rightPanel;
    globalControlsLabel.setBounds (controls.removeFromTop (20));
    globalControlsViewport.setBounds (controls);
    levelsViewport.setBounds (levelsArea);
    const auto contentWidth = juce::jmax (500, controls.getWidth() - 10);
    const auto contentHeight = 460;
    globalControlsContent.setBounds (0, 0, contentWidth, contentHeight);
    auto controlsContent = juce::Rectangle<int> (0, 0, contentWidth, contentHeight).reduced (4, 0);
    const auto levelsWidth = juce::jmax (172, levelsArea.getWidth() - 10);
    const auto levelsHeight = levelsArea.getHeight();
    levelsContent.setBounds (0, 0, levelsWidth, levelsHeight);
    auto levelsContentArea = juce::Rectangle<int> (0, 0, levelsWidth, levelsHeight).reduced (3, 0);

    auto setKnob = [] (juce::Rectangle<int> cell, juce::Slider& slider, juce::Label& label)
    {
        auto labelArea = cell.removeFromBottom (20);
        slider.setBounds (cell.reduced (0, 0));
        label.setBounds (labelArea);
    };
    constexpr int knobHeight = 108;
    auto monitorKnobs = controlsContent.removeFromTop (knobHeight);
    const auto monitorCellWidth = monitorKnobs.getWidth() / 3;
    setKnob (monitorKnobs.removeFromLeft (monitorCellWidth), dryWet, dryWetLabel);
    setKnob (monitorKnobs.removeFromLeft (monitorCellWidth), crossfeed, crossfeedLabel);
    setKnob (monitorKnobs, globalBalance, globalBalanceLabel);

    auto advancedRow = controlsContent.removeFromTop (24).reduced (18, 0);
    crossfeedAdvanced.setBounds (advancedRow.withSizeKeepingCentre (juce::jmin (142, advancedRow.getWidth()), 24));

    controlsContent.removeFromTop (0);
    auto gainKnobs = controlsContent.removeFromTop (knobHeight);
    const auto gainCellWidth = gainKnobs.getWidth() / 3;
    setKnob (gainKnobs.removeFromLeft (gainCellWidth), gain, gainLabel);
    setKnob (gainKnobs.removeFromLeft (gainCellWidth), inputGain, inputGainLabel);
    setKnob (gainKnobs, outputGain, outputGainLabel);

    controlsContent.removeFromTop (2);
    auto gainOptions = controlsContent.removeFromTop (56).reduced (18, 0);
    autoGain.setBounds (gainOptions.removeFromTop (22));
    gainOptions.removeFromTop (2);
    loudnessMatchMode.setBounds (gainOptions.removeFromTop (28));

    controlsContent.removeFromTop (2);
    phaseModeLabel.setBounds (controlsContent.removeFromTop (16));
    phaseMode.setBounds (controlsContent.removeFromTop (28));
    controlsContent.removeFromTop (3);
    auto switches = controlsContent.removeFromTop (28);
    lockMode.setBounds (switches.removeFromLeft (74).withHeight (28));
    limiter.setBounds (switches.removeFromLeft (78).withHeight (28));
    bypass.setBounds (switches.removeFromLeft (82).withHeight (28));
    includeOutputGainFir.setBounds (controlsContent.removeFromTop (20));
    includeAutoGainFir.setBounds (controlsContent.removeFromTop (20));
    independentLrPreamp.setBounds (controlsContent.removeFromTop (20));

    meterPanel->setBounds (levelsContentArea.reduced (0, 2));

    area.removeFromTop (10);
    tabs.setBounds (area);
}

void FlexCurveAudioProcessorEditor::timerCallback()
{
    const auto samples = processor.getActiveLatencySamples();
    const auto sampleRate = processor.getCurrentSampleRate() > 1.0 ? processor.getCurrentSampleRate() : 48000.0;
    const auto ms = 1000.0 * static_cast<double> (samples) / sampleRate;
    status.setText (processor.getStatusText() + " | " + juce::String (samples) + " samples / " + juce::String (ms, 1) + " ms"
                    + " | IR peak " + juce::String (processor.getActiveImpulsePeakSamples()),
                    juce::dontSendNotification);
    processor.setLastOpenTabIndex (tabs.getCurrentTabIndex());
    graph.repaint();
    graph.setVariableEditingAllowed (tabs.getCurrentTabIndex() == 1 && ! processor.isEditLocked());
    const auto phaseSelectable = processor.canSelectPhaseMode();
    phaseMode.setEnabled (phaseSelectable);
    exportFir.setEnabled (processor.canExportRenderedFir());
    lockMode.setToggleState (processor.isEditLocked(), juce::dontSendNotification);
    const auto locked = processor.isEditLocked();
    lockMode.setTooltip (locked ? "Editing locked. Click to unlock." : "Editing unlocked. Click to lock.");
    undoButton.setEnabled (! locked && processor.canUndo());
    redoButton.setEnabled (! locked && processor.canRedo());
    smoothAll.setEnabled (! locked && ! processor.getLayers().empty());
    smoothAll.setToggleState (processor.areAllGraphicCurvesSmoothed(), juce::dontSendNotification);
    addCurve.setEnabled (! locked && processor.canAddUserLayer());
    newFlat.setEnabled (! locked && processor.canAddUserLayer());
    renderFir.setEnabled (! locked);
    resetFlat.setEnabled (! locked);
    resetAll.setEnabled (! locked);
    const auto range = processor.getGlobalDbRange();
    gain.setRange (-range, range, 0.01);
    meterPanel->setSnapshot (processor.getMeterSnapshot());
    loudnessMatchMode.setEnabled (autoGain.getToggleState());
    const auto rangeId = range == 12.0f || range == 24.0f || range == 36.0f || range == 48.0f
                       ? static_cast<int> (range) : 0;
    if (! dbScale.hasKeyboardFocus (true))
    {
        if (rangeId > 0)
            dbScale.setSelectedId (rangeId, juce::dontSendNotification);
        else
            dbScale.setText (juce::String::charToString (0x00b1) + juce::String (range, 1) + " dB", juce::dontSendNotification);
    }
    if (! phaseSelectable && phaseMode.getSelectedId() != 1)
        phaseMode.setSelectedId (1, juce::dontSendNotification);
    updateActiveLayerCombo();
    updateAddButtons();
    if (globalLayerRack != nullptr && globalLayerRack->sync())
        resized();
}

void FlexCurveAudioProcessorEditor::updateActiveLayerCombo()
{
    const auto currentId = processor.getActiveLayerId();
    const auto layers = processor.getLayers();
    juce::String name = "none";
    if (currentId == -1)
        name = "Average (read-only)";
    else if (const auto it = std::find_if (layers.begin(), layers.end(), [currentId] (const auto& layer) { return layer.id == currentId; });
             it != layers.end())
    {
        const auto channel = it->selectedChannel == FlexChannelSelection::left ? "L"
                           : it->selectedChannel == FlexChannelSelection::right ? "R" : "L+R";
        name = it->name + " [" + channel + "]"
             + (it->channelsLinked ? " linked" : "");
    }

    const auto text = "Active Layer: " + name;
    if (activeLayerLabel.getText() != text)
    {
        activeLayerLabel.setText (text, juce::dontSendNotification);
        resized();
    }
}

void FlexCurveAudioProcessorEditor::updateAddButtons()
{
    const auto canAdd = processor.canAddUserLayer() && ! processor.isEditLocked();
    for (auto* button : { &addCurve, &newFlat })
    {
        button->setEnabled (canAdd);
        button->setColour (juce::TextButton::buttonColourId, canAdd ? juce::Colour (0xff202832) : juce::Colour (0xff171c22));
        button->setColour (juce::TextButton::textColourOffId, canAdd ? inkColour() : mutedColour().withAlpha (0.45f));
    }
}

void FlexCurveAudioProcessorEditor::updatePresetCombo()
{
    presetCombo.clear (juce::dontSendNotification);
    presetFiles.clear();

    presetCombo.addItem ("Select preset...", 1);
    presetCombo.addSeparator();

    auto files = processor.getPresetsDirectory().findChildFiles (juce::File::TypesOfFileToFind::findFiles,
                                                                  false,
                                                                  "*.flexcurvepreset;*.calcurvepreset;*.xml");
    files.sort();

    int itemId = 3;
    const auto currentName = processor.getPresetName().trim();
    for (const auto& file : files)
    {
        presetCombo.addItem (file.getFileNameWithoutExtension(), itemId);
        presetFiles.push_back (file);
        if (currentName.equalsIgnoreCase (file.getFileNameWithoutExtension()))
            presetCombo.setSelectedId (itemId, juce::dontSendNotification);
        ++itemId;
    }

    presetCombo.addSeparator();
    presetCombo.addItem ("Save current preset...", 9999);
    presetCombo.addItem ("Delete current preset...", 10000);

    if (presetCombo.getSelectedId() == 0)
        presetCombo.setSelectedId (1, juce::dontSendNotification);
}

void FlexCurveAudioProcessorEditor::handlePresetComboChange()
{
    const auto selectedId = presetCombo.getSelectedId();
    if (selectedId <= 1)
        return;

    if (selectedId == 9999)
    {
        presetCombo.setSelectedId (1, juce::dontSendNotification);
        auto* window = new juce::AlertWindow ("Save Preset", "Enter a name for this FlexCurve preset:", juce::AlertWindow::QuestionIcon);
        auto suggestedName = processor.getPresetName().trim();
        if (suggestedName.isEmpty() || suggestedName == "Untitled")
            suggestedName = "My FlexCurve Preset";
        window->addTextEditor ("presetName", suggestedName, "Preset Name:");
        window->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey, 0, 0));
        window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey, 0, 0));

        const juce::Component::SafePointer<FlexCurveAudioProcessorEditor> safeThis (this);
        window->enterModalState (true, juce::ModalCallbackFunction::create ([safeThis, window] (int result)
        {
            std::unique_ptr<juce::AlertWindow> windowToDelete (window);
            if (result != 1 || safeThis == nullptr)
                return;

            auto name = window->getTextEditorContents ("presetName").trim();
            if (name.isEmpty())
                return;

            auto safeName = name.replaceCharacters ("\\/:*?\"<>|", "_________");
            auto file = safeThis->processor.getPresetsDirectory().getChildFile (safeName + ".flexcurvepreset");
            if (! safeThis->processor.savePresetToFile (file, safeName))
                juce::AlertWindow::showMessageBoxAsync (juce::AlertWindow::WarningIcon, "Save Preset", "Could not save the preset.");
            safeThis->updatePresetCombo();
        }), true);
        return;
    }

    if (selectedId == 10000)
    {
        presetCombo.setSelectedId (1, juce::dontSendNotification);
        auto activeName = processor.getPresetName().trim();
        if (activeName.isEmpty() || activeName == "Untitled")
        {
            juce::AlertWindow::showMessageBoxAsync (juce::AlertWindow::WarningIcon, "Delete Preset", "No custom preset is currently loaded.");
            return;
        }

        auto file = processor.getPresetsDirectory().getChildFile (activeName + ".flexcurvepreset");
        if (! file.existsAsFile())
            file = processor.getPresetsDirectory().getChildFile (activeName + ".calcurvepreset");

        if (! file.existsAsFile())
        {
            juce::AlertWindow::showMessageBoxAsync (juce::AlertWindow::WarningIcon, "Delete Preset", "The current preset file was not found.");
            return;
        }

        const juce::Component::SafePointer<FlexCurveAudioProcessorEditor> safeThis (this);
        juce::AlertWindow::showOkCancelBox (juce::AlertWindow::QuestionIcon,
                                            "Delete Preset",
                                            "Delete preset \"" + activeName + "\"?",
                                            "Delete",
                                            "Cancel",
                                            nullptr,
                                            juce::ModalCallbackFunction::create ([safeThis, file] (int result) mutable
                                            {
                                                if (result != 0 && safeThis != nullptr)
                                                {
                                                    file.deleteFile();
                                                    safeThis->updatePresetCombo();
                                                }
                                            }));
        return;
    }

    const auto index = selectedId - 3;
    if (index >= 0 && index < static_cast<int> (presetFiles.size()))
    {
        if (! processor.loadPresetFromFile (presetFiles[static_cast<size_t> (index)]))
            juce::AlertWindow::showMessageBoxAsync (juce::AlertWindow::WarningIcon, "Load Preset", "Could not load the preset.");
        updatePresetCombo();
        updateActiveLayerCombo();
        updateAddButtons();
        if (globalLayerRack != nullptr)
        {
            globalLayerRack->sync();
            globalLayerRack->resized();
            globalLayerRack->repaint();
        }
        resized();
        graph.repaint();
    }
}

void FlexCurveAudioProcessorEditor::openAddCurveChooser()
{
    chooser = std::make_unique<juce::FileChooser> ("Add TXT / CSV correction curve or FIR WAV",
                                                   juce::File{},
                                                   "*.txt;*.csv;*.wav;*.fir");
    const juce::Component::SafePointer<FlexCurveAudioProcessorEditor> safeThis (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectMultipleItems,
        [safeThis] (const juce::FileChooser& fc)
        {
            if (safeThis == nullptr)
                return;
            int imported = 0;
            int skipped = 0;
            for (const auto& file : fc.getResults())
            {
                if (! safeThis->processor.canAddUserLayer())
                {
                    ++skipped;
                    continue;
                }
                if (file.existsAsFile())
                {
                    if (safeThis->processor.addCurveFile (file))
                        ++imported;
                    else
                        ++skipped;
                }
            }
            safeThis->updateActiveLayerCombo();
            safeThis->updateAddButtons();
            if (safeThis->globalLayerRack != nullptr)
            {
                safeThis->globalLayerRack->sync();
                safeThis->globalLayerRack->resized();
                safeThis->globalLayerRack->repaint();
            }
            safeThis->resized();
            safeThis->graph.repaint();
            if (skipped > 0)
                juce::AlertWindow::showMessageBoxAsync (juce::AlertWindow::InfoIcon,
                                                        "Import Curve",
                                                        "Imported " + juce::String (imported) + " file(s). Skipped "
                                                            + juce::String (skipped) + " file(s) because the layer limit is 6 or the file could not be parsed.");
        });
}

void FlexCurveAudioProcessorEditor::openExportFirChooser()
{
    chooser = std::make_unique<juce::FileChooser> ("Export FlexCurve FIR WAV",
                                                   juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("FlexCurve_FIR.wav"),
                                                   "*.wav");
    const juce::Component::SafePointer<FlexCurveAudioProcessorEditor> safeThis (this);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
        [safeThis] (const juce::FileChooser& fc)
        {
            if (safeThis == nullptr)
                return;
            auto file = fc.getResult();
            if (file == juce::File{})
                return;
            if (! file.hasFileExtension ("wav"))
                file = file.withFileExtension ("wav");
            if (! safeThis->processor.exportCurrentFirToFile (file))
                juce::AlertWindow::showMessageBoxAsync (juce::AlertWindow::WarningIcon, "Export FIR", "Could not export the current FIR.");
        });
}

void FlexCurveAudioProcessorEditor::showHelp()
{
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (new HelpContent());
    options.dialogTitle = "FlexCurve Help";
    options.dialogBackgroundColour = juce::Colour (0xff12171d);
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    options.launchAsync();
}
