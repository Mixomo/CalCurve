#pragma once

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

struct CurvePoint
{
    double frequency = 0.0;
    double db = 0.0;
};

struct ParsedCurveData
{
    std::vector<CurvePoint> points;
    std::vector<CurvePoint> rightPoints;
    double gainDb = 0.0;
    double rightGainDb = 0.0;
    bool hasExplicitGain = false;
    bool hasExplicitRightGain = false;
    bool hasIndependentRightChannel = false;
};

class CurveFIR
{
public:
    static std::vector<CurvePoint> parseCurveFile (const juce::File& file);
    static std::vector<CurvePoint> parseCurveText (const juce::String& text);
    static ParsedCurveData parseCurveFileWithGain (const juce::File& file);
    static ParsedCurveData parseCurveTextWithGain (const juce::String& text);

    static juce::AudioBuffer<float> createLinearPhaseFIR (const std::vector<CurvePoint>& points,
                                                          double sampleRate,
                                                          int taps = 4096);
    static juce::AudioBuffer<float> createMinimumPhaseFIR (const std::vector<CurvePoint>& points,
                                                           double sampleRate,
                                                           int taps = 4096);
    static juce::AudioBuffer<float> createMixedPhaseFIR (const std::vector<CurvePoint>& points,
                                                         double sampleRate,
                                                         int taps = 4096,
                                                         float minimumPhaseWeight = 0.72f,
                                                         int latencySamples = 512);

    static double interpolateDb (const std::vector<CurvePoint>& points, double frequency);
    static std::vector<CurvePoint> createMagnitudeCurveFromImpulse (const juce::AudioBuffer<float>& impulse,
                                                                    double sampleRate,
                                                                    int points = 220);
    static double calculateKWeightedGainOffset (const std::vector<CurvePoint>& points, double sampleRate);
    static double calculateMagnitudeErrorDb (const std::vector<CurvePoint>& targetPoints,
                                             const juce::AudioBuffer<float>& impulse,
                                             double sampleRate);
};
