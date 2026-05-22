#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "../Source/CurveFIR.h"

#include <iostream>

namespace
{
    constexpr double referenceSampleRate = 44100.0;

    int scaleReferenceSamples (int referenceSamples, double sampleRate)
    {
        auto scaled = static_cast<int> (std::round (static_cast<double> (referenceSamples) * sampleRate / referenceSampleRate));
        scaled = juce::jlimit (256, 65536, scaled);

        if ((scaled & 1) != 0)
            ++scaled;

        return juce::jmin (scaled, 65536);
    }

    int findPeakIndex (const juce::AudioBuffer<float>& impulse)
    {
        int peakIndex = 0;
        float peak = -1.0f;

        if (impulse.getNumSamples() <= 0)
            return peakIndex;

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

    int measureConvolutionPeakIndex (const juce::AudioBuffer<float>& impulse, double sampleRate)
    {
        juce::dsp::Convolution convolution;
        auto ir = impulse;
        convolution.loadImpulseResponse (std::move (ir),
                                         sampleRate,
                                         impulse.getNumChannels() > 1 ? juce::dsp::Convolution::Stereo::yes
                                                                      : juce::dsp::Convolution::Stereo::no,
                                         juce::dsp::Convolution::Trim::no,
                                         juce::dsp::Convolution::Normalise::no);

        constexpr int blockSize = 512;
        juce::dsp::ProcessSpec spec { sampleRate, blockSize, 2 };
        convolution.prepare (spec);

        const auto totalSamples = impulse.getNumSamples() + blockSize * 4;
        int globalSample = 0;
        int peakIndex = 0;
        float peak = -1.0f;

        juce::AudioBuffer<float> block (2, blockSize);

        while (globalSample < totalSamples)
        {
            block.clear();

            if (globalSample == 0)
            {
                block.setSample (0, 0, 1.0f);
                block.setSample (1, 0, 1.0f);
            }

            juce::dsp::AudioBlock<float> audioBlock (block);
            juce::dsp::ProcessContextReplacing<float> context (audioBlock);
            convolution.process (context);

            for (int i = 0; i < blockSize; ++i)
            {
                const auto value = std::abs (block.getSample (0, i));
                if (value > peak)
                {
                    peak = value;
                    peakIndex = globalSample + i;
                }
            }

            globalSample += blockSize;
        }

        return peakIndex;
    }
}

class SmokeWindow final : public juce::DocumentWindow
{
public:
    SmokeWindow()
        : DocumentWindow ("CalCurve VST3 Smoke Test",
                          juce::Colours::black,
                          DocumentWindow::closeButton)
    {
    }

    void closeButtonPressed() override
    {
        juce::MessageManager::getInstance()->stopDispatchLoop();
    }
};

int main (int argc, char* argv[])
{
    const juce::ScopedJuceInitialiser_GUI juceInitialiser;

    const auto pluginPath = argc > 1
        ? juce::String (argv[1])
        : juce::String ("J:\\Convolver_VST\\build\\CalCurve_artefacts\\Release\\VST3\\CalCurve.vst3");

    std::cout << "Plugin: " << pluginPath << "\n";

    juce::AudioPluginFormatManager formatManager;
    juce::addDefaultFormatsToManager (formatManager);

    juce::OwnedArray<juce::PluginDescription> descriptions;

    for (auto* format : formatManager.getFormats())
    {
        std::cout << "Format: " << format->getName() << "\n";

        if (format->getName().containsIgnoreCase ("VST3"))
            format->findAllTypesForFile (descriptions, pluginPath);
    }

    std::cout << "Descriptions: " << descriptions.size() << "\n";

    if (descriptions.isEmpty())
        return 2;

    auto& description = *descriptions.getFirst();
    std::cout << "Name: " << description.name << "\n";
    std::cout << "Manufacturer: " << description.manufacturerName << "\n";
    std::cout << "Format: " << description.pluginFormatName << "\n";

    juce::String error;
    auto instance = formatManager.createPluginInstance (description, 48000.0, 512, error);

    if (instance == nullptr)
    {
        std::cout << "Create error: " << error << "\n";
        return 3;
    }

    std::cout << "Instance created\n";
    std::cout << "Has editor: " << (instance->hasEditor() ? "yes" : "no") << "\n";

    const auto openEditor = argc > 2 && juce::String (argv[2]).equalsIgnoreCase ("--editor");
    const auto testFir = argc > 3 && juce::String (argv[2]).equalsIgnoreCase ("--fir-test");
    const auto testWavFir = argc > 3 && juce::String (argv[2]).equalsIgnoreCase ("--wav-fir-test");
    const auto testPreset = argc > 2 && juce::String (argv[2]).equalsIgnoreCase ("--preset-roundtrip");

    if (testFir)
    {
        const juce::File curveFile { juce::String (argv[3]) };
        const auto points = CurveFIR::parseCurveFile (curveFile);
        std::cout << "Curve points: " << points.size() << "\n";

        for (auto testSampleRate : { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 })
        {
            const auto linearTaps = scaleReferenceSamples (8192, testSampleRate);
            const auto naturalTaps = scaleReferenceSamples (4096, testSampleRate);
            const auto minimumTaps = scaleReferenceSamples (4096, testSampleRate);
            const auto naturalLatency = scaleReferenceSamples (1024, testSampleRate);
            const auto linearLatency = linearTaps / 2;
            const auto linear = CurveFIR::createLinearPhaseFIR (points, testSampleRate, linearTaps);
            const auto natural = CurveFIR::createMixedPhaseFIR (points, testSampleRate, naturalTaps, 0.72f, naturalLatency);
            const auto minimum = CurveFIR::createMinimumPhaseFIR (points, testSampleRate, minimumTaps);
            const auto autoGainDb = juce::jlimit (-18.0, 18.0, CurveFIR::calculateKWeightedGainOffset (points, testSampleRate));

            std::cout << "Sample rate: " << testSampleRate << "\n";
            std::cout << "Auto gain dB: " << autoGainDb << "\n";
            std::cout << "Linear peak: " << linear.getMagnitude (0, linear.getNumSamples()) << "\n";
            std::cout << "Linear taps: " << linearTaps << "\n";
            std::cout << "Linear peak index / expected latency: " << findPeakIndex (linear) << " / " << linearLatency << "\n";
            std::cout << "Linear convolution output peak index: " << measureConvolutionPeakIndex (linear, testSampleRate) << "\n";
            std::cout << "Linear error dB: " << CurveFIR::calculateMagnitudeErrorDb (points, linear, testSampleRate) << "\n";
            std::cout << "Natural peak: " << natural.getMagnitude (0, natural.getNumSamples()) << "\n";
            std::cout << "Natural taps: " << naturalTaps << "\n";
            std::cout << "Natural peak index / expected latency: " << findPeakIndex (natural) << " / " << naturalLatency << "\n";
            std::cout << "Natural convolution output peak index: " << measureConvolutionPeakIndex (natural, testSampleRate) << "\n";
            std::cout << "Natural error dB: " << CurveFIR::calculateMagnitudeErrorDb (points, natural, testSampleRate) << "\n";
            std::cout << "Minimum peak: " << minimum.getMagnitude (0, minimum.getNumSamples()) << "\n";
            std::cout << "Minimum taps: " << minimumTaps << "\n";
            std::cout << "Minimum peak index / expected latency: " << findPeakIndex (minimum) << " / 0\n";
            std::cout << "Minimum convolution output peak index: " << measureConvolutionPeakIndex (minimum, testSampleRate) << "\n";
            std::cout << "Minimum error dB: " << CurveFIR::calculateMagnitudeErrorDb (points, minimum, testSampleRate) << "\n";

            const auto minResponse = CurveFIR::createMagnitudeCurveFromImpulse (minimum, testSampleRate, 220);
            for (auto frequency : { 100.0, 1000.0, 10000.0 })
                std::cout << "Minimum " << frequency << " Hz target/actual dB: "
                          << CurveFIR::interpolateDb (points, frequency) << " / "
                          << CurveFIR::interpolateDb (minResponse, frequency) << "\n";
        }
    }

    if (testWavFir)
    {
        juce::AudioFormatManager audioFormatManager;
        audioFormatManager.registerBasicFormats();

        const juce::File wavFile { juce::String (argv[3]) };

        if (auto reader = std::unique_ptr<juce::AudioFormatReader> (audioFormatManager.createReaderFor (wavFile)))
        {
            juce::AudioBuffer<float> impulse (static_cast<int> (reader->numChannels), static_cast<int> (reader->lengthInSamples));
            impulse.clear();
            reader->read (&impulse, 0, impulse.getNumSamples(), 0, true, true);

            const auto points = CurveFIR::createMagnitudeCurveFromImpulse (impulse, reader->sampleRate);
            const auto autoGainDb = juce::jlimit (-18.0, 18.0, CurveFIR::calculateKWeightedGainOffset (points, reader->sampleRate));

            std::cout << "WAV samples: " << impulse.getNumSamples() << "\n";
            std::cout << "WAV channels: " << impulse.getNumChannels() << "\n";
            std::cout << "WAV curve points: " << points.size() << "\n";
            std::cout << "WAV auto gain dB: " << autoGainDb << "\n";
            std::cout << "WAV magnitude error dB: " << CurveFIR::calculateMagnitudeErrorDb (points, impulse, reader->sampleRate) << "\n";
        }
        else
        {
            std::cout << "WAV read failed\n";
            return 5;
        }
    }

    if (testPreset)
    {
        juce::MemoryBlock data;
        instance->getStateInformation (data);

        const auto tempFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("CalCurveSmokeTest.calcurvepreset");

        if (! tempFile.replaceWithData (data.getData(), data.getSize()))
        {
            std::cout << "Preset write failed\n";
            return 6;
        }

        juce::MemoryBlock loaded;

        if (! tempFile.loadFileAsData (loaded))
        {
            std::cout << "Preset read failed\n";
            return 7;
        }

        instance->setStateInformation (loaded.getData(), static_cast<int> (loaded.getSize()));
        tempFile.deleteFile();
        std::cout << "Preset roundtrip OK\n";
    }

    if (openEditor && instance->hasEditor())
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor (instance->createEditorAndMakeActive());

        if (editor == nullptr)
        {
            std::cout << "Editor create failed\n";
            return 4;
        }

        std::cout << "Editor created: " << editor->getWidth() << "x" << editor->getHeight() << "\n";

        SmokeWindow window;
        window.setContentOwned (editor.release(), true);
        window.centreWithSize (window.getWidth(), window.getHeight());
        window.setVisible (true);

        juce::Timer::callAfterDelay (3000, []
        {
            juce::MessageManager::getInstance()->stopDispatchLoop();
        });

        juce::MessageManager::getInstance()->runDispatchLoop();

        window.setVisible (false);
        window.clearContentComponent();
        std::cout << "Editor OK\n";
    }

    instance->setRateAndBufferSizeDetails (48000.0, 512);
    instance->prepareToPlay (48000.0, 512);

    juce::AudioBuffer<float> buffer (2, 512);
    buffer.clear();
    juce::MidiBuffer midi;
    instance->processBlock (buffer, midi);
    instance->releaseResources();

    std::cout << "Process OK\n";
    return 0;
}
