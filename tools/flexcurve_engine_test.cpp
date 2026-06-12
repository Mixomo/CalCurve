#include "../Source/FlexCurveProcessor.h"

#include <atomic>
#include <cmath>
#include <iostream>
#include <thread>

namespace
{
    void setParameterValue (FlexCurveAudioProcessor& processor, const juce::String& id, float actualValue)
    {
        if (auto* parameter = dynamic_cast<juce::RangedAudioParameter*> (processor.parameters.getParameter (id)))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (actualValue));
    }

    bool bufferIsFinite (const juce::AudioBuffer<float>& buffer)
    {
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                if (! std::isfinite (buffer.getSample (channel, sample)))
                    return false;
        return true;
    }

    double processNoise (FlexCurveAudioProcessor& processor, int blocks)
    {
        juce::Random random (0x5a17);
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        double inputEnergy = 0.0;
        double outputEnergy = 0.0;

        for (int block = 0; block < blocks; ++block)
        {
            for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
                for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                {
                    const auto value = 0.1f * (random.nextFloat() * 2.0f - 1.0f);
                    buffer.setSample (channel, sample, value);
                    inputEnergy += static_cast<double> (value) * value;
                }

            processor.processBlock (buffer, midi);
            if (! bufferIsFinite (buffer))
                return -1.0;

            for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
                for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                {
                    const auto value = buffer.getSample (channel, sample);
                    outputEnergy += static_cast<double> (value) * value;
                }
        }

        return std::sqrt (outputEnergy / juce::jmax (1.0, inputEnergy));
    }

    float processTonePeak (FlexCurveAudioProcessor& processor, float amplitude, int blocks)
    {
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        double phase = 0.0;
        float peak = 0.0f;
        for (int block = 0; block < blocks; ++block)
        {
            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            {
                const auto value = amplitude * static_cast<float> (std::sin (phase));
                phase += juce::MathConstants<double>::twoPi * 997.0 / 48000.0;
                for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
                    buffer.setSample (channel, sample, value);
            }
            processor.processBlock (buffer, midi);
            peak = juce::jmax (peak, buffer.getMagnitude (0, 0, buffer.getNumSamples()));
        }
        return peak;
    }

    void pumpMessageLoop (int milliseconds)
    {
        std::thread stopper ([milliseconds]
        {
            juce::Thread::sleep (milliseconds);
            juce::MessageManager::callAsync ([]
            {
                juce::MessageManager::getInstance()->stopDispatchLoop();
            });
        });
        juce::MessageManager::getInstance()->runDispatchLoop();
        stopper.join();
    }
}

int main (int argc, char* argv[])
{
    std::cout.setf (std::ios::unitbuf);

    if (argc < 2)
    {
        std::cout << "Usage: FlexCurveEngineTest <curve.txt|csv|wav>\n";
        return 1;
    }

    const juce::ScopedJuceInitialiser_GUI juceInitialiser;
    const juce::File curveFile { juce::String (argv[1]) };

    const auto tempDirectory = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                   .getChildFile ("FlexCurveGainRoundtrip_"
                                                  + juce::String::toHexString (
                                                      juce::Random::getSystemRandom().nextInt64()));
    if (! tempDirectory.createDirectory())
    {
        std::cout << "Could not create gain roundtrip test directory\n";
        return 32;
    }

    const auto gainCurveFile = tempDirectory.getChildFile ("explicit_gain.txt");
    if (! gainCurveFile.replaceWithText (
            "Preamp: -6.0 dB\n"
            "GraphicEQ: 20 1.0; 1000 2.0; 20000 3.0\n"))
    {
        std::cout << "Could not create explicit gain curve\n";
        return 33;
    }

    const auto flatRawFile = tempDirectory.getChildFile ("flat_raw.txt");
    const auto raisedTargetFile = tempDirectory.getChildFile ("raised_target.txt");
    if (! flatRawFile.replaceWithText ("GraphicEQ: 20 0.0; 500 0.0; 1000 0.0; 20000 0.0\n")
        || ! raisedTargetFile.replaceWithText (
            "GraphicEQ: 20 2.0; 500 4.0; 1000 6.0; 4000 3.0; 20000 1.0\n"))
    {
        std::cout << "Could not create AutoEQ peak-normalization fixtures\n";
        return 74;
    }

    const auto parsedGainCurve = CurveFIR::parseCurveFileWithGain (gainCurveFile);
    if (! parsedGainCurve.hasExplicitGain || std::abs (parsedGainCurve.gainDb + 6.0) > 0.001
        || std::abs (CurveFIR::interpolateDb (parsedGainCurve.points, 1000.0) - 2.0) > 0.001)
    {
        std::cout << "Text parser did not separate Preamp from curve geometry\n";
        return 34;
    }
    const auto parsedGlobalGain = CurveFIR::parseCurveTextWithGain (
        "Global Gain: 2.5 dB\n20 0\n20000 0\n");
    const auto parsedGainDirective = CurveFIR::parseCurveTextWithGain (
        "Gain: -1.25 dB\n20 0\n20000 0\n");
    if (! parsedGlobalGain.hasExplicitGain || std::abs (parsedGlobalGain.gainDb - 2.5) > 0.001
        || ! parsedGainDirective.hasExplicitGain || std::abs (parsedGainDirective.gainDb + 1.25) > 0.001)
    {
        std::cout << "Global Gain/Gain text directives were not recognized\n";
        return 68;
    }

    FlexCurveAudioProcessor gainProcessor;
    gainProcessor.prepareToPlay (48000.0, 512);
    if (! gainProcessor.addCurveFile (gainCurveFile))
    {
        std::cout << "Explicit gain layer import failed\n";
        return 35;
    }
    const auto gainLayers = gainProcessor.getLayers();
    if (gainLayers.size() != 1 || std::abs (gainLayers.front().gainDb + 6.0f) > 0.001f
        || std::abs (CurveFIR::interpolateDb (gainLayers.front().points, 1000.0) - 2.0) > 0.001
        || std::abs (CurveFIR::interpolateDb (gainProcessor.getLayerCurve (gainLayers.front().id), 1000.0) + 4.0) > 0.05)
    {
        std::cout << "Imported layer gain was hidden in geometry or applied twice\n";
        return 36;
    }

    const auto exportedGraphicEq = tempDirectory.getChildFile ("gain_roundtrip_graphiceq.txt");
    const auto exportedFir = tempDirectory.getChildFile ("gain_roundtrip.wav");
    if (! gainProcessor.exportLayerToFile (gainLayers.front().id, false,
                                           FlexCurveAudioProcessor::CurveExportFormat::graphicEq,
                                           exportedGraphicEq)
        || ! gainProcessor.exportLayerToFile (gainLayers.front().id, false,
                                              FlexCurveAudioProcessor::CurveExportFormat::firWav,
                                              exportedFir))
    {
        std::cout << "Gain-aware layer export failed\n";
        return 37;
    }

    const auto reparsedGraphicEq = CurveFIR::parseCurveFileWithGain (exportedGraphicEq);
    if (! reparsedGraphicEq.hasExplicitGain || std::abs (reparsedGraphicEq.gainDb + 6.0) > 0.001
        || std::abs (CurveFIR::interpolateDb (reparsedGraphicEq.points, 1000.0) - 2.0) > 0.1)
    {
        std::cout << "GraphicEQ export duplicated or lost layer gain\n";
        return 38;
    }

    FlexCurveAudioProcessor firGainProcessor;
    firGainProcessor.prepareToPlay (48000.0, 512);
    if (! firGainProcessor.addCurveFile (exportedFir))
    {
        std::cout << "Gain-aware FIR reimport failed\n";
        return 39;
    }
    const auto firGainLayers = firGainProcessor.getLayers();
    if (firGainLayers.size() != 1 || std::abs (firGainLayers.front().gainDb + 6.0f) > 0.01f)
    {
        std::cout << "FIR metadata did not restore layer gain\n";
        return 40;
    }

    setParameterValue (gainProcessor, "outputgain", 3.0f);
    setParameterValue (gainProcessor, "includeoutputgainfir", 1.0f);
    const auto exportedFirWithOutput = tempDirectory.getChildFile ("gain_roundtrip_with_output.wav");
    if (! gainProcessor.exportLayerToFile (gainLayers.front().id, false,
                                           FlexCurveAudioProcessor::CurveExportFormat::firWav,
                                           exportedFirWithOutput))
        return 53;
    FlexCurveAudioProcessor includedGainProcessor;
    includedGainProcessor.prepareToPlay (48000.0, 512);
    if (! includedGainProcessor.addCurveFile (exportedFirWithOutput)
        || includedGainProcessor.getLayers().size() != 1
        || std::abs (includedGainProcessor.getLayers().front().gainDb + 3.0f) > 0.05f)
    {
        std::cout << "Explicit Output Gain FIR export was lost or duplicated\n";
        return 54;
    }
    setParameterValue (gainProcessor, "outputgain", 0.0f);
    setParameterValue (gainProcessor, "includeoutputgainfir", 0.0f);

    setParameterValue (gainProcessor, "gain", 12.0f);
    const auto exportedFirWithGlobalGain = tempDirectory.getChildFile ("gain_roundtrip_global_gain_ignored.wav");
    if (! gainProcessor.exportLayerToFile (gainLayers.front().id, false,
                                           FlexCurveAudioProcessor::CurveExportFormat::firWav,
                                           exportedFirWithGlobalGain))
        return 55;
    FlexCurveAudioProcessor ignoredGlobalGainProcessor;
    ignoredGlobalGainProcessor.prepareToPlay (48000.0, 512);
    if (! ignoredGlobalGainProcessor.addCurveFile (exportedFirWithGlobalGain)
        || ignoredGlobalGainProcessor.getLayers().size() != 1
        || std::abs (ignoredGlobalGainProcessor.getLayers().front().gainDb + 6.0f) > 0.05f)
    {
        std::cout << "Global Gain leaked into layer FIR export\n";
        return 56;
    }
    setParameterValue (gainProcessor, "gain", 0.0f);

    setParameterValue (gainProcessor, "inputgain", 2.5f);
    setParameterValue (gainProcessor, "outputgain", -1.5f);
    setParameterValue (gainProcessor, "autogain", 0.0f);
    setParameterValue (gainProcessor, "loudnessmatchmode", 1.0f);
    setParameterValue (gainProcessor, "includeoutputgainfir", 1.0f);
    setParameterValue (gainProcessor, "includeautogainfir", 1.0f);

    juce::MemoryBlock gainState;
    gainProcessor.getStateInformation (gainState);
    FlexCurveAudioProcessor restoredGainProcessor;
    restoredGainProcessor.prepareToPlay (48000.0, 512);
    restoredGainProcessor.setStateInformation (gainState.getData(), static_cast<int> (gainState.getSize()));
    const auto restoredGainLayers = restoredGainProcessor.getLayers();
    if (restoredGainLayers.size() != 1 || std::abs (restoredGainLayers.front().gainDb + 6.0f) > 0.001f)
    {
        std::cout << "Preset/state roundtrip lost imported layer gain\n";
        return 41;
    }
    if (std::abs (restoredGainProcessor.parameters.getRawParameterValue ("inputgain")->load() - 2.5f) > 0.01f
        || std::abs (restoredGainProcessor.parameters.getRawParameterValue ("outputgain")->load() + 1.5f) > 0.01f
        || restoredGainProcessor.parameters.getRawParameterValue ("autogain")->load() > 0.5f
        || juce::roundToInt (restoredGainProcessor.parameters.getRawParameterValue ("loudnessmatchmode")->load()) != 1
        || restoredGainProcessor.parameters.getRawParameterValue ("includeoutputgainfir")->load() < 0.5f
        || restoredGainProcessor.parameters.getRawParameterValue ("includeautogainfir")->load() < 0.5f)
    {
        std::cout << "Preset/state roundtrip lost gain staging parameters\n";
        return 57;
    }
    setParameterValue (gainProcessor, "inputgain", 0.0f);
    setParameterValue (gainProcessor, "outputgain", 0.0f);
    setParameterValue (gainProcessor, "autogain", 1.0f);
    setParameterValue (gainProcessor, "loudnessmatchmode", 0.0f);
    setParameterValue (gainProcessor, "includeoutputgainfir", 0.0f);
    setParameterValue (gainProcessor, "includeautogainfir", 0.0f);

    gainProcessor.renderFir();
    processNoise (gainProcessor, 500);
    const auto autoGainSnapshot = gainProcessor.getMeterSnapshot();
    if (autoGainSnapshot.autoGainDb < 1.0f)
    {
        std::cout << "Auto Gain did not compensate a quieter processed curve: auto="
                  << autoGainSnapshot.autoGainDb << " dB inputRms=" << autoGainSnapshot.inputRmsDb
                  << " dB preRms=" << autoGainSnapshot.preAutoRmsDb << " dB\n";
        return 51;
    }
    gainProcessor.resetMeters();
    if (std::abs (gainProcessor.getMeterSnapshot().autoGainDb - autoGainSnapshot.autoGainDb) > 0.01f)
    {
        std::cout << "Reset Meters changed the audible Auto Gain state\n";
        return 58;
    }
    const auto matchedWetRatio = processNoise (gainProcessor, 220);
    setParameterValue (gainProcessor, "bypass", 1.0f);
    const auto bypassRatio = processNoise (gainProcessor, 64);
    setParameterValue (gainProcessor, "bypass", 0.0f);
    if (matchedWetRatio <= 0.0 || bypassRatio <= 0.0
        || std::abs (20.0 * std::log10 (matchedWetRatio / bypassRatio)) > 1.5)
    {
        std::cout << "Auto Gain did not keep processed/bypass A-B levels reasonably matched\n";
        return 62;
    }

    FlexCurveAudioProcessor downwardGainProcessor;
    downwardGainProcessor.prepareToPlay (48000.0, 512);
    if (! downwardGainProcessor.addCurveFile (gainCurveFile))
        return 59;
    downwardGainProcessor.setLayerGain (downwardGainProcessor.getLayers().front().id, 6.0f);
    setParameterValue (downwardGainProcessor, "loudnessmatchmode", 1.0f);
    downwardGainProcessor.renderFir();
    const auto downwardRatio = processNoise (downwardGainProcessor, 500);
    const auto downwardSnapshot = downwardGainProcessor.getMeterSnapshot();
    if (downwardSnapshot.autoGainDb > -1.0f)
    {
        std::cout << "Downward Match did not attenuate a louder processed signal: ratio="
                  << downwardRatio << " auto=" << downwardSnapshot.autoGainDb << " dB\n";
        return 52;
    }

    FlexCurveAudioProcessor headroomProcessor;
    headroomProcessor.prepareToPlay (48000.0, 512);
    setParameterValue (headroomProcessor, "autogain", 1.0f);
    setParameterValue (headroomProcessor, "loudnessmatchmode", 1.0f);
    setParameterValue (headroomProcessor, "outputgain", 12.0f);
    processTonePeak (headroomProcessor, 0.5f, 120);
    const auto headroomSnapshot = headroomProcessor.getMeterSnapshot();
    if (! headroomSnapshot.preAutoClipped || headroomSnapshot.outputClipped
        || headroomSnapshot.autoGainDb > -5.5f || headroomSnapshot.outputPeakDb > 0.05f)
    {
        std::cout << "Downward Match did not retain visible pre-auto clipping while protecting output: preClip="
                  << headroomSnapshot.preAutoClipped << " outClip=" << headroomSnapshot.outputClipped
                  << " auto=" << headroomSnapshot.autoGainDb
                  << " outPeak=" << headroomSnapshot.outputPeakDb << "\n";
        return 72;
    }
    setParameterValue (headroomProcessor, "autogain", 0.0f);
    headroomProcessor.resetMeters();
    processTonePeak (headroomProcessor, 0.5f, 8);
    if (! headroomProcessor.getMeterSnapshot().outputClipped)
    {
        std::cout << "Disabling Auto Gain did not expose the real clipped output\n";
        return 73;
    }

    FlexCurveAudioProcessor positiveAutoGainHeadroomProcessor;
    positiveAutoGainHeadroomProcessor.prepareToPlay (48000.0, 512);
    if (! positiveAutoGainHeadroomProcessor.addFlatCurve())
        return 75;
    const auto positiveHeadroomLayerId = positiveAutoGainHeadroomProcessor.getLayers().front().id;
    positiveAutoGainHeadroomProcessor.setLayerGain (positiveHeadroomLayerId, -12.0f);
    setParameterValue (positiveAutoGainHeadroomProcessor, "autogain", 1.0f);
    setParameterValue (positiveAutoGainHeadroomProcessor, "loudnessmatchmode", 0.0f);
    pumpMessageLoop (100);
    processTonePeak (positiveAutoGainHeadroomProcessor, 0.05f, 500);
    if (positiveAutoGainHeadroomProcessor.getMeterSnapshot().autoGainDb < 8.0f)
    {
        std::cout << "Positive Auto Gain headroom fixture did not learn a boost\n";
        return 76;
    }
    setParameterValue (positiveAutoGainHeadroomProcessor, "outputgain", 12.0f);
    positiveAutoGainHeadroomProcessor.resetMeters();
    processTonePeak (positiveAutoGainHeadroomProcessor, 0.8f, 8);
    const auto positiveHeadroomSnapshot = positiveAutoGainHeadroomProcessor.getMeterSnapshot();
    if (positiveHeadroomSnapshot.outputClipped || positiveHeadroomSnapshot.outputPeakDb > 0.05f
        || positiveHeadroomSnapshot.autoGainDb > 2.1f)
    {
        std::cout << "Positive Auto Gain exceeded current block headroom: auto="
                  << positiveHeadroomSnapshot.autoGainDb << " dB outPeak="
                  << positiveHeadroomSnapshot.outputPeakDb << " dB\n";
        return 77;
    }

    FlexCurveAudioProcessor autoEqPeakProcessor;
    autoEqPeakProcessor.prepareToPlay (48000.0, 512);
    if (! autoEqPeakProcessor.addReferenceCurveFile (flatRawFile, FlexCurveLayerType::raw)
        || ! autoEqPeakProcessor.addReferenceCurveFile (raisedTargetFile, FlexCurveLayerType::target))
        return 78;
    const auto peakReferenceLayers = autoEqPeakProcessor.getLayers();
    const auto peakRaw = std::find_if (peakReferenceLayers.begin(), peakReferenceLayers.end(), [] (const auto& layer)
    {
        return layer.type == FlexCurveLayerType::raw;
    });
    const auto peakTarget = std::find_if (peakReferenceLayers.begin(), peakReferenceLayers.end(), [] (const auto& layer)
    {
        return layer.type == FlexCurveLayerType::target;
    });
    if (peakRaw == peakReferenceLayers.end() || peakTarget == peakReferenceLayers.end()
        || ! autoEqPeakProcessor.normalizeReferencesAtFrequency (peakRaw->id, peakTarget->id, 500.0)
        )
        return 79;
    const auto normalizedReferenceLayers = autoEqPeakProcessor.getLayers();
    const auto normalizedRawState = std::find_if (normalizedReferenceLayers.begin(), normalizedReferenceLayers.end(),
                                                   [peakRaw] (const auto& layer)
                                                   {
                                                       return layer.id == peakRaw->id;
                                                   });
    const auto normalizedTargetState = std::find_if (normalizedReferenceLayers.begin(), normalizedReferenceLayers.end(),
                                                      [peakTarget] (const auto& layer)
                                                      {
                                                          return layer.id == peakTarget->id;
                                                      });
    if (normalizedRawState == normalizedReferenceLayers.end()
        || normalizedTargetState == normalizedReferenceLayers.end())
        return 79;
    const auto rawBeforeAutoEq = autoEqPeakProcessor.getLayerCurve (peakRaw->id);
    const auto targetBeforeAutoEq = autoEqPeakProcessor.getLayerCurve (peakTarget->id);
    const auto rawStateBeforeAutoEq = *normalizedRawState;
    const auto targetStateBeforeAutoEq = *normalizedTargetState;
    if (! autoEqPeakProcessor.generateAutoEq (peakRaw->id, peakTarget->id,
                                               FlexCurveAudioProcessor::AutoEqMode::variable))
        return 79;
    const auto peakAutoEqId = autoEqPeakProcessor.getActiveLayerId();
    const auto peakAutoEqCurve = autoEqPeakProcessor.getLayerCurve (peakAutoEqId);
    const auto repositionedRaw = autoEqPeakProcessor.getLayerCurve (peakRaw->id);
    const auto repositionedTarget = autoEqPeakProcessor.getLayerCurve (peakTarget->id);
    const auto peakAutoEqPoint = std::max_element (peakAutoEqCurve.begin(), peakAutoEqCurve.end(),
                                                   [] (const auto& lhs, const auto& rhs)
                                                   {
                                                       return lhs.db < rhs.db;
                                                   });
    double rawMutationError = 0.0;
    for (const auto& point : rawBeforeAutoEq)
        rawMutationError = std::max (rawMutationError,
                                     std::abs (point.db - CurveFIR::interpolateDb (
                                         repositionedRaw, point.frequency)));
    const auto postGenerationLayers = autoEqPeakProcessor.getLayers();
    const auto rawStateAfterAutoEq = std::find_if (postGenerationLayers.begin(), postGenerationLayers.end(),
                                                   [peakRaw] (const auto& layer)
                                                   {
                                                       return layer.id == peakRaw->id;
                                                   });
    const auto targetStateAfterAutoEq = std::find_if (postGenerationLayers.begin(), postGenerationLayers.end(),
                                                      [peakTarget] (const auto& layer)
                                                      {
                                                          return layer.id == peakTarget->id;
                                                      });
    const auto autoEqState = std::find_if (postGenerationLayers.begin(), postGenerationLayers.end(),
                                          [peakAutoEqId] (const auto& layer)
                                          {
                                              return layer.id == peakAutoEqId;
                                          });
    const auto unchangedLayerState = [] (const auto& before, const auto& after)
    {
        if (before.points.size() != after.points.size())
            return false;
        for (size_t i = 0; i < before.points.size(); ++i)
            if (before.points[i].frequency != after.points[i].frequency
                || before.points[i].db != after.points[i].db)
                return false;
        return before.gainDb == after.gainDb
            && before.normalizationOffsetDb == after.normalizationOffsetDb
            && before.smoothSourceCurve == after.smoothSourceCurve
            && before.visible == after.visible
            && before.name == after.name;
    };
    const auto displayRaw = autoEqPeakProcessor.getLayerCurveForDisplay (peakRaw->id);
    const auto displayTarget = autoEqPeakProcessor.getLayerCurveForDisplay (peakTarget->id);
    const auto rawDisplayDelta = CurveFIR::interpolateDb (displayRaw, 500.0)
                               - CurveFIR::interpolateDb (rawBeforeAutoEq, 500.0);
    const auto targetDisplayDelta = CurveFIR::interpolateDb (displayTarget, 500.0)
                                  - CurveFIR::interpolateDb (targetBeforeAutoEq, 500.0);
    const auto residualRms = autoEqPeakProcessor.getAutoEqResidualRmsDb (peakAutoEqId);
    if (peakAutoEqPoint == peakAutoEqCurve.end() || std::abs (peakAutoEqPoint->db) > 0.02
        || rawMutationError > 0.0001
        || rawStateAfterAutoEq == postGenerationLayers.end()
        || targetStateAfterAutoEq == postGenerationLayers.end()
        || autoEqState == postGenerationLayers.end()
        || ! unchangedLayerState (rawStateBeforeAutoEq, *rawStateAfterAutoEq)
        || ! unchangedLayerState (targetStateBeforeAutoEq, *targetStateAfterAutoEq)
        || autoEqState->autoEqReferenceOffsetDb >= -0.1f
        || std::abs (rawDisplayDelta - autoEqState->autoEqReferenceOffsetDb) > 0.02
        || std::abs (targetDisplayDelta - autoEqState->autoEqReferenceOffsetDb) > 0.02
        || std::abs (CurveFIR::interpolateDb (repositionedRaw, 500.0)
                     - CurveFIR::interpolateDb (rawBeforeAutoEq, 500.0)) > 0.0001
        || std::abs (CurveFIR::interpolateDb (repositionedTarget, 500.0)
                     - CurveFIR::interpolateDb (targetBeforeAutoEq, 500.0)) > 0.0001
        || residualRms > 0.30)
    {
        std::cout << "Generated AutoEQ/reference context failed: peak="
                  << (peakAutoEqPoint == peakAutoEqCurve.end() ? 999.0 : peakAutoEqPoint->db)
                  << " storedRawError=" << rawMutationError
                  << " offset=" << (autoEqState == postGenerationLayers.end()
                                         ? 999.0f : autoEqState->autoEqReferenceOffsetDb)
                  << " rawDisplayDelta=" << rawDisplayDelta
                  << " targetDisplayDelta=" << targetDisplayDelta
                  << " residual=" << residualRms << "\n";
        return 80;
    }
    juce::MemoryBlock autoEqReferenceState;
    autoEqPeakProcessor.getStateInformation (autoEqReferenceState);
    FlexCurveAudioProcessor autoEqReferenceRestored;
    autoEqReferenceRestored.prepareToPlay (48000.0, 512);
    autoEqReferenceRestored.setStateInformation (autoEqReferenceState.getData(),
                                                 static_cast<int> (autoEqReferenceState.getSize()));
    const auto restoredAutoEqLayers = autoEqReferenceRestored.getLayers();
    const auto restoredAutoEq = std::find_if (restoredAutoEqLayers.begin(), restoredAutoEqLayers.end(),
                                              [peakAutoEqId] (const auto& layer)
                                              {
                                                  return layer.id == peakAutoEqId;
                                              });
    if (restoredAutoEq == restoredAutoEqLayers.end()
        || std::abs (restoredAutoEq->autoEqReferenceOffsetDb
                     - autoEqState->autoEqReferenceOffsetDb) > 0.0001f
        || ! restoredAutoEq->autoEqReferenceDisplayEnabled)
    {
        std::cout << "AutoEQ reference display offset did not survive preset roundtrip\n";
        return 85;
    }

    FlexCurveAudioProcessor normalizeLayersProcessor;
    normalizeLayersProcessor.prepareToPlay (48000.0, 512);
    if (! normalizeLayersProcessor.addFlatCurve() || ! normalizeLayersProcessor.addFlatCurve())
        return 83;
    const auto normalizeLayerFixtures = normalizeLayersProcessor.getLayers();
    normalizeLayersProcessor.setLayerGain (normalizeLayerFixtures[0].id, 6.0f);
    normalizeLayersProcessor.setLayerGain (normalizeLayerFixtures[1].id, -3.0f);
    normalizeLayersProcessor.normalizeEqLayersToZeroDb();
    for (const auto& layer : normalizeLayersProcessor.getLayers())
    {
        const auto curve = normalizeLayersProcessor.getLayerCurve (layer.id);
        const auto peak = std::max_element (curve.begin(), curve.end(),
                                            [] (const auto& lhs, const auto& rhs)
                                            {
                                                return lhs.db < rhs.db;
                                            });
        if (peak == curve.end() || std::abs (peak->db) > 0.02)
        {
            std::cout << "Normalize Layers to 0 dB did not place every EQ peak at zero\n";
            return 84;
        }
    }

    autoEqPeakProcessor.setCorrectedMeasurementsVisible (false);
    juce::MemoryBlock visibilityState;
    autoEqPeakProcessor.getStateInformation (visibilityState);
    FlexCurveAudioProcessor visibilityRestored;
    visibilityRestored.prepareToPlay (48000.0, 512);
    visibilityRestored.setStateInformation (visibilityState.getData(),
                                            static_cast<int> (visibilityState.getSize()));
    if (visibilityRestored.areCorrectedMeasurementsVisible())
    {
        std::cout << "Tracking visibility did not survive state roundtrip\n";
        return 82;
    }
    autoEqPeakProcessor.renderFir();
    const auto renderedPeakCurve = autoEqPeakProcessor.getFinalCurve();
    const auto renderedPeak = std::max_element (renderedPeakCurve.begin(), renderedPeakCurve.end(),
                                                [] (const auto& lhs, const auto& rhs)
                                                {
                                                    return lhs.db < rhs.db;
                                                });
    if (renderedPeak == renderedPeakCurve.end() || renderedPeak->db > 0.02)
    {
        std::cout << "Rendered AutoEQ FIR curve exceeded 0 dB\n";
        return 81;
    }

    FlexCurveAudioProcessor referenceGainProcessor;
    referenceGainProcessor.prepareToPlay (48000.0, 512);
    if (! referenceGainProcessor.addReferenceCurveFile (gainCurveFile, FlexCurveLayerType::target)
        || ! referenceGainProcessor.addReferenceCurveFile (gainCurveFile, FlexCurveLayerType::raw))
    {
        std::cout << "Reference gain import failed\n";
        return 42;
    }
    for (const auto& layer : referenceGainProcessor.getLayers())
        if (std::abs (layer.gainDb + 6.0f) > 0.001f)
        {
            std::cout << "Target/RAW offset was not mapped to layer gain\n";
            return 43;
        }

    const auto referenceGainLayers = referenceGainProcessor.getLayers();
    const auto targetGainIt = std::find_if (referenceGainLayers.begin(), referenceGainLayers.end(), [] (const auto& layer)
    {
        return layer.type == FlexCurveLayerType::target;
    });
    const auto rawGainIt = std::find_if (referenceGainLayers.begin(), referenceGainLayers.end(), [] (const auto& layer)
    {
        return layer.type == FlexCurveLayerType::raw;
    });
    if (targetGainIt == referenceGainLayers.end() || rawGainIt == referenceGainLayers.end()
        || ! referenceGainProcessor.normalizeReferencesAtFrequency (rawGainIt->id, targetGainIt->id, 500.0)
        || std::abs (CurveFIR::interpolateDb (referenceGainProcessor.getLayerCurve (rawGainIt->id), 500.0)) > 0.01
        || std::abs (CurveFIR::interpolateDb (referenceGainProcessor.getLayerCurve (targetGainIt->id), 500.0)) > 0.01)
    {
        std::cout << "RAW/Target normalization at 500 Hz failed\n";
        return 44;
    }

    referenceGainProcessor.setLayerSourceSmoothing (rawGainIt->id, true);
    const auto smoothedReferenceLayers = referenceGainProcessor.getLayers();
    const auto smoothedRaw = std::find_if (smoothedReferenceLayers.begin(), smoothedReferenceLayers.end(), [rawGainIt] (const auto& layer)
    {
        return layer.id == rawGainIt->id;
    });
    if (smoothedRaw == smoothedReferenceLayers.end()
        || std::abs (CurveFIR::interpolateDb (smoothedRaw->points, 1000.0) - 2.0) > 0.01)
    {
        std::cout << "Source smoothing destructively modified imported points\n";
        return 63;
    }
    juce::MemoryBlock normalizedState;
    referenceGainProcessor.getStateInformation (normalizedState);
    FlexCurveAudioProcessor normalizedRestored;
    normalizedRestored.prepareToPlay (48000.0, 512);
    normalizedRestored.setStateInformation (normalizedState.getData(), static_cast<int> (normalizedState.getSize()));
    const auto normalizedLayers = normalizedRestored.getLayers();
    const auto normalizedRaw = std::find_if (normalizedLayers.begin(), normalizedLayers.end(), [rawGainIt] (const auto& layer)
    {
        return layer.id == rawGainIt->id;
    });
    if (normalizedRaw == normalizedLayers.end() || ! normalizedRaw->smoothSourceCurve
        || std::abs (normalizedRaw->normalizationOffsetDb) < 0.01f)
    {
        std::cout << "Reference normalization/smoothing did not survive state roundtrip\n";
        return 45;
    }

    FlexCurveAudioProcessor meterProcessor;
    meterProcessor.prepareToPlay (48000.0, 512);
    setParameterValue (meterProcessor, "inputgain", 6.0f);
    setParameterValue (meterProcessor, "outputgain", -3.0f);
    setParameterValue (meterProcessor, "autogain", 0.0f);
    juce::AudioBuffer<float> meterBuffer (2, 512);
    meterBuffer.clear();
    for (int channel = 0; channel < meterBuffer.getNumChannels(); ++channel)
        for (int sample = 0; sample < meterBuffer.getNumSamples(); ++sample)
            meterBuffer.setSample (channel, sample, 0.1f);
    juce::MidiBuffer meterMidi;
    meterProcessor.processBlock (meterBuffer, meterMidi);
    const auto meterSnapshot = meterProcessor.getMeterSnapshot();
    if (std::abs (meterSnapshot.inputRmsDb + 14.0f) > 0.4f
        || std::abs (meterSnapshot.outputRmsDb + 17.0f) > 0.5f)
    {
        std::cout << "Input/Output gain staging or RMS metering failed\n";
        return 46;
    }
    meterProcessor.resetMeters();
    meterBuffer.clear();
    for (int channel = 0; channel < meterBuffer.getNumChannels(); ++channel)
        for (int sample = 0; sample < meterBuffer.getNumSamples(); ++sample)
            meterBuffer.setSample (channel, sample, 0.8f);
    meterProcessor.processBlock (meterBuffer, meterMidi);
    const auto clippingSnapshot = meterProcessor.getMeterSnapshot();
    if (! clippingSnapshot.inputClipped || ! clippingSnapshot.outputClipped)
    {
        std::cout << "Meter clipping latch failed\n";
        return 47;
    }

    tempDirectory.deleteRecursively();

    FlexCurveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    std::cout << "prepare ok\n";

    if (! processor.addCurveFile (curveFile))
    {
        std::cout << "Curve load failed\n";
        return 2;
    }
    std::cout << "curve load ok\n";

    const auto layers = processor.getLayers();
    if (layers.empty())
        return 7;
    std::cout << "layer list ok\n";
    const auto layerId = layers.front().id;
    const auto initialDb = CurveFIR::interpolateDb (processor.getFinalCurve(), 1000.0);

    if (! processor.addReferenceCurveFile (curveFile, FlexCurveLayerType::raw)
        || ! processor.addReferenceCurveFile (curveFile, FlexCurveLayerType::target))
    {
        std::cout << "RAW/Target reference import failed\n";
        return 27;
    }
    const auto referenceLayers = processor.getLayers();
    const auto rawIt = std::find_if (referenceLayers.begin(), referenceLayers.end(), [] (const auto& layer)
    {
        return layer.type == FlexCurveLayerType::raw;
    });
    const auto targetIt = std::find_if (referenceLayers.begin(), referenceLayers.end(), [] (const auto& layer)
    {
        return layer.type == FlexCurveLayerType::target;
    });
    if (rawIt == referenceLayers.end() || targetIt == referenceLayers.end()
        || std::abs (CurveFIR::interpolateDb (processor.getFinalCurve(), 1000.0) - initialDb) > 0.01)
    {
        std::cout << "Reference curves affected the audible EQ Average\n";
        return 28;
    }
    if (! processor.generateAutoEq (rawIt->id, targetIt->id, FlexCurveAudioProcessor::AutoEqMode::variable))
    {
        std::cout << "AutoEQ generation failed\n";
        return 29;
    }
    const auto autoEqId = processor.getActiveLayerId();
    const auto autoEqLayers = processor.getLayers();
    const auto autoEqIt = std::find_if (autoEqLayers.begin(), autoEqLayers.end(), [autoEqId] (const auto& layer)
    {
        return layer.id == autoEqId;
    });
    if (autoEqIt == autoEqLayers.end() || autoEqIt->type != FlexCurveLayerType::eq
        || autoEqIt->autoEqMethod != "Variable" || autoEqIt->graphicMode != 0)
    {
        std::cout << "AutoEQ did not create a normal editable EQ layer\n";
        return 30;
    }
    const auto corrected = processor.getCorrectedMeasurementCurve (autoEqId);
    if (corrected.empty() || processor.getAutoEqResidualRmsDb (autoEqId) > 0.25)
    {
        std::cout << "Corrected Measurement did not follow Target\n";
        return 48;
    }
    processor.setLayerGain (autoEqId, 3.0f);
    if (processor.getAutoEqResidualRmsDb (autoEqId) < 2.5)
    {
        std::cout << "Corrected Measurement did not react to EQ gain edits\n";
        return 49;
    }
    processor.setLayerMuted (autoEqId, true);
    const auto mutedCorrected = processor.getCorrectedMeasurementCurve (autoEqId);
    const auto mutedRaw = processor.getLayerCurve (rawIt->id);
    if (std::abs (CurveFIR::interpolateDb (mutedCorrected, 1000.0)
                  - CurveFIR::interpolateDb (mutedRaw, 1000.0)) > 0.05)
    {
        std::cout << "Corrected Measurement did not return to RAW when EQ was muted\n";
        return 50;
    }
    processor.setLayerMuted (autoEqId, false);
    setParameterValue (processor, "bypass", 1.0f);
    const auto bypassedCorrected = processor.getCorrectedMeasurementCurve (autoEqId);
    if (std::abs (CurveFIR::interpolateDb (bypassedCorrected, 1000.0)
                  - CurveFIR::interpolateDb (mutedRaw, 1000.0)) > 0.05)
    {
        std::cout << "Corrected Measurement did not return to RAW under global bypass\n";
        return 86;
    }
    setParameterValue (processor, "bypass", 0.0f);
    processor.setLayerGain (autoEqId, 0.0f);
    processor.setLayerGain (targetIt->id, 1.0f);
    const auto changedSourceLayers = processor.getLayers();
    const auto changedAutoEq = std::find_if (changedSourceLayers.begin(), changedSourceLayers.end(), [autoEqId] (const auto& layer)
    {
        return layer.id == autoEqId;
    });
    if (changedAutoEq == changedSourceLayers.end() || ! changedAutoEq->autoEqSourcesOutdated)
    {
        std::cout << "AutoEQ was not invalidated after a linked Target change\n";
        return 60;
    }
    processor.undo();
    const auto undoneSourceLayers = processor.getLayers();
    const auto undoneAutoEq = std::find_if (undoneSourceLayers.begin(), undoneSourceLayers.end(), [autoEqId] (const auto& layer)
    {
        return layer.id == autoEqId;
    });
    if (undoneAutoEq == undoneSourceLayers.end() || undoneAutoEq->autoEqSourcesOutdated)
    {
        std::cout << "Undo did not restore AutoEQ source validity\n";
        return 61;
    }
    juce::MemoryBlock referenceState;
    processor.getStateInformation (referenceState);
    FlexCurveAudioProcessor referenceRestored;
    referenceRestored.prepareToPlay (48000.0, 512);
    referenceRestored.setStateInformation (referenceState.getData(), static_cast<int> (referenceState.getSize()));
    const auto restoredReferences = referenceRestored.getLayers();
    if (std::count_if (restoredReferences.begin(), restoredReferences.end(), [] (const auto& layer)
        {
            return layer.type == FlexCurveLayerType::raw;
        }) != 1
        || std::count_if (restoredReferences.begin(), restoredReferences.end(), [] (const auto& layer)
        {
            return layer.type == FlexCurveLayerType::target;
        }) != 1)
    {
        std::cout << "RAW/Target layer types did not survive state roundtrip\n";
        return 31;
    }
    processor.removeLayer (autoEqId);
    processor.removeLayer (rawIt->id);
    processor.removeLayer (targetIt->id);
    processor.setActiveLayerId (layerId);

    processor.setGraphicEnabled (true);
    processor.setGraphicMode (15);
    processor.setGraphicGain (0, 5.0f);
    processor.setGraphicMode (31);
    if (std::abs (processor.getGraphicGain (0)) > 0.01f)
    {
        std::cout << "31-band Graphic EQ inherited the 15-band state\n";
        return 22;
    }
    processor.setGraphicGain (0, -3.0f);
    processor.setGraphicMode (0);
    processor.setFreeformPoints ({ { 20.0, 4.0 }, { 20000.0, 4.0 } });
    processor.setGraphicMode (15);
    if (std::abs (processor.getGraphicGain (0) - 5.0f) > 0.01f)
    {
        std::cout << "15-band Graphic EQ did not preserve its independent state\n";
        return 23;
    }
    processor.resetGraphic();
    processor.setGraphicMode (31);
    if (std::abs (processor.getGraphicGain (0) + 3.0f) > 0.01f)
    {
        std::cout << "Resetting 15-band Graphic EQ modified the 31-band state\n";
        return 24;
    }
    processor.resetGraphic();
    processor.setGraphicMode (0);
    if (processor.getFreeformPoints().size() != 2)
    {
        std::cout << "Resetting a fixed Graphic EQ mode modified Variable EQ\n";
        return 87;
    }
    processor.setGraphicMode (15);
    processor.setGraphicGain (0, 2.0f);
    processor.setGraphicMode (31);
    processor.setGraphicGain (0, -2.0f);
    processor.setGraphicMode (0);
    processor.resetGraphic();
    if (! processor.getFreeformPoints().empty())
    {
        std::cout << "Resetting Variable EQ did not clear Variable EQ\n";
        return 88;
    }
    processor.setGraphicMode (15);
    if (std::abs (processor.getGraphicGain (0) - 2.0f) > 0.01f)
    {
        std::cout << "Resetting Variable EQ modified the 15-band state\n";
        return 89;
    }
    processor.resetGraphic();
    processor.setGraphicMode (31);
    if (std::abs (processor.getGraphicGain (0) + 2.0f) > 0.01f)
    {
        std::cout << "Resetting Variable EQ modified the 31-band state\n";
        return 90;
    }
    processor.resetGraphic();

    processor.setGraphicMode (0);
    processor.setGraphicEnabled (true);
    processor.setFreeformPoints ({ { 20.0, 6.0 }, { 20000.0, 6.0 } });
    const auto variableEnabledDb = CurveFIR::interpolateDb (processor.getLayerCurve (layerId), 1000.0);
    if (std::abs ((variableEnabledDb - initialDb) - 6.0) > 0.2)
    {
        std::cout << "Variable EQ did not affect the active curve\n";
        return 11;
    }

    processor.setGraphicEnabled (false);
    const auto variableBypassedDb = CurveFIR::interpolateDb (processor.getLayerCurve (layerId), 1000.0);
    if (std::abs (variableBypassedDb - initialDb) > 0.1)
    {
        std::cout << "Variable EQ bypass leaked into the active curve\n";
        return 12;
    }

    processor.setGraphicEnabled (true);
    processor.setGraphicMode (31);
    const auto switchedModeDb = CurveFIR::interpolateDb (processor.getLayerCurve (layerId), 1000.0);
    if (std::abs (switchedModeDb - initialDb) > 0.1)
    {
        std::cout << "Variable EQ leaked into fixed Graphic EQ mode\n";
        return 13;
    }

    processor.setPreserveVariableShapeAcrossModes (true);
    const auto preservedDb = CurveFIR::interpolateDb (processor.getLayerCurve (layerId), 1000.0);
    if (std::abs ((preservedDb - initialDb) - 6.0) > 0.2)
    {
        std::cout << "Variable preserve mode did not retain the stored shape\n";
        return 14;
    }
    processor.setPreserveVariableShapeAcrossModes (false);
    processor.setGraphicMode (0);
    processor.copyCurrentGraphicEq();
    if (! processor.canPasteCurrentGraphicEq())
    {
        std::cout << "Variable EQ could not paste within its source layer\n";
        return 15;
    }
    if (! processor.addFlatCurve())
        return 16;
    const auto secondLayerId = processor.getLayers().back().id;
    processor.setActiveLayerId (secondLayerId);
    processor.setGraphicMode (0);
    if (! processor.canPasteCurrentGraphicEq())
    {
        std::cout << "Variable EQ could not paste to Variable on another layer\n";
        return 17;
    }
    processor.pasteCurrentGraphicEq();
    if (processor.getFreeformPoints().empty())
    {
        std::cout << "Paste did not copy Variable EQ data\n";
        return 18;
    }
    processor.setGraphicMode (15);
    if (processor.canPasteCurrentGraphicEq())
    {
        std::cout << "Variable EQ was allowed to paste into 15-band mode\n";
        return 91;
    }
    processor.setGraphicMode (31);
    if (processor.canPasteCurrentGraphicEq())
    {
        std::cout << "Variable EQ was allowed to paste into 31-band mode\n";
        return 92;
    }

    processor.setActiveLayerId (layerId);
    processor.setGraphicMode (31);
    processor.setGraphicGain (17, 3.5f);
    processor.copyCurrentGraphicEq();
    processor.setActiveLayerId (secondLayerId);
    processor.setGraphicMode (15);
    if (processor.canPasteCurrentGraphicEq())
    {
        std::cout << "31-band EQ was allowed to paste into 15-band mode\n";
        return 93;
    }
    processor.setGraphicMode (0);
    if (! processor.canPasteCurrentGraphicEq())
    {
        std::cout << "31-band EQ could not paste into Variable mode\n";
        return 94;
    }
    processor.pasteCurrentGraphicEq();
    if (processor.getFreeformPoints().size() != 31)
    {
        std::cout << "31-band to Variable paste did not preserve all bands\n";
        return 95;
    }

    processor.setActiveLayerId (layerId);
    processor.setGraphicMode (15);
    processor.resetGraphic();
    processor.setGraphicGain (8, 5.0f);
    processor.copyCurrentGraphicEq();
    processor.setActiveLayerId (secondLayerId);
    processor.setGraphicMode (31);
    if (! processor.canPasteCurrentGraphicEq())
    {
        std::cout << "15-band EQ could not paste into 31-band mode\n";
        return 96;
    }
    processor.pasteCurrentGraphicEq();
    if (std::abs (processor.getGraphicGain (17) - 5.0f) > 0.01f)
    {
        std::cout << "15-band to 31-band paste did not map the matching 1 kHz band\n";
        return 97;
    }
    processor.setGraphicMode (0);
    processor.pasteCurrentGraphicEq (true);
    const auto invertedVariable = processor.getFreeformPoints();
    if (invertedVariable.size() != 15
        || std::abs (CurveFIR::interpolateDb (invertedVariable, 1000.0) + 5.0) > 0.01)
    {
        std::cout << "Inverted 15-band to Variable paste was incorrect\n";
        return 98;
    }

    processor.copyCurrentParametricEq();
    if (! processor.canPasteCurrentParametricEq())
    {
        std::cout << "Parametric EQ could not paste within the same layer\n";
        return 99;
    }
    processor.pasteCurrentParametricEq();
    processor.removeLayer (secondLayerId);

    processor.setActiveLayerId (layerId);
    processor.setGraphicMode (15);
    processor.setGraphicGain (2, 4.0f);
    processor.setGraphicSmoothing (true);
    if (! processor.cloneLayer (layerId))
    {
        std::cout << "Layer clone failed\n";
        return 25;
    }
    const auto clonedLayerId = processor.getActiveLayerId();
    const auto clonedLayers = processor.getLayers();
    const auto cloned = std::find_if (clonedLayers.begin(), clonedLayers.end(), [clonedLayerId] (const auto& layer)
    {
        return layer.id == clonedLayerId;
    });
    if (cloned == clonedLayers.end() || cloned->colour == layers.front().colour
        || cloned->graphic15Gains[2] != 4.0f || ! cloned->smoothGraphicCurve
        || ! cloned->name.endsWithIgnoreCase ("Copy"))
    {
        std::cout << "Layer clone did not preserve state or assign a new identity\n";
        return 26;
    }
    processor.removeLayer (clonedLayerId);

    processor.setActiveLayerId (layerId);
    processor.setGraphicEnabled (false);
    processor.setLayerGain (layerId, 6.0f);
    const auto gainedDb = CurveFIR::interpolateDb (processor.getFinalCurve(), 1000.0);
    if (gainedDb <= initialDb + 2.5)
    {
        std::cout << "Layer gain did not reach final curve\n";
        return 8;
    }
    processor.undo();
    const auto undoneLayer = processor.getLayerCurve (layerId);
    if (undoneLayer.empty() || std::abs (CurveFIR::interpolateDb (undoneLayer, 1000.0) - initialDb) > 0.2)
    {
        std::cout << "Undo did not restore layer gain\n";
        return 19;
    }

    processor.setLayerGain (layerId, 0.0f);
    const auto localBass = CurveFIR::interpolateDb (processor.getLayerCurve (layerId), 50.0);
    processor.setBlendPerLayerMode (false);
    processor.setRegionSettings (6.0f, 0.0f, 0.0f, 250.0f, 4000.0f);
    const auto globalLayerBass = CurveFIR::interpolateDb (processor.getLayerCurve (layerId), 50.0);
    const auto globalAverageBass = CurveFIR::interpolateDb (processor.getAverageCurve(), 50.0);
    if (std::abs ((globalLayerBass - localBass) - 6.0) > 0.2
        || std::abs (globalAverageBass - globalLayerBass) > 0.1)
    {
        std::cout << "Global blend did not affect layer and Average together\n";
        return 9;
    }
    processor.setRegionSettings (0.0f, 0.0f, 0.0f, 250.0f, 4000.0f);
    processor.setBlendPerLayerMode (true);

    const auto exportFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getChildFile ("flexcurve_average_test.txt");
    if (! processor.exportLayerToFile (-1, true, FlexCurveAudioProcessor::CurveExportFormat::apoParametric, exportFile)
        || ! exportFile.loadFileAsString().containsIgnoreCase ("Filter: ON"))
    {
        std::cout << "Average APO Parametric export failed\n";
        return 20;
    }
    exportFile.deleteFile();

    processor.setActiveLayerId (layerId);
    processor.setGraphicSmoothing (true);
    processor.setPreserveVariableShapeAcrossModes (true);
    if (! processor.isGraphicSmoothingEnabled() || ! processor.isPreserveVariableShapeAcrossModes())
    {
        std::cout << "Smoothing or Variable preserve was not applied before state save\n";
        return 21;
    }
    processor.setEditLocked (true);
    juce::MemoryBlock savedState;
    processor.getStateInformation (savedState);

    FlexCurveAudioProcessor restoredProcessor;
    restoredProcessor.prepareToPlay (48000.0, 512);
    restoredProcessor.setStateInformation (savedState.getData(), static_cast<int> (savedState.getSize()));
    const auto restoredLock = restoredProcessor.isEditLocked();
    const auto restoredSmoothing = restoredProcessor.isGraphicSmoothingEnabled();
    const auto restoredPreserve = restoredProcessor.isPreserveVariableShapeAcrossModes();
    if (! restoredLock || ! restoredSmoothing || ! restoredPreserve)
    {
        std::cout << "Lock, smoothing, or Variable preserve did not survive state roundtrip: "
                  << restoredLock << ", " << restoredSmoothing << ", " << restoredPreserve
                  << " layers=" << restoredProcessor.getLayers().size()
                  << " active=" << restoredProcessor.getActiveLayerId() << "\n";
        return 21;
    }
    processor.setEditLocked (false);
    std::cout << "blend checks ok\n";
    pumpMessageLoop (250);
    std::cout << "message loop ok\n";

    const auto originalBandCount = static_cast<int> (processor.getParamBands().size());
    for (int i = 0; i < 12; ++i)
        processor.addParamBand();
    if (static_cast<int> (processor.getParamBands().size()) != originalBandCount + 12)
    {
        std::cout << "Parametric filter list did not grow dynamically\n";
        return 10;
    }
    std::cout << "parametric growth ok\n";

    for (int i = 0; i < 100; ++i)
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
        if (editor == nullptr)
        {
            std::cout << "Editor creation failed\n";
            return 6;
        }

        editor->setBounds (0, 0, 1280 + (i % 5) * 16, 760 + (i % 3) * 12);
        editor->setVisible (false);
        if ((i % 10) == 0)
            std::cout << "editor cycle " << i << "\n";
    }
    std::cout << "Editor lifecycle test: 100 create/hide/destroy cycles OK\n";

    const auto previewRatio = processNoise (processor, 256);
    std::cout << "Preview RMS ratio: " << previewRatio << "\n";
    if (previewRatio < 0.35 || previewRatio > 2.85)
        return 3;

    std::atomic<bool> editingDone { false };
    std::thread editorThread ([&]
    {
        for (int i = 0; i < 400; ++i)
        {
            const auto gain = 6.0f * std::sin (static_cast<float> (i) * 0.071f);
            processor.setRegionSettings (gain, -0.5f * gain, 0.25f * gain, 250.0f, 4000.0f);
            processor.setGraphicEnabled (true);
            processor.setGraphicGain (i % 31, 0.5f * gain);
        }
        editingDone.store (true);
    });

    const auto stressRatio = processNoise (processor, 1200);
    editorThread.join();
    std::cout << "Concurrent edit RMS ratio: " << stressRatio << "\n";
    if (! editingDone.load() || stressRatio < 0.20 || stressRatio > 4.0)
        return 4;

    processor.renderFir();
    const auto firRatio = processNoise (processor, 256);
    std::cout << "Rendered FIR RMS ratio: " << firRatio << "\n";
    if (firRatio < 0.35 || firRatio > 2.85)
        return 5;

    const auto renderedAt1k = CurveFIR::interpolateDb (processor.getRenderedCurve(), 1000.0);
    setParameterValue (processor, "gain", 12.0f);
    const auto pureRenderedExport = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                        .getChildFile ("flexcurve_global_gain_not_baked.wav");
    if (! processor.exportCurrentFirToFile (pureRenderedExport))
        return 66;
    FlexCurveAudioProcessor pureRenderedImport;
    pureRenderedImport.prepareToPlay (48000.0, 512);
    if (! pureRenderedImport.addCurveFile (pureRenderedExport)
        || pureRenderedImport.getLayers().empty()
        || std::abs (pureRenderedImport.getLayers().front().gainDb) > 0.05f
        || std::abs (CurveFIR::interpolateDb (pureRenderedImport.getLayerCurve (
                         pureRenderedImport.getLayers().front().id), 1000.0) - renderedAt1k) > 1.0)
    {
        std::cout << "Global Gain leaked into rendered FIR export\n";
        return 67;
    }
    pureRenderedExport.deleteFile();
    setParameterValue (processor, "gain", 0.0f);

    processor.setEditLocked (false);
    setParameterValue (processor, "phasemode", 2.0f);
    processor.renderFir();
    if (processor.getActiveLatencySamples() <= 0)
    {
        std::cout << "Rendered Linear phase did not rebuild with non-zero latency\n";
        return 64;
    }
    processor.setEditLocked (false);
    setParameterValue (processor, "phasemode", 0.0f);
    processor.renderFir();
    if (processor.getActiveLatencySamples() != 0)
    {
        std::cout << "Rendered Minimum phase did not return to zero latency\n";
        return 65;
    }

    processor.releaseResources();
    std::cout << "FlexCurve engine test OK\n";
    return 0;
}
