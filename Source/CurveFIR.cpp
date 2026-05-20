#include "CurveFIR.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace
{
    struct ParametricBand
    {
        double frequency = 1000.0;
        double gainDb = 0.0;
        double q = 1.0;
        juce::String type = "PK";
    };

    bool parsePair (const juce::String& text, CurvePoint& point)
    {
        auto cleaned = text.trim()
                           .replaceCharacter (',', ' ')
                           .replaceCharacter (';', ' ')
                           .replaceCharacter ('\t', ' ');

        juce::StringArray tokens;
        tokens.addTokens (cleaned, " ", "");
        tokens.removeEmptyStrings();

        if (tokens.size() < 2)
            return false;

        const auto frequency = tokens[0].getDoubleValue();
        const auto db = tokens[1].getDoubleValue();

        if (frequency <= 0.0 || ! std::isfinite (frequency) || ! std::isfinite (db))
            return false;

        point = { frequency, db };
        return true;
    }

    bool parseParametricLine (const juce::String& line, ParametricBand& band)
    {
        if (! line.containsIgnoreCase ("Filter") || ! line.containsIgnoreCase ("Fc"))
            return false;

        juce::StringArray tokens;
        tokens.addTokens (line.replaceCharacter (':', ' '), " \t", "");
        tokens.removeEmptyStrings();

        for (int i = 0; i < tokens.size(); ++i)
        {
            const auto token = tokens[i];

            if (token.equalsIgnoreCase ("PK") || token.equalsIgnoreCase ("LS") || token.equalsIgnoreCase ("HS")
                || token.equalsIgnoreCase ("LP") || token.equalsIgnoreCase ("HP"))
                band.type = token.toUpperCase();

            if (token.equalsIgnoreCase ("Fc") && i + 1 < tokens.size())
                band.frequency = tokens[i + 1].getDoubleValue();

            if (token.equalsIgnoreCase ("Gain") && i + 1 < tokens.size())
                band.gainDb = tokens[i + 1].getDoubleValue();

            if (token.equalsIgnoreCase ("Q") && i + 1 < tokens.size())
                band.q = tokens[i + 1].getDoubleValue();
        }

        return band.frequency > 0.0 && band.q > 0.0 && std::isfinite (band.gainDb);
    }

    double parsePreamp (const juce::String& line)
    {
        if (! line.containsIgnoreCase ("Preamp"))
            return 0.0;

        juce::StringArray tokens;
        tokens.addTokens (line.replaceCharacter (':', ' '), " \t", "");
        tokens.removeEmptyStrings();

        for (const auto& token : tokens)
        {
            const auto value = token.getDoubleValue();
            if (std::abs (value) > 0.0001)
                return value;
        }

        return 0.0;
    }

    double biquadMagnitudeDb (const ParametricBand& band, double frequency, double sampleRate)
    {
        const auto a = std::pow (10.0, band.gainDb / 40.0);
        const auto w0 = juce::MathConstants<double>::twoPi * juce::jlimit (1.0, sampleRate * 0.49, band.frequency) / sampleRate;
        const auto w = juce::MathConstants<double>::twoPi * juce::jlimit (1.0, sampleRate * 0.49, frequency) / sampleRate;
        const auto alpha = std::sin (w0) / (2.0 * band.q);

        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;

        if (band.type == "LS")
        {
            const auto sqrtA = std::sqrt (a);
            const auto twoSqrtAAlpha = 2.0 * sqrtA * alpha;
            b0 = a * ((a + 1.0) - (a - 1.0) * std::cos (w0) + twoSqrtAAlpha);
            b1 = 2.0 * a * ((a - 1.0) - (a + 1.0) * std::cos (w0));
            b2 = a * ((a + 1.0) - (a - 1.0) * std::cos (w0) - twoSqrtAAlpha);
            a0 = (a + 1.0) + (a - 1.0) * std::cos (w0) + twoSqrtAAlpha;
            a1 = -2.0 * ((a - 1.0) + (a + 1.0) * std::cos (w0));
            a2 = (a + 1.0) + (a - 1.0) * std::cos (w0) - twoSqrtAAlpha;
        }
        else if (band.type == "HS")
        {
            const auto sqrtA = std::sqrt (a);
            const auto twoSqrtAAlpha = 2.0 * sqrtA * alpha;
            b0 = a * ((a + 1.0) + (a - 1.0) * std::cos (w0) + twoSqrtAAlpha);
            b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * std::cos (w0));
            b2 = a * ((a + 1.0) + (a - 1.0) * std::cos (w0) - twoSqrtAAlpha);
            a0 = (a + 1.0) - (a - 1.0) * std::cos (w0) + twoSqrtAAlpha;
            a1 = 2.0 * ((a - 1.0) - (a + 1.0) * std::cos (w0));
            a2 = (a + 1.0) - (a - 1.0) * std::cos (w0) - twoSqrtAAlpha;
        }
        else
        {
            b0 = 1.0 + alpha * a;
            b1 = -2.0 * std::cos (w0);
            b2 = 1.0 - alpha * a;
            a0 = 1.0 + alpha / a;
            a1 = -2.0 * std::cos (w0);
            a2 = 1.0 - alpha / a;
        }

        const std::complex<double> z1 = std::exp (std::complex<double> (0.0, -w));
        const auto z2 = z1 * z1;
        const auto numerator = b0 + b1 * z1 + b2 * z2;
        const auto denominator = a0 + a1 * z1 + a2 * z2;
        return juce::Decibels::gainToDecibels (std::abs (numerator / denominator), -96.0);
    }

    float hann (int n, int size)
    {
        if (size <= 1)
            return 1.0f;

        return static_cast<float> (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * n / (size - 1)));
    }

    int fftOrderForSize (int size)
    {
        int order = 0;
        int value = 1;
        while (value < size)
        {
            value <<= 1;
            ++order;
        }
        return order;
    }
}

std::vector<CurvePoint> CurveFIR::parseCurveFile (const juce::File& file)
{
    return parseCurveText (file.loadFileAsString());
}

std::vector<CurvePoint> CurveFIR::parseCurveText (const juce::String& sourceText)
{
    std::vector<CurvePoint> points;
    auto text = sourceText;
    std::vector<ParametricBand> bands;
    double preamp = 0.0;

    if (text.containsIgnoreCase ("GraphicEQ:"))
    {
        text = text.fromFirstOccurrenceOf ("GraphicEQ:", false, true);
        juce::StringArray chunks;
        chunks.addTokens (text, ";", "");

        for (const auto& chunk : chunks)
        {
            CurvePoint point;
            if (parsePair (chunk, point))
                points.push_back (point);
        }
    }
    else
    {
        juce::StringArray lines;
        lines.addLines (text);

        for (const auto& line : lines)
        {
            CurvePoint point;
            if (parsePair (line, point))
                points.push_back (point);

            ParametricBand band;
            if (parseParametricLine (line, band))
                bands.push_back (band);

            preamp += parsePreamp (line);
        }
    }

    if (points.empty() && ! bands.empty())
    {
        points.reserve (240);
        for (int i = 0; i < 240; ++i)
        {
            const auto alpha = static_cast<double> (i) / 239.0;
            const auto frequency = 20.0 * std::pow (1000.0, alpha);
            double db = preamp;

            for (const auto& band : bands)
                db += biquadMagnitudeDb (band, frequency, 48000.0);

            points.push_back ({ frequency, db });
        }
    }

    std::sort (points.begin(), points.end(), [] (const auto& a, const auto& b)
    {
        return a.frequency < b.frequency;
    });

    points.erase (std::unique (points.begin(), points.end(), [] (const auto& a, const auto& b)
    {
        return juce::approximatelyEqual (a.frequency, b.frequency);
    }), points.end());

    if (points.size() > 2 && points.back().frequency <= 5.0)
    {
        for (auto& point : points)
        {
            point.frequency = std::pow (10.0, point.frequency);

            if (std::abs (point.db) <= 1.5)
                point.db *= 20.0;
        }

        std::sort (points.begin(), points.end(), [] (const auto& a, const auto& b)
        {
            return a.frequency < b.frequency;
        });
    }

    return points;
}

juce::AudioBuffer<float> CurveFIR::createLinearPhaseFIR (const std::vector<CurvePoint>& points,
                                                         double sampleRate,
                                                         int taps)
{
    taps = juce::jlimit (256, 16384, taps);
    const auto fftSize = juce::nextPowerOfTwo (taps * 2);
    const auto halfBins = fftSize / 2;

    juce::AudioBuffer<float> impulse (1, taps);
    impulse.clear();

    auto* out = impulse.getWritePointer (0);
    const auto centre = taps / 2;

    for (int n = 0; n < taps; ++n)
    {
        const auto m = n - centre;
        double value = 0.0;

        for (int k = 0; k <= halfBins; ++k)
        {
            const auto frequency = (static_cast<double> (k) * sampleRate) / fftSize;
            const auto gain = juce::Decibels::decibelsToGain (interpolateDb (points, frequency));
            const auto weight = (k == 0 || k == halfBins) ? 1.0 : 2.0;
            value += weight * gain * std::cos (juce::MathConstants<double>::twoPi * k * m / fftSize);
        }

        out[n] = static_cast<float> (value / fftSize) * hann (n, taps);
    }

    return impulse;
}

juce::AudioBuffer<float> CurveFIR::createMinimumPhaseFIR (const std::vector<CurvePoint>& points,
                                                          double sampleRate,
                                                          int taps)
{
    taps = juce::jlimit (256, 16384, taps);

    // The minimum-phase FIR is derived from the linear-phase FIR via the
    // homomorphic (cepstral) method:
    //   1. FFT the zero-padded linear FIR -> complex spectrum L[k]
    //   2. Take log |L[k]| (real-only log-magnitude spectrum)
    //   3. IFFT -> real cepstrum c[n]
    //   4. Fold: keep DC & Nyquist, double causal half, zero anticausal half
    //   5. FFT the folded cepstrum -> complex log-spectrum H[k]
    //   6. Exponentiate -> minimum-phase spectrum M[k]
    //   7. IFFT -> minimum-phase impulse h[n]
    //
    // JUCE FFT convention: forward does NOT divide by N; inverse DOES divide by N.
    // Round-trip accounting:
    //   step 3: IFFT divides by N  -> cepstrum is scaled by 1/N relative to logMag
    //   step 5: FFT does NOT divide -> complexLogSpec is N * (1/N) = 1x correct
    //   No extra /N needed after step 5.
    //   step 7: IFFT divides by N  -> timeDomain is (1/N) * M[k]
    //   M[k] lives at the same absolute scale as L[k] (from step 1, not divided by N)
    //   So timeDomain must be multiplied by N to get back to the right amplitude.

    const auto fftSize = juce::nextPowerOfTwo (taps * 4);
    const auto order   = fftOrderForSize (fftSize);
    juce::dsp::FFT fft (order);

    std::vector<juce::dsp::Complex<float>> logMag        (static_cast<size_t> (fftSize), { 0.0f, 0.0f });
    std::vector<juce::dsp::Complex<float>> cepstrum      (static_cast<size_t> (fftSize));
    std::vector<juce::dsp::Complex<float>> minCepstrum   (static_cast<size_t> (fftSize), { 0.0f, 0.0f });
    std::vector<juce::dsp::Complex<float>> complexLogSpec(static_cast<size_t> (fftSize));
    std::vector<juce::dsp::Complex<float>> minSpectrum   (static_cast<size_t> (fftSize));
    std::vector<juce::dsp::Complex<float>> minTimeDomain (static_cast<size_t> (fftSize));
    std::vector<juce::dsp::Complex<float>> linTime       (static_cast<size_t> (fftSize), { 0.0f, 0.0f });
    std::vector<juce::dsp::Complex<float>> linSpectrum   (static_cast<size_t> (fftSize));

    // Step 1: linear FIR -> FFT
    const auto linear = createLinearPhaseFIR (points, sampleRate, taps);
    const auto* linearSamples = linear.getReadPointer (0);
    for (int n = 0; n < taps; ++n)
        linTime[static_cast<size_t> (n)] = { linearSamples[n], 0.0f };
    fft.perform (linTime.data(), linSpectrum.data(), false);   // not normalised

    // Step 2: log-magnitude spectrum (real only, imag = 0)
    for (int k = 0; k < fftSize; ++k)
        logMag[static_cast<size_t> (k)] = { std::log (std::max (std::abs (linSpectrum[static_cast<size_t> (k)]), 1.0e-7f)), 0.0f };

    // Step 3: IFFT -> real cepstrum  (JUCE divides by N)
    fft.perform (logMag.data(), cepstrum.data(), true);

    // Step 4: fold cepstrum
    minCepstrum[0] = cepstrum[0];
    minCepstrum[static_cast<size_t> (fftSize / 2)] = cepstrum[static_cast<size_t> (fftSize / 2)];
    for (int i = 1; i < fftSize / 2; ++i)
        minCepstrum[static_cast<size_t> (i)] = cepstrum[static_cast<size_t> (i)] * 2.0f;
    // indices fftSize/2+1 .. fftSize-1 remain zero (anticausal part)

    // Step 5: FFT folded cepstrum -> complex log-spectrum  (JUCE does NOT divide by N)
    // Round-trip IFFT*FFT leaves scale correct (1/N * N = 1): no extra division needed.
    fft.perform (minCepstrum.data(), complexLogSpec.data(), false);

    // Step 6: exponentiate -> minimum-phase spectrum
    for (int k = 0; k < fftSize; ++k)
    {
        const auto v   = complexLogSpec[static_cast<size_t> (k)];
        const auto mag = std::exp (static_cast<double> (v.real()));
        minSpectrum[static_cast<size_t> (k)] = { static_cast<float> (mag * std::cos (v.imag())),
                                                  static_cast<float> (mag * std::sin (v.imag())) };
    }

    // Step 7: IFFT -> time domain  (JUCE divides by N)
    // Compensate by multiplying output by N so amplitude matches the linear FIR.
    fft.perform (minSpectrum.data(), minTimeDomain.data(), true);

    juce::AudioBuffer<float> impulse (1, taps);
    auto* out = impulse.getWritePointer (0);

    for (int n = 0; n < taps; ++n)
    {
        float w = 1.0f;
        const auto fadeStart = static_cast<int> (taps * 0.82);
        if (n >= fadeStart)
        {
            const auto fadeLen = juce::jmax (1, taps - fadeStart);
            const auto x = static_cast<double> (n - fadeStart) / static_cast<double> (fadeLen);
            w = static_cast<float> (0.5 + 0.5 * std::cos (juce::MathConstants<double>::pi * x));
        }
        out[n] = minTimeDomain[static_cast<size_t> (n)].real() * w;
    }

    return impulse;
}

juce::AudioBuffer<float> CurveFIR::createMixedPhaseFIR (const std::vector<CurvePoint>& points,
                                                        double sampleRate,
                                                        int taps,
                                                        float minimumPhaseWeight,
                                                        int latencySamples)
{
    taps               = juce::jlimit (256, 16384, taps);
    minimumPhaseWeight = juce::jlimit (0.0f, 1.0f, minimumPhaseWeight);
    latencySamples     = juce::jlimit (0, taps - 1, latencySamples);

    // Same cepstral method as createMinimumPhaseFIR, but the cepstrum is
    // blended between linear (w=0) and minimum-phase (w=1):
    //   causal quefrencies [1..N/2-1]       scaled by (1 + w)   [1..2]
    //   anticausal quefrencies [N/2+1..N-1] scaled by (1 - w)   [1..0]
    // Same JUCE FFT scaling accounting as the minimum-phase case applies.

    const auto fftSize = juce::nextPowerOfTwo (taps * 4);
    const auto order   = fftOrderForSize (fftSize);
    juce::dsp::FFT fft (order);

    std::vector<juce::dsp::Complex<float>> logMag          (static_cast<size_t> (fftSize), { 0.0f, 0.0f });
    std::vector<juce::dsp::Complex<float>> cepstrum        (static_cast<size_t> (fftSize));
    std::vector<juce::dsp::Complex<float>> mixedCepstrum   (static_cast<size_t> (fftSize), { 0.0f, 0.0f });
    std::vector<juce::dsp::Complex<float>> complexLogSpec  (static_cast<size_t> (fftSize));
    std::vector<juce::dsp::Complex<float>> mixedSpectrum   (static_cast<size_t> (fftSize));
    std::vector<juce::dsp::Complex<float>> mixedTimeDomain (static_cast<size_t> (fftSize));
    std::vector<juce::dsp::Complex<float>> linTime         (static_cast<size_t> (fftSize), { 0.0f, 0.0f });
    std::vector<juce::dsp::Complex<float>> linSpectrum     (static_cast<size_t> (fftSize));

    const auto linear = createLinearPhaseFIR (points, sampleRate, taps);
    const auto* linearSamples = linear.getReadPointer (0);
    for (int n = 0; n < taps; ++n)
        linTime[static_cast<size_t> (n)] = { linearSamples[n], 0.0f };
    fft.perform (linTime.data(), linSpectrum.data(), false);

    for (int k = 0; k < fftSize; ++k)
        logMag[static_cast<size_t> (k)] = { std::log (std::max (std::abs (linSpectrum[static_cast<size_t> (k)]), 1.0e-7f)), 0.0f };

    fft.perform (logMag.data(), cepstrum.data(), true);   // JUCE divides by N

    const auto causalScale     = 1.0f + minimumPhaseWeight;
    const auto anticausalScale = 1.0f - minimumPhaseWeight;
    mixedCepstrum[0] = cepstrum[0];
    mixedCepstrum[static_cast<size_t> (fftSize / 2)] = cepstrum[static_cast<size_t> (fftSize / 2)];
    for (int i = 1; i < fftSize / 2; ++i)
    {
        mixedCepstrum[static_cast<size_t> (i)]           = cepstrum[static_cast<size_t> (i)]           * causalScale;
        mixedCepstrum[static_cast<size_t> (fftSize - i)] = cepstrum[static_cast<size_t> (fftSize - i)] * anticausalScale;
    }

    fft.perform (mixedCepstrum.data(), complexLogSpec.data(), false);  // JUCE does NOT divide by N

    for (int k = 0; k < fftSize; ++k)
    {
        const auto v   = complexLogSpec[static_cast<size_t> (k)];
        const auto mag = std::exp (static_cast<double> (v.real()));
        mixedSpectrum[static_cast<size_t> (k)] = { static_cast<float> (mag * std::cos (v.imag())),
                                                    static_cast<float> (mag * std::sin (v.imag())) };
    }

    fft.perform (mixedSpectrum.data(), mixedTimeDomain.data(), true);  // JUCE divides by N

    juce::AudioBuffer<float> impulse (1, taps);
    auto* out = impulse.getWritePointer (0);

    for (int n = 0; n < taps; ++n)
    {
        const auto sourceIndex = (n - latencySamples + fftSize) % fftSize;
        float w = 1.0f;
        const auto fadeStart = juce::jmax (latencySamples + 1, static_cast<int> (taps * 0.82));
        if (n >= fadeStart)
        {
            const auto fadeLen = juce::jmax (1, taps - fadeStart);
            const auto x = static_cast<double> (n - fadeStart) / static_cast<double> (fadeLen);
            w = static_cast<float> (0.5 + 0.5 * std::cos (juce::MathConstants<double>::pi * x));
        }
        out[n] = mixedTimeDomain[static_cast<size_t> (sourceIndex)].real() * w;
    }

    return impulse;
}

double CurveFIR::interpolateDb (const std::vector<CurvePoint>& points, double frequency)
{
    if (points.empty())
        return 0.0;

    if (frequency <= points.front().frequency)
        return points.front().db;

    if (frequency >= points.back().frequency)
        return points.back().db;

    auto upper = std::lower_bound (points.begin(), points.end(), frequency, [] (const auto& point, double value)
    {
        return point.frequency < value;
    });

    auto lower = std::prev (upper);
    const auto x0 = std::log2 (lower->frequency);
    const auto x1 = std::log2 (upper->frequency);
    const auto x = std::log2 (frequency);
    const auto alpha = (x - x0) / (x1 - x0);

    return lower->db + alpha * (upper->db - lower->db);
}

std::vector<CurvePoint> CurveFIR::createMagnitudeCurveFromImpulse (const juce::AudioBuffer<float>& impulse,
                                                                   double sampleRate,
                                                                   int numPoints)
{
    std::vector<CurvePoint> points;

    if (impulse.getNumSamples() == 0 || sampleRate <= 0.0)
        return points;

    numPoints = juce::jlimit (32, 512, numPoints);
    const auto* samples = impulse.getReadPointer (0);
    const auto sampleCount = impulse.getNumSamples();
    const auto minFrequency = 20.0;
    const auto maxFrequency = juce::jmin (20000.0, sampleRate * 0.475);

    points.reserve (static_cast<size_t> (numPoints));

    for (int i = 0; i < numPoints; ++i)
    {
        const auto alpha = static_cast<double> (i) / (numPoints - 1);
        const auto frequency = minFrequency * std::pow (maxFrequency / minFrequency, alpha);
        const auto phaseStep = -juce::MathConstants<double>::twoPi * frequency / sampleRate;
        double real = 0.0;
        double imag = 0.0;

        for (int n = 0; n < sampleCount; ++n)
        {
            const auto phase = phaseStep * n;
            real += samples[n] * std::cos (phase);
            imag += samples[n] * std::sin (phase);
        }

        const auto magnitude = std::sqrt (real * real + imag * imag);
        points.push_back ({ frequency, juce::Decibels::gainToDecibels (magnitude, -96.0) });
    }

    return points;
}

double CurveFIR::calculateKWeightedGainOffset (const std::vector<CurvePoint>& points, double sampleRate)
{
    if (points.empty())
        return 0.0;

    double weightedPowerSum = 0.0;
    double weightSum = 0.0;

    // High-shelf: f = 1500 Hz, Q = 1.0, gain = 4.0 dB (1.58489319)
    auto hs = juce::dsp::IIR::Coefficients<double>::makeHighShelf (sampleRate, 1500.0, 1.0, 1.58489319);
    // High-pass: f = 38.0 Hz, Q = 0.5
    auto hp = juce::dsp::IIR::Coefficients<double>::makeHighPass (sampleRate, 38.0, 0.5);

    for (size_t i = 0; i < points.size(); ++i)
    {
        const auto& pt = points[i];
        double f = pt.frequency;
        double db = pt.db;

        // Calculate K-weighting magnitude
        double kMag = 1.0;
        if (hs != nullptr)
            kMag *= hs->getMagnitudeForFrequency (f, sampleRate);
        if (hp != nullptr)
            kMag *= hp->getMagnitudeForFrequency (f, sampleRate);

        double kPower = kMag * kMag;
        double filterPower = std::pow (10.0, db / 10.0); // power = gain^2 = 10^(db/10)

        // Integrate over logarithmic frequency spacing
        // Since points are logarithmic, we weight by frequency width delta
        double width = f;
        if (i > 0)
            width = f - points[i - 1].frequency;
        else if (points.size() > 1)
            width = points[1].frequency - f;

        if (width > 0.0)
        {
            weightedPowerSum += filterPower * kPower * width;
            weightSum += kPower * width;
        }
    }

    if (weightSum <= 0.0)
        return 0.0;

    double meanPower = weightedPowerSum / weightSum;
    double perceivedGainDb = 10.0 * std::log10 (std::max (meanPower, 1e-12));

    return -perceivedGainDb;
}

double CurveFIR::calculateMagnitudeErrorDb (const std::vector<CurvePoint>& targetPoints,
                                            const juce::AudioBuffer<float>& impulse,
                                            double sampleRate)
{
    if (targetPoints.empty() || impulse.getNumSamples() == 0 || sampleRate <= 0.0)
        return 0.0;

    const auto response = createMagnitudeCurveFromImpulse (impulse, sampleRate, 220);

    if (response.empty())
        return 0.0;

    double errorSum = 0.0;
    int count = 0;

    for (const auto& point : response)
    {
        if (point.frequency < 20.0 || point.frequency > juce::jmin (20000.0, sampleRate * 0.475))
            continue;

        const auto targetDb = interpolateDb (targetPoints, point.frequency);
        errorSum += std::abs (point.db - targetDb);
        ++count;
    }

    return count > 0 ? errorSum / static_cast<double> (count) : 0.0;
}
