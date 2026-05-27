# CalCurve

CalCurve loads local headphone calibration files and applies them as convolution correction. It accepts FIR WAV files directly, or TXT/CSV correction curves which are converted internally to FIR.

![CalCurve](assets/VST_GUI.png)

The tool is intentionally narrow: it does not include a headphone database, headphone simulation, or target selection. It assumes the loaded file is already an exported/calibrated correction curve.

---

# Copy / Install CalCurve VST3

You do not need to build CalCurve just to use the plugin.

For normal installation, download the release asset from GitHub Releases:

```text id="r6x70o"
CalCurve.zip
```

Extract it and copy the included `CalCurve.vst3` bundle.

The ready-to-copy VST3 bundle is included at:

```text id="x54l3i"
CalCurve_VST3/CalCurve.vst3
```

On Windows, copy the full `CalCurve.vst3` bundle to your DAW's VST3 scan folder.

You may need administrator permission depending on the folder you choose. After copying, rescan plugins in your DAW.

---

# FIR and Convolution

An FIR, or finite impulse response, is a filter stored as a short audio-like impulse. When audio passes through that impulse by convolution, the frequency response of the impulse is applied to the signal.

In CalCurve, the loaded calibration is turned into an FIR filter when needed, then applied with convolution. A WAV FIR can be loaded directly. TXT and CSV correction curves are first converted into an internal FIR, then processed the same way.

This is why FIR and convolution are linked here: the FIR is the correction filter, and convolution is the process that applies that filter to the audio.

Compared with a simple frequency/gain correction curve, an FIR has a few practical advantages:

* it turns the whole correction into one continuous filter instead of a set of separate EQ points
* it can represent very detailed correction shapes without needing many individual EQ bands
* it can preserve or intentionally reshape phase behavior depending on the selected phase mode
* it makes TXT/CSV curves and WAV impulse responses share the same convolution engine
* it allows CalCurve to offer Minimum, Natural, and Linear phase versions of the same correction curve

The TXT/CSV curve is still useful as an editable and portable description of the correction. CalCurve uses that curve as the source, then generates the FIR that actually processes the audio.

---

# Supported Formats

* WAV FIR impulse responses
* Frequency/dB TXT files separated by tabs, spaces, commas, or semicolons
* Equalizer APO / Wavelet `GraphicEQ:` text files
* APO CSV correction curves
* Melda FreeForm EQ CSV exports
* Equalizer APO parametric EQ text files

---

# Controls

* **Dry/Wet:** blends the original signal with the corrected signal.
* **Crossfeed:** stereo headphone crossfeed for headphone listening.
* **Gain:** final output trim in dB.
* **Phase Mode:** selects the FIR phase engine.

  * **Minimum:** causal minimum-phase FIR, zero reported latency.
  * **Natural:** mixed-phase FIR with latency scaled from 1024 samples at 44.1 kHz and reduced pre-ringing.
  * **Linear:** symmetric linear-phase FIR with latency scaled from 4096 samples at 44.1 kHz and flat phase.
* **Auto Gain:** hidden internal compensation. When a curve or FIR is loaded, CalCurve estimates the perceived K-weighted loudness change and applies compensation to the corrected wet path. This keeps Dry/Wet and Bypass A/B comparisons closer in perceived level without moving the Gain knob.
* **Limiter:** safety limiter to reduce accidental clipping.
* **Bypass:** latency-compensated bypass path.
* **Load TXT / CSV / FIR:** opens a local calibration file.
* **Export FIR:** writes the active correction as a mono 32-bit WAV FIR using the current host sample rate and selected Phase Mode. Auto Gain is not baked into the exported FIR.
* **Help:** opens the in-plugin reference window.

---

# Presets

The `Presets` menu lets you select, save, and delete user presets from inside the plugin.

* `Select user preset...` is the default empty menu entry.
* Existing presets appear below it.
* `Save current preset...` asks for a preset name and writes a `.calcurvepreset` file.
* `Delete current preset...` deletes the currently loaded custom preset.

On Windows, user presets are stored in the application data folder:

```text
%APPDATA%\Mixomo\CalCurve\UserPresets\
```

CalCurve creates this folder automatically when the presets menu is used.

A preset stores:

* Dry/Wet, Crossfeed, Gain, Phase Mode, Limiter, and Bypass state
* the custom preset name
* the path to the loaded calibration file
* the loaded file label shown in the interface
* an embedded copy of the loaded correction curve as frequency/dB points

When a preset is loaded, CalCurve first tries to reload the original file from the saved path. If the file is still there, it uses the file so the preset follows any deliberate updates you made to that calibration.

If the original file is missing, renamed, or moved, CalCurve falls back to the embedded frequency/dB curve stored inside the preset and rebuilds the FIR from that. This makes presets portable and keeps them useful even when the original calibration file is no longer available.

For WAV FIR files, the preset embeds the magnitude curve extracted from the WAV, not the raw WAV samples. The selected Phase Mode still regenerates the active FIR from that embedded curve.

---

# Graph

* The green line is the loaded calibration curve scaled by Dry/Wet.
* The 0 dB line remains the reference.
* The dB range expands or contracts around 0 dB to fit each loaded curve.
* At 0% Dry/Wet the line becomes flat.
* At 100% Dry/Wet it shows the full correction.
* The GUI displays the current reported latency and measured FIR peak position.

---

# Phase Engines

CalCurve regenerates the active FIR whenever the phase mode changes or when the host sample rate changes.

* TXT and CSV files are parsed into a correction curve, then converted to FIR using the selected phase engine.
* WAV FIR files are converted to a magnitude curve, then regenerated into the selected phase engine so Minimum, Natural, and Linear remain consistent.
* The plugin reports latency changes to the host when the mode or sample rate changes.
* FIR tap counts and latency are scaled from a 44.1 kHz reference to preserve the same time/frequency resolution across host sample rates. This keeps the correction curve stable at 44.1, 48, 88.2, 96, 176.4, 192 kHz, and other rates.
* If you export FIR WAVs for use in another convolver, export one FIR per sample rate you plan to use. Inside CalCurve this is handled automatically at load/rebuild time.

Reference values at 44.1 kHz:

| Mode    | FIR Type      | Reference Taps | Reference Latency |
| ------- | ------------- | -------------: | ----------------: |
| Minimum | Minimum phase |           4096 |         0 samples |
| Natural | Mixed phase   |           4096 |      1024 samples |
| Linear  | Linear phase  |           8192 |      4096 samples |

Some DAWs may compensate plugin delay during playback or monitoring. The plugin GUI shows the active reported latency and FIR peak to make the selected mode visible.

---

## Natural Phase

The term "Natural" in CalCurve is a user-facing description for a mixed-phase FIR reconstruction. It is not intended as a claim of a standardized "natural phase" topology.

Internally, Natural mode uses the same homomorphic / cepstral framework as the minimum-phase reconstruction, but blends the causal and anti-causal cepstral components instead of fully collapsing the response into minimum phase.

In simplified terms:

* Minimum phase pushes the FIR fully toward the causal side.
* Linear phase keeps the FIR symmetric.
* Natural phase sits between those two extremes.

The current implementation uses a fixed minimum-phase weighting of `0.72` and latency scaled from `1024` samples at `44.1 kHz`.

The goal is to reduce the subjective sharpness and pre-ringing associated with strict linear-phase FIRs while preserving more phase symmetry than a fully minimum-phase reconstruction.

Natural phase is therefore better described as a practical mixed-phase FIR reconstruction from the target magnitude, using cepstral weighting, rather than a separate analog filter topology.

---

## Minimum Phase Reconstruction

The minimum-phase FIR generation uses a homomorphic / cepstral reconstruction pipeline:

1. FFT the linear-phase FIR
2. Take the log-magnitude spectrum
3. IFFT into the real cepstrum
4. Fold the cepstrum into the causal region
5. FFT back into the complex log-spectrum
6. Exponentiate into a minimum-phase spectrum
7. IFFT into the final FIR impulse response

The Natural phase mode uses the same framework, but blends the causal and anti-causal cepstral regions instead of fully collapsing them.

---

# APO Parametric EQ Parsing

When importing Equalizer APO parametric EQ text files, CalCurve parses `Preamp` and `Filter` lines and evaluates the magnitude response of each parametric band.

This import stage uses RBJ-style biquad coefficient equations for:

* peaking EQ (`PK`)
* low shelf (`LS`)
* high shelf (`HS`)

Example:

```cpp
const auto a = std::pow (10.0, band.gainDb / 40.0);
const auto alpha = std::sin (w0) / (2.0 * band.q);

b0 = 1.0 + alpha * a;
b1 = -2.0 * std::cos (w0);
b2 = 1.0 - alpha * a;

a0 = 1.0 + alpha / a;
a1 = -2.0 * std::cos (w0);
a2 = 1.0 - alpha / a;
```

These RBJ-style filters are only used during curve import and magnitude evaluation.

The runtime correction engine itself is not a live biquad cascade. After the parametric filters are evaluated, their magnitude response is sampled into a frequency/dB correction curve, and the selected FIR phase engine regenerates the final convolution kernel.

In simplified terms:

```text
APO Parametric EQ
        ↓
RBJ-style magnitude evaluation
        ↓
frequency/dB correction curve
        ↓
Minimum / Natural / Linear FIR generation
        ↓
Convolution engine
```

So although RBJ-style biquad math is involved during import parsing, the final audio processing path remains FIR convolution.

---

# Crossfeed

Crossfeed is intentionally lightweight and separate from the FIR correction engine.

It narrows headphone stereo width slightly by introducing:

* a small delayed opposite-channel feed
* low-pass filtering
* controlled crossfeed gain

The goal is to reduce extreme hard-panned headphone separation without heavily altering the correction curve itself.

Crossfeed is applied after the FIR correction stage.

---

# FIR Regeneration and Sample Rate Scaling

CalCurve regenerates FIR kernels whenever:

* the loaded correction changes
* the phase mode changes
* the host sample rate changes

Instead of reusing a fixed FIR at all sample rates, the FIR tap count and latency are scaled from 44.1 kHz reference values.

This preserves approximately the same time/frequency resolution and correction behavior at:

* 44.1 kHz
* 48 kHz
* 88.2 kHz
* 96 kHz
* 176.4 kHz
* 192 kHz

Without regeneration, the same FIR would behave differently at higher sample rates.

---

# Building

CalCurve is a JUCE CMake project. The required JUCE checkout is expected in:

```text
third_party/JUCE
```

No machine-specific library paths are required. You may still override the JUCE path with `-DJUCE_DIR=/path/to/JUCE` if you are doing local development with a separate checkout.

Requirements:

* CMake 3.22 or newer
* A C++20-capable compiler
* On Windows, Visual Studio 2022 with the MSVC C++ toolchain
* JUCE in `third_party/JUCE`

Example folder layout:

```text
CalCurve/
  CMakeLists.txt
  Source/
  tools/
  third_party/
    JUCE/
```

From the `CalCurve` repository folder, configure the build:

```powershell
cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
```

Build the VST3 plugin and the smoke test executable:

```powershell
cmake --build build --target CalCurve_VST3 CalCurveVST3SmokeTest
```

Expected local build outputs:

```text
build/CalCurve_artefacts/Release/VST3/CalCurve.vst3
build/CalCurveVST3SmokeTest_artefacts/Release/CalCurveVST3SmokeTest.exe
```

---

# Smoke Test

The repository includes `CalCurveVST3SmokeTest`, a small command-line VST3 host used to confirm that the built plugin can be discovered, instantiated, opened, and processed.

Basic plugin load and process test:

```powershell
.\build\CalCurveVST3SmokeTest_artefacts\Release\CalCurveVST3SmokeTest.exe `
  .\build\CalCurve_artefacts\Release\VST3\CalCurve.vst3
```

This checks:

* VST3 discovery
* plugin description parsing
* instance creation
* editor availability
* one empty audio process block

Briefly open the editor:

```powershell
.\build\CalCurveVST3SmokeTest_artefacts\Release\CalCurveVST3SmokeTest.exe `
  .\build\CalCurve_artefacts\Release\VST3\CalCurve.vst3 `
  --editor
```

Round-trip plugin state/preset data:

```powershell
.\build\CalCurveVST3SmokeTest_artefacts\Release\CalCurveVST3SmokeTest.exe `
  .\build\CalCurve_artefacts\Release\VST3\CalCurve.vst3 `
  --preset-roundtrip
```

Curve/FIR phase validation using a generic local TXT or CSV correction curve:

```powershell
.\build\CalCurveVST3SmokeTest_artefacts\Release\CalCurveVST3SmokeTest.exe `
  .\build\CalCurve_artefacts\Release\VST3\CalCurve.vst3 `
  --fir-test "<path-to-your-calibration-curve.txt>"
```

This prints:

* parsed curve point count
* Auto Gain estimate
* generated FIR peak values
* expected and measured peak/latency positions for Minimum, Natural, and Linear phase
* measured convolution output peak position
* average magnitude error against the loaded curve

Inspect a WAV FIR file:

```powershell
.\build\CalCurveVST3SmokeTest_artefacts\Release\CalCurveVST3SmokeTest.exe `
  .\build\CalCurve_artefacts\Release\VST3\CalCurve.vst3 `
  --wav-fir-test "<path-to-your-impulse-response.wav>"
```

This reads a WAV impulse response, extracts its magnitude curve, estimates Auto Gain, and reports basic FIR information.

---

# Credits

* **Development:** Ezequiel Casas (Mixomo)

   https://github.com/Mixomo

* **Example curves:**

   https://github.com/Mixomo/My-Headphones-Calibration-Files

* **More calibration curves:**

   https://autoeq.app/

* **Thanks and credit to Jaakko Pasanen:**

  https://github.com/jaakkopasanen

* **AutoEq:**

   https://github.com/jaakkopasanen/AutoEq

* **squig.link:**

   https://squig.link/

* **MeldaProduction:**

  https://www.meldaproduction.com/

* **Steinberg / VST3 SDK:**

   https://github.com/steinbergmedia/vst3sdk

* **JUCE:**

   https://juce.com/

* **Microsoft MSVC / Visual Studio C++ toolchain:**

  https://microsoft.com/

* **C++ language created by Bjarne Stroustrup**

---

# License

* **CalCurve:** GNU General Public License v3.0 (GPLv3). See `LICENSE`.
* **VST3 SDK components:** MIT License, copyright Steinberg Media Technologies GmbH.
* **JUCE framework:** AGPLv3/commercial licensing.

See `THIRD_PARTY_NOTICES.md` for third-party notices.
