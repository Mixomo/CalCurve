#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "../Source/CurveFIR.h"

#include <iostream>

namespace
{
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
        const auto linear = CurveFIR::createLinearPhaseFIR (points, 48000.0, 8192);
        const auto natural = CurveFIR::createMixedPhaseFIR (points, 48000.0, 4096, 0.72f, 1024);
        const auto minimum = CurveFIR::createMinimumPhaseFIR (points, 48000.0, 4096);
        const auto autoGainDb = juce::jlimit (-18.0, 18.0, CurveFIR::calculateKWeightedGainOffset (points, 48000.0));

        std::cout << "Curve points: " << points.size() << "\n";
        std::cout << "Auto gain dB: " << autoGainDb << "\n";
        std::cout << "Linear peak: " << linear.getMagnitude (0, linear.getNumSamples()) << "\n";
        std::cout << "Linear peak index / expected latency: " << findPeakIndex (linear) << " / 4096\n";
        std::cout << "Linear convolution output peak index: " << measureConvolutionPeakIndex (linear, 48000.0) << "\n";
        std::cout << "Linear error dB: " << CurveFIR::calculateMagnitudeErrorDb (points, linear, 48000.0) << "\n";
        std::cout << "Natural peak: " << natural.getMagnitude (0, natural.getNumSamples()) << "\n";
        std::cout << "Natural peak index / expected latency: " << findPeakIndex (natural) << " / 1024\n";
        std::cout << "Natural convolution output peak index: " << measureConvolutionPeakIndex (natural, 48000.0) << "\n";
        std::cout << "Natural error dB: " << CurveFIR::calculateMagnitudeErrorDb (points, natural, 48000.0) << "\n";
        std::cout << "Minimum peak: " << minimum.getMagnitude (0, minimum.getNumSamples()) << "\n";
        std::cout << "Minimum peak index / expected latency: " << findPeakIndex (minimum) << " / 0\n";
        std::cout << "Minimum convolution output peak index: " << measureConvolutionPeakIndex (minimum, 48000.0) << "\n";
        std::cout << "Minimum error dB: " << CurveFIR::calculateMagnitudeErrorDb (points, minimum, 48000.0) << "\n";

        const auto minResponse = CurveFIR::createMagnitudeCurveFromImpulse (minimum, 48000.0, 220);
        for (auto frequency : { 100.0, 1000.0, 10000.0 })
            std::cout << "Minimum " << frequency << " Hz target/actual dB: "
                      << CurveFIR::interpolateDb (points, frequency) << " / "
                      << CurveFIR::interpolateDb (minResponse, frequency) << "\n";
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
