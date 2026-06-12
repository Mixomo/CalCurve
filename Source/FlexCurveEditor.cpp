#include "FlexCurveEditor.h"
#include <limits>

namespace
{
    juce::Colour panelColour() { return juce::Colour (0xff101419); }
    juce::Colour surfaceColour() { return juce::Colour (0xff151b22); }
    juce::Colour inkColour() { return juce::Colour (0xffeff5f3); }
    juce::Colour mutedColour() { return juce::Colour (0xff9eabb8); }
    juce::Colour accentColour() { return juce::Colour (0xff27d7a4); }

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

    std::vector<CurvePoint> graphScalePointsFor (FlexCurveAudioProcessor& processor)
    {
        const auto dryWet = processor.parameters.getRawParameterValue ("drywet")->load();
        const auto gainDb = processor.parameters.getRawParameterValue ("gain")->load();
        const auto bypassed = processor.parameters.getRawParameterValue ("bypass")->load() > 0.5f;
        auto displayAudioCurve = [dryWet, gainDb, bypassed] (std::vector<CurvePoint> points)
        {
            for (auto& point : points)
                point.db = bypassed ? 0.0 : point.db * dryWet + gainDb;
            return points;
        };

        auto points = displayAudioCurve (processor.getFinalCurve());
        const auto layers = processor.getLayers();
        for (const auto& layer : layers)
        {
            if (! layer.visible)
                continue;
            if (processor.isLayerTypeVisible (layer.type))
            {
                const auto curve = layer.type == FlexCurveLayerType::eq
                    ? displayAudioCurve (processor.getLayerCurve (layer.id))
                    : processor.getLayerCurveForDisplay (layer.id);
                points.insert (points.end(), curve.begin(), curve.end());
            }
            if (processor.areCorrectedMeasurementsVisible()
                && layer.type == FlexCurveLayerType::eq && layer.autoEqRawLayerId > 0)
            {
                const auto corrected = processor.getCorrectedMeasurementCurve (layer.id);
                points.insert (points.end(), corrected.begin(), corrected.end());
            }
        }
        if (processor.isAverageVisible())
        {
            const auto average = displayAudioCurve (processor.getAverageCurve());
            points.insert (points.end(), average.begin(), average.end());
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
                const auto average = processor.getAverageCurveForTypeForDisplay (type);
                points.insert (points.end(), average.begin(), average.end());
            }
        }
        return points;
    }

    std::vector<CurvePoint> graphDisplayAudioCurveFor (FlexCurveAudioProcessor& processor,
                                                        std::vector<CurvePoint> points)
    {
        const auto dryWet = processor.parameters.getRawParameterValue ("drywet")->load();
        const auto gainDb = processor.parameters.getRawParameterValue ("gain")->load();
        const auto bypassed = processor.parameters.getRawParameterValue ("bypass")->load() > 0.5f;
        for (auto& point : points)
            point.db = bypassed ? 0.0 : point.db * dryWet + gainDb;
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
            "- Equalizer APO parametric EQ text\n\n"
            "LAYERS AND BLEND\n\n"
            "- Import Curve as a New Layer loads CSV, TXT, or WAV FIR.\n"
            "- Explicit Preamp/global gain from text curves is separated into the layer Gain slider instead of being hidden in curve points. FlexCurve FIR exports preserve that gain in WAV metadata; external FIRs without explicit metadata keep their measured response and default to 0.0 dB layer gain.\n"
            "- Add Curve as a New Layer creates a flat editable layer.\n"
            "- Up to six user layers can coexist. Each has an embedded source curve, color, custom name, visibility, mute, solo, gain, Blend settings, Graphic/Variable EQ, and Parametric EQ.\n"
            "- Clone duplicates a layer's embedded curve, EQ banks, Variable points, Parametric filters, Blend, smoothing, inversion, and visibility state under a new name and color.\n"
            "- The persistent color rack is available in every tab: V controls graph visibility, M excludes a layer from audio and Average, and S restricts audio and Average to soloed layers. Click the color circle to make that layer active.\n"
            "- Average is the read-only white result derived live from audible layers and is used by preview and Render FIR.\n"
            "- Per Layer Blend applies Bass, Mid, Treble, and crossover settings to the active EQ layer. Global Blend keeps a separate regional-trim state and applies it to all EQ layers. Layer Gain always remains an independent per-layer control.\n"
            "- Invert reverses a layer correction around 0 dB.\n\n"
            "GRAPHIC AND VARIABLE EQ\n\n"
            "- Graphic EQ provides editable 15-band and 31-band modes.\n"
            "- The 15-band and 31-band modes have independent gain banks. Switching modes never reinterprets or overwrites the other mode's faders.\n"
            "- Variable mode uses points on the active layer. Double-click adds a point; drag changes frequency/gain; Ctrl-click and drag paints the curve continuously; Shift-click and drag-box select several; Delete/Backspace removes them; arrows and mouse wheel adjust gain.\n"
            "- Numeric Frequency/Gain fields are scrollable and synchronized with graph selection.\n"
            "- Variable editing and processing are enabled only while Graphic EQ is enabled and Variable mode is selected. Switching to 15/31 bands bypasses stored Variable points unless Preserve Variable Shape is enabled explicitly.\n"
            "- Smooth Curve applies non-destructive smoothing to the active layer's Graphic/Variable contribution. Smooth All Layers enables smoothing across the project.\n"
            "- Copy EQ, Paste EQ, and Paste Inverted EQ work within the same layer or across layers. 15-band EQ can paste into 15, 31, or Variable mode; 31-band EQ can paste into 31 or Variable; Variable EQ can paste only into Variable mode. Ctrl-click toggles individual fixed bands, Shift-click selects a continuous range, and double-click resets the selected bands to 0 dB.\n\n"
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
            "- Crossfeed reduces hard headphone stereo separation.\n"
            "- Gain is a bipolar final output trim and moves audible EQ/Average curves. It does not rewrite or reposition RAW, Target, or Tracking references.\n"
            "- Input Gain is applied before correction and before the Input meter.\n"
            "- Output Gain is applied after correction and Auto Gain, before the final Global Gain stage.\n"
            "- Auto Gain is an output trim for honest level-matched A/B monitoring. It does not change Input Gain, compress, reshape transients, or alter the FIR.\n"
            "- Match Output to Input follows the RMS difference between corrected and input audio. It may boost or cut, but every audio block constrains positive compensation to its current peak headroom so previously learned gain cannot clip OUT after a curve or layer change.\n"
            "- Downward Match never boosts. It also remembers the greatest required peak reduction and only ratchets downward until Auto Gain is reset, disabled, or its mode changes.\n"
            "- The meter shows IN, PRE, and OUT RMS/Peak. PRE is measured after correction and manual output stages but before Auto Gain, so correction-induced clipping remains visible even when the final output is attenuated safely.\n"
            "- Reset Meters clears held meter and clip displays without changing the currently learned Auto Gain trim.\n"
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
            "- The top Export FIR button is enabled only for a current rendered FIR and writes a mono 32-bit WAV at the host sample rate.\n"
            "- Each Blend layer and Average can be exported independently as FIR WAV, frequency/dB TXT or CSV, GraphicEQ/APO text, Melda CSV, or APO parametric text. Parametric export approximates the complete displayed curve with a practical filter set.\n"
            "- GraphicEQ/APO exports preserve layer gain as Preamp. FlexCurve FIR WAV exports preserve it as metadata. Other curve formats bake the gain into their exported dB values, so it is never lost or duplicated.\n\n"
            "PRESETS\n\n"
            "- The Presets menu loads, saves, and deletes .flexcurvepreset files in %APPDATA%\\Mixomo\\FlexCurve\\UserPresets\\.\n"
            "- Compatible .calcurvepreset and XML state files can be loaded for migration from CalCurve.\n"
            "- Presets embed every source curve plus names, colors, V/M/S, layer gains, normalization offsets, source smoothing, Blend, Graphic/Variable EQ, Parametric filters, active layer, Average state, Input/Output Gain, Auto Gain mode, FIR-export options, global controls, phase, and rendered state where available.\n"
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
            if (layer.type == FlexCurveLayerType::target) ++targetCount;
            if (layer.type == FlexCurveLayerType::raw) ++rawCount;
            if (layer.type == FlexCurveLayerType::eq && layer.autoEqRawLayerId > 0) ++count;
        }
    if (targetCount >= 2) ++count;
    if (rawCount >= 2) ++count;
    return count > 0 ? (count + 5) / 6 : 0;
}

juce::Rectangle<float> FlexCurveGraph::getPlotBounds() const
{
    auto graph = getLocalBounds().toFloat().reduced (16.0f, 12.0f);
    graph.removeFromBottom (26.0f);
    const auto rows = getLegendRowCount();
    if (rows > 0)
        graph.removeFromTop (static_cast<float> (rows * 22 + 4));
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
        const auto curve = layer.type == FlexCurveLayerType::eq
            ? graphDisplayAudioCurveFor (processor, processor.getLayerCurve (layer.id))
            : processor.getLayerCurveForDisplay (layer.id);
        if (curve.empty())
            continue;
        const auto alpha = layer.type == FlexCurveLayerType::raw ? 0.46f : 1.0f;
        g.setColour ((layer.muted ? layer.colour.darker (0.55f) : layer.colour).withAlpha (alpha));
        drawLayerPath (layer, curve, 1.55f);
    }

    if (processor.isAverageVisible())
    {
        const auto average = graphDisplayAudioCurveFor (processor, processor.getAverageCurve());
        if (! average.empty())
        {
            g.setColour (juce::Colours::white);
            g.strokePath (buildPath (average, graph, minDb, maxDb), juce::PathStrokeType (2.0f));
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
        const auto average = processor.getAverageCurveForTypeForDisplay (FlexCurveLayerType::target);
        juce::Path dashed;
        const float pattern[] { 7.0f, 4.0f };
        juce::PathStrokeType (1.8f).createDashedStroke (dashed, buildPath (average, graph, minDb, maxDb), pattern, 2);
        g.setColour (juce::Colours::white.withAlpha (0.72f));
        g.fillPath (dashed);
    }
    if (visibleRaws >= 2)
    {
        const auto average = processor.getAverageCurveForTypeForDisplay (FlexCurveLayerType::raw);
        g.setColour (juce::Colours::lightgrey.withAlpha (0.56f));
        g.strokePath (buildPath (average, graph, minDb, maxDb), juce::PathStrokeType (1.15f));
    }

    for (const auto& layer : layers)
    {
        if (! processor.areCorrectedMeasurementsVisible()
            || layer.type != FlexCurveLayerType::eq || layer.autoEqRawLayerId <= 0 || ! layer.visible)
            continue;
        const auto corrected = processor.getCorrectedMeasurementCurve (layer.id);
        if (corrected.empty())
            continue;
        const auto path = buildPath (corrected, graph, minDb, maxDb);
        juce::Path dashed;
        const float pattern[] { 3.0f, 3.0f };
        juce::PathStrokeType (1.7f).createDashedStroke (dashed, path, pattern, 2);
        g.setColour (layer.colour.darker (0.55f));
        g.fillPath (dashed);
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
            const auto activeCurve = activeIt != layers.end() && activeIt->type == FlexCurveLayerType::eq
                ? graphDisplayAudioCurveFor (processor, processor.getLayerCurve (activeLayerId))
                : processor.getLayerCurveForDisplay (activeLayerId);
            if (! activeCurve.empty())
            {
                auto activeColour = juce::Colours::white;
                for (const auto& layer : layers)
                    if (layer.id == activeLayerId)
                        activeColour = layer.colour;
                if (activeIt != layers.end())
                {
                    g.setColour (activeColour.withAlpha (activeIt->type == FlexCurveLayerType::raw ? 0.78f : 1.0f));
                    drawLayerPath (*activeIt, activeCurve, 2.8f);
                }
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
    const auto legendCount = static_cast<int> (legendLayers.size()) + (showAverageLegend ? 1 : 0)
                           + (visibleTargets >= 2 ? 1 : 0) + (visibleRaws >= 2 ? 1 : 0)
                           + static_cast<int> (trackingLegendLayers.size());
    auto legendArea = bounds.reduced (80.0f, 12.0f);
    legendArea = legendArea.removeFromTop (static_cast<float> (getLegendRowCount() * 22));
    g.setFont (juce::FontOptions (10.5f));
    const auto columns = juce::jmin (6, juce::jmax (1, legendCount));
    const auto itemWidth = legendArea.getWidth() / static_cast<float> (columns);
    int legendIndex = 0;
    auto drawLegendItem = [&] (juce::String text, juce::Colour colour, int id, bool active, bool muted)
    {
        const auto row = legendIndex / 6;
        const auto column = legendIndex % 6;
        auto item = juce::Rectangle<float> (legendArea.getX() + column * itemWidth,
                                            legendArea.getY() + row * 22.0f,
                                            itemWidth,
                                            20.0f);
        legendHitBoxes.push_back ({ item, id });
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
        drawLegendItem (prefix + layer->name, layer->colour, layer->id,
                        layer->id == activeLayerId, layer->muted);
    }
    for (const auto* layer : trackingLegendLayers)
        drawLegendItem ("Corrected: " + layer->name
                            + (layer->autoEqSourcesOutdated ? " (source changed)" : ""),
                        layer->colour.darker (0.55f),
                        0, false, layer->muted);
    if (showAverageLegend)
    {
        drawLegendItem ("Average", juce::Colours::white, -1, activeLayerId == -1, false);
    }
    if (visibleTargets >= 2)
        drawLegendItem ("Average Target", juce::Colours::white.withAlpha (0.72f), 0, false, false);
    if (visibleRaws >= 2)
        drawLegendItem ("Average RAW", juce::Colours::lightgrey.withAlpha (0.70f), 0, false, false);

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
        if (hit.first.contains (e.position))
        {
            processor.setActiveLayerId (hit.second);
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

        blendModeLabel.setText ("Blend Apply Mode:", juce::dontSendNotification);
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
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (mutedColour());
        if (processor.getLayers().empty())
            g.drawText ("Import a curve or add a flat layer to start.", layerArea, juce::Justification::centred);
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

        area.removeFromTop (4);
        auto modeRow = area.removeFromTop (32);
        blendModeLabel.setBounds (modeRow.removeFromLeft (126));
        perLayer.setBounds (modeRow.removeFromLeft (100));
        globalAverage.setBounds (modeRow.removeFromLeft (178));
        reset.setBounds (modeRow.removeFromRight (90));

        area.removeFromTop (6);
        auto importRow = area.removeFromTop (34);
        importTarget.setBounds (importRow.removeFromLeft (150));
        importRow.removeFromLeft (10);
        importRaw.setBounds (importRow.removeFromLeft (150));

        area.removeFromTop (6);
        auto normalizeRow = area.removeFromTop (34);
        normalizeReferences.setBounds (normalizeRow.removeFromLeft (218));
        normalizeRow.removeFromLeft (6);
        normalizeFrequency.setBounds (normalizeRow.removeFromLeft (72));
        normalizeHzLabel.setBounds (normalizeRow.removeFromLeft (30));
        normalizeReferencesHelp.setBounds (normalizeRow.removeFromLeft (34));
        normalizeRow.removeFromLeft (12);
        normalizeLayersToZero.setBounds (normalizeRow.removeFromLeft (210));

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
            gain.setSliderStyle (juce::Slider::LinearHorizontal);
            gain.setTextBoxStyle (juce::Slider::TextBoxRight, false, 74, 22);
            const auto range = static_cast<double> (processor.getGlobalDbRange());
            gain.setRange (-range, range, 0.1);
            gain.setTextValueSuffix (" dB");
            gain.setColour (juce::Slider::thumbColourId, accentColour());
            gain.setColour (juce::Slider::trackColourId, juce::Colour (0xff33404c));
            addAndMakeVisible (gain);
            installSliderReset (gain, 0.0);
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
            smooth.setToggleState (it->smoothSourceCurve, juce::dontSendNotification);
            if (! gain.isMouseButtonDown())
            {
                const auto range = processor.getGlobalDbRange();
                gain.setRange (-range, range, 0.1);
                gain.setValue (it->gainDb, juce::dontSendNotification);
            }
            const auto isEq = layerType == FlexCurveLayerType::eq;
            mute.setVisible (isEq);
            solo.setVisible (isEq);
            gain.setVisible (true);
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
            reset.setEnabled (editable);
            invert.setEnabled (editable && isEq);
            remove.setEnabled (editable);
            clone.setVisible (true);
            clone.setEnabled (editable && processor.canAddLayerType (layerType));
            smooth.setVisible (true);
            smooth.setEnabled (editable);
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
            visible.setBounds (area.removeFromLeft (34));
            const auto isEq = ! isAverage && layerType == FlexCurveLayerType::eq;
            if (isEq)
            {
                mute.setBounds (area.removeFromLeft (38));
                solo.setBounds (area.removeFromLeft (38));
            }
            name.setBounds (area.removeFromLeft (200));
            if (isAverage || ! isEq)
                gain.setBounds (area.removeFromLeft (220));
            else
                gain.setBounds (area.removeFromLeft (210));
            if (isEq)
                invert.setBounds (area.removeFromLeft (58).reduced (2, 0));
            if (! isAverage)
                clone.setBounds (area.removeFromLeft (58).reduced (2, 0));
            if (! isAverage)
                smooth.setBounds (area.removeFromLeft (70).reduced (2, 0));
            exportCurve.setBounds (area.removeFromLeft (66).reduced (2, 0));
            if (! isAverage)
                reset.setBounds (area.removeFromLeft (58).reduced (2, 0));
            remove.setBounds (area.removeFromRight (32));
        }

        FlexCurveAudioProcessor& processor;
        int layerId;
        bool isAverage;
        FlexCurveLayerType layerType = FlexCurveLayerType::eq;
        juce::Colour colour;
        juce::TextEditor name;
        juce::ToggleButton visible, mute, solo;
        FlexCurveResettableSlider gain;
        juce::TextButton reset, invert, clone, exportCurve, remove;
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
        addAndMakeVisible (pointViewport);

        hint.setColour (juce::Label::textColourId, mutedColour());
        hint.setFont (juce::FontOptions (15.0f));
        hint.setJustificationType (juce::Justification::centred);
        hint.setText ("Choose an active layer or add a new layer to edit Graphic EQ.", juce::dontSendNotification);
        addAndMakeVisible (hint);

        rebuildControls();
        startTimerHz (10);
    }

    ~GraphicTab() override
    {
        stopTimer();
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
        hint.setVisible (! hasLayer);
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
        hint.setText ("Choose an active layer or add a new layer to edit Parametric EQ.", juce::dontSendNotification);
        addAndMakeVisible (hint);
        rebuild();
        startTimerHz (10);
    }

    ~ParametricTab() override
    {
        stopTimer();
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
        hint.setVisible (! hasLayer);
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

class FlexCurveAudioProcessorEditor::GlobalLayerRack final : public juce::Component
{
public:
    explicit GlobalLayerRack (FlexCurveAudioProcessor& p) : processor (p)
    {}

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
        if (rows.empty())
            return;

        const auto width = juce::jmin (118, area.getWidth() / static_cast<int> (rows.size()));
        for (auto& row : rows)
            row->setBounds (area.removeFromLeft (juce::jmin (width, area.getWidth())).reduced (2, 1));
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
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            juce::ignoreUnused (g);
        }

        void resized() override
        {
            auto area = getLocalBounds();
            colourButton.setBounds (area.removeFromLeft (30));
            visible.setBounds (area.removeFromLeft (28));
            mute.setBounds (area.removeFromLeft (28));
            solo.setBounds (area.removeFromLeft (28));
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
    };

    FlexCurveAudioProcessor& processor;
    juce::String signature;
    std::vector<std::unique_ptr<Row>> rows;
};

class FlexCurveAudioProcessorEditor::MeterPanel final : public juce::Component
{
public:
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

        auto footer = content.removeFromBottom (42.0f);
        auto scale = content.removeFromLeft (28.0f);
        auto bars = content.reduced (4.0f, 2.0f);
        const auto gap = 6.0f;
        const auto barWidth = juce::jmax (7.0f, (bars.getWidth() - gap * 5.0f) / 6.0f);
        const std::array<float, 6> values {
            snapshot.inputRmsDb, snapshot.inputPeakDb,
            snapshot.preAutoRmsDb, snapshot.preAutoPeakDb,
            snapshot.outputRmsDb, snapshot.outputPeakDb
        };
        const std::array<juce::String, 6> labels { "R", "P", "R", "P", "R", "P" };

        g.setFont (juce::FontOptions (9.5f));
        for (const auto db : { 0.0f, -12.0f, -24.0f, -36.0f, -48.0f, -60.0f })
        {
            const auto y = juce::jmap (db, -60.0f, 0.0f, bars.getBottom(), bars.getY());
            g.setColour (juce::Colour (0xff2a333d));
            g.drawHorizontalLine (static_cast<int> (std::round (y)), bars.getX(), bars.getRight());
            g.setColour (mutedColour());
            g.drawText (juce::String (static_cast<int> (db)), static_cast<int> (scale.getX()), static_cast<int> (y - 7.0f),
                        static_cast<int> (scale.getWidth() - 3.0f), 14, juce::Justification::centredRight);
        }

        for (size_t i = 0; i < values.size(); ++i)
        {
            auto bar = juce::Rectangle<float> (bars.getX() + static_cast<float> (i) * (barWidth + gap),
                                               bars.getY(), barWidth, bars.getHeight());
            g.setColour (juce::Colour (0xff111820));
            g.fillRect (bar);
            g.setColour (juce::Colour (0xff3a4652));
            g.drawRect (bar, 1.0f);

            const auto level = juce::jlimit (0.0f, 1.0f, juce::jmap (values[i], -60.0f, 0.0f, 0.0f, 1.0f));
            auto fill = bar;
            fill.removeFromTop (fill.getHeight() * (1.0f - level));
            juce::ColourGradient gradient (juce::Colour (0xff46d5bd), fill.getBottomLeft(),
                                           juce::Colour (0xfff5d34f), fill.getTopLeft(), false);
            gradient.addColour (0.72, juce::Colour (0xff58dd62));
            gradient.addColour (0.90, juce::Colour (0xffffa43a));
            gradient.addColour (1.0, juce::Colour (0xffff4d52));
            g.setGradientFill (gradient);
            g.fillRect (fill);

            const auto clipped = i < 2 ? snapshot.inputClipped
                               : i < 4 ? snapshot.preAutoClipped
                                       : snapshot.outputClipped;
            g.setColour (clipped ? juce::Colour (0xffff4d52) : inkColour());
            const auto valueText = values[i] <= -99.0f ? "-inf" : juce::String (values[i], 1);
            g.drawFittedText (valueText, bar.toNearestInt().withY (static_cast<int> (bars.getY() - 1.0f)).withHeight (14),
                              juce::Justification::centredTop, 1);
            g.setColour (mutedColour());
            g.drawText (labels[i], static_cast<int> (bar.getX()), static_cast<int> (bars.getBottom() + 3.0f),
                        static_cast<int> (bar.getWidth()), 14, juce::Justification::centred);
        }

        const auto groupWidth = barWidth * 2.0f + gap;
        g.setColour (inkColour());
        g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
        g.drawText ("IN", static_cast<int> (bars.getX()), static_cast<int> (bars.getBottom() + 18.0f),
                    static_cast<int> (groupWidth), 15, juce::Justification::centred);
        g.drawText ("PRE", static_cast<int> (bars.getX() + groupWidth + gap), static_cast<int> (bars.getBottom() + 18.0f),
                    static_cast<int> (groupWidth), 15, juce::Justification::centred);
        g.drawText ("OUT", static_cast<int> (bars.getX() + (groupWidth + gap) * 2.0f), static_cast<int> (bars.getBottom() + 18.0f),
                    static_cast<int> (groupWidth), 15, juce::Justification::centred);

        g.setFont (juce::FontOptions (9.5f));
        g.setColour (snapshot.inputClipped || snapshot.preAutoClipped || snapshot.outputClipped
                         ? juce::Colour (0xffff5a5f) : mutedColour());
        const auto clipText = snapshot.inputClipped ? "  IN CLIP"
                            : snapshot.preAutoClipped ? "  PRE CLIP"
                            : snapshot.outputClipped ? "  OUT CLIP" : "";
        g.drawFittedText ("Auto " + juce::String (snapshot.autoGainDb, 1) + " dB" + clipText,
                          footer.toNearestInt(), juce::Justification::centredBottom, 1);
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
        owner.globalControlsWidth = juce::jlimit (540, juce::jmax (540, owner.getWidth() - 600),
                                                  startControlsWidth - event.getDistanceFromDragStartX());
        owner.resized();
    }

private:
    FlexCurveAudioProcessorEditor& owner;
    int startHeight = 430;
    int startControlsWidth = 560;
};

FlexCurveAudioProcessorEditor::FlexCurveAudioProcessorEditor (FlexCurveAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p), graph (p)
{
    meterPanel = std::make_unique<MeterPanel>();
    graphResizeHandle = std::make_unique<GraphResizeHandle> (*this);
    addAndMakeVisible (scaledContent);
    setResizable (true, true);
    setResizeLimits (1008, 624, 2352, 1456);
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
    addAndMakeVisible (lockMode);
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

    styleSlider (dryWet);
    styleSlider (crossfeed);
    styleSlider (gain, " dB");
    styleSlider (inputGain, " dB");
    styleSlider (outputGain, " dB");
    dryWet.setTooltip ("Blend latency-aligned dry audio with the corrected signal");
    crossfeed.setTooltip ("Reduce hard left/right separation for headphone listening");
    gain.setTooltip ("Final monitoring gain. Never included in FIR rendering or export");
    inputGain.setTooltip ("Gain before correction and Input metering");
    outputGain.setTooltip ("Gain after correction and Auto Gain, before Global Gain");

    for (auto* label : { &dryWetLabel, &crossfeedLabel, &gainLabel, &inputGainLabel, &outputGainLabel,
                         &phaseModeLabel, &globalControlsLabel })
    {
        label->setColour (juce::Label::textColourId, inkColour());
        label->setFont (juce::FontOptions (13.5f));
        label->setJustificationType (juce::Justification::centred);
        addAndMakeVisible (*label);
    }
    dryWetLabel.setText ("Dry/Wet", juce::dontSendNotification);
    crossfeedLabel.setText ("Crossfeed", juce::dontSendNotification);
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
    addAndMakeVisible (phaseMode);

    styleToggle (limiter);
    styleToggle (bypass);
    styleToggle (autoGain);
    styleToggle (includeOutputGainFir);
    styleToggle (includeAutoGainFir);
    styleCombo (loudnessMatchMode);
    loudnessMatchMode.addItem ("Match Output to Input", 1);
    loudnessMatchMode.addItem ("Downward Match", 2);
    autoGain.setTooltip ("Slow transparent RMS level matching without compression or limiting");
    loudnessMatchMode.setTooltip ("Choose bidirectional matching or attenuation-only Downward Match");
    includeOutputGainFir.setTooltip ("Explicitly bake Output Gain into exported FIR files");
    includeAutoGainFir.setTooltip ("Explicitly bake the current learned Auto Gain into exported FIR files");
    styleButton (resetMeters);
    resetMeters.setTooltip ("Clear Peak/RMS and clipping displays without changing the audible Auto Gain");
    resetMeters.onClick = [this] { processor.resetMeters(); };
    addAndMakeVisible (*meterPanel);
    addAndMakeVisible (*graphResizeHandle);
    addAndMakeVisible (limiter);
    addAndMakeVisible (bypass);
    addAndMakeVisible (autoGain);
    addAndMakeVisible (loudnessMatchMode);
    addAndMakeVisible (includeOutputGainFir);
    addAndMakeVisible (includeAutoGainFir);
    addAndMakeVisible (resetMeters);

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

    installSliderReset (dryWet, 1.0);
    installSliderReset (crossfeed, 0.0);
    installSliderReset (gain, 0.0);
    installSliderReset (inputGain, 0.0);
    installSliderReset (outputGain, 0.0);

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
    addAndMakeVisible (slider);
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
    const auto scale = juce::jmin (static_cast<float> (getWidth()) / static_cast<float> (designWidth),
                                  static_cast<float> (getHeight()) / static_cast<float> (designHeight));
    const auto scaledWidth = static_cast<float> (designWidth) * scale;
    const auto scaledHeight = static_cast<float> (designHeight) * scale;
    const auto offsetX = (static_cast<float> (getWidth()) - scaledWidth) * 0.5f;
    const auto offsetY = (static_cast<float> (getHeight()) - scaledHeight) * 0.5f;
    scaledContent.setBounds (0, 0, designWidth, designHeight);
    scaledContent.setTransform (juce::AffineTransform::scale (scale).translated (offsetX, offsetY));

    auto area = juce::Rectangle<int> (0, 0, designWidth, designHeight).reduced (22);
    auto header = area.removeFromTop (126);
    auto firstRow = header.removeFromTop (38);
    title.setBounds (firstRow.removeFromLeft (210));
    presetCombo.setBounds (firstRow.removeFromLeft (220).withHeight (34).translated (0, 3));
    firstRow.removeFromLeft (12);
    help.setBounds (firstRow.removeFromRight (74).withHeight (36).translated (0, 2));
    firstRow.removeFromRight (10);
    status.setBounds (firstRow.translated (0, 8));

    auto actionRow = header.removeFromTop (36);
    newFlat.setBounds (actionRow.removeFromLeft (190).withHeight (32));
    actionRow.removeFromLeft (8);
    addCurve.setBounds (actionRow.removeFromLeft (220).withHeight (32));
    actionRow.removeFromLeft (8);
    renderFir.setBounds (actionRow.removeFromLeft (104).withHeight (32));
    actionRow.removeFromLeft (8);
    exportFir.setBounds (actionRow.removeFromLeft (104).withHeight (32));
    actionRow.removeFromLeft (14);
    resetFlat.setBounds (actionRow.removeFromLeft (150).withHeight (30));
    actionRow.removeFromLeft (8);
    resetAll.setBounds (actionRow.removeFromLeft (90).withHeight (30));
    uiScale.setBounds (actionRow.removeFromRight (82).withHeight (30));
    uiScaleLabel.setBounds (actionRow.removeFromRight (62).withHeight (30));

    auto secondRow = header.removeFromTop (34);
    zoomReset.setBounds (secondRow.removeFromRight (48).withHeight (28));
    secondRow.removeFromRight (4);
    zoomIn.setBounds (secondRow.removeFromRight (30).withHeight (28));
    secondRow.removeFromRight (4);
    zoomOut.setBounds (secondRow.removeFromRight (30).withHeight (28));
    secondRow.removeFromRight (10);
    dbScale.setBounds (secondRow.removeFromRight (112).withHeight (28));
    dbScaleLabel.setBounds (secondRow.removeFromRight (108).withHeight (28));
    secondRow.removeFromRight (10);
    redoButton.setBounds (secondRow.removeFromRight (32).withHeight (28));
    secondRow.removeFromRight (4);
    undoButton.setBounds (secondRow.removeFromRight (32).withHeight (28));
    secondRow.removeFromRight (6);
    smoothAll.setBounds (secondRow.removeFromRight (154).withHeight (28));
    secondRow.removeFromRight (10);
    const auto maxActiveLabelWidth = juce::jmax (220, secondRow.getWidth() - 80);
    const auto desiredActiveLabelWidth = 24 + activeLayerLabel.getText().length() * 8;
    const auto activeLabelWidth = juce::jlimit (220, maxActiveLabelWidth, desiredActiveLabelWidth);
    activeLayerLabel.setBounds (secondRow.removeFromLeft (activeLabelWidth));

    auto rackRow = header.removeFromTop (36);
    if (globalLayerRack != nullptr)
        globalLayerRack->setBounds (rackRow);

    const auto maxGraphHeight = juce::jmax (410, area.getHeight() - 180);
    graphSectionHeight = juce::jlimit (410, maxGraphHeight, graphSectionHeight);
    globalControlsWidth = juce::jlimit (540, juce::jmax (540, area.getWidth() - 600), globalControlsWidth);
    auto top = area.removeFromTop (graphSectionHeight);
    auto controls = top.removeFromRight (globalControlsWidth);
    top.removeFromRight (10);
    const auto graphBounds = top.reduced (0, 6);
    graph.setBounds (graphBounds);
    graphResizeHandle->setBounds (graphBounds.getRight() - 10, graphBounds.getBottom() - 10, 22, 22);
    graphResizeHandle->toFront (false);

    globalControlsLabel.setBounds (controls.removeFromTop (22));
    auto meterArea = controls.removeFromRight (200);
    meterPanel->setBounds (meterArea.reduced (5, 0));
    controls.removeFromRight (8);

    auto setKnob = [] (juce::Rectangle<int> cell, juce::Slider& slider, juce::Label& label)
    {
        auto labelArea = cell.removeFromBottom (22);
        slider.setBounds (cell);
        label.setBounds (labelArea);
    };
    constexpr int knobHeight = 110;
    auto firstKnobs = controls.removeFromTop (knobHeight);
    const auto firstCellWidth = firstKnobs.getWidth() / 3;
    setKnob (firstKnobs.removeFromLeft (firstCellWidth), dryWet, dryWetLabel);
    setKnob (firstKnobs.removeFromLeft (firstCellWidth), crossfeed, crossfeedLabel);
    setKnob (firstKnobs, gain, gainLabel);
    auto secondKnobs = controls.removeFromTop (knobHeight);
    const auto secondCellWidth = secondKnobs.getWidth() / 3;
    setKnob (secondKnobs.removeFromLeft (secondCellWidth), inputGain, inputGainLabel);
    setKnob (secondKnobs.removeFromLeft (secondCellWidth), outputGain, outputGainLabel);
    auto gainOptions = secondKnobs.reduced (4, 2);
    autoGain.setBounds (gainOptions.removeFromTop (24));
    loudnessMatchMode.setBounds (gainOptions.removeFromTop (30));
    resetMeters.setBounds (gainOptions.removeFromTop (26));

    controls.removeFromTop (4);
    phaseModeLabel.setBounds (controls.removeFromTop (18));
    phaseMode.setBounds (controls.removeFromTop (30));
    controls.removeFromTop (4);
    auto switches = controls.removeFromTop (28);
    lockMode.setBounds (switches.removeFromLeft (74));
    limiter.setBounds (switches.removeFromLeft (78));
    bypass.setBounds (switches.removeFromLeft (82));
    includeOutputGainFir.setBounds (controls.removeFromTop (20));
    includeAutoGainFir.setBounds (controls.removeFromTop (20));

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
        name = it->name;

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
