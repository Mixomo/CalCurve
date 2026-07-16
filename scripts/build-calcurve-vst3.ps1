param(
    [string] $BuildDir = "_build_verify",
    [string] $Config = "Release",
    [string] $Generator = "NMake Makefiles",
    [string] $JuceDir = "third_party/JUCE",
    [string] $TestCurve = "",
    [switch] $SkipTests,
    [switch] $NoBundleCopy
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
$BuildPath = Join-Path $Root $BuildDir
$JucePath = if ([System.IO.Path]::IsPathRooted($JuceDir)) { $JuceDir } else { Join-Path $Root $JuceDir }

function Require-Command {
    param([Parameter(Mandatory=$true)][string] $Name)
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "$Name was not found in PATH."
    }
}

function Find-VsDevCmd {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        $path = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find "Common7\Tools\VsDevCmd.bat" | Select-Object -First 1
        if ($path -and (Test-Path -LiteralPath $path)) {
            return $path
        }
    }

    $fallbacks = @(
        "$env:ProgramFiles\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat",
        "$env:ProgramFiles\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat",
        "$env:ProgramFiles\Microsoft Visual Studio\2022\Enterprise\Common7\Tools\VsDevCmd.bat",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
    )

    foreach ($candidate in $fallbacks) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }

    throw "Could not find VsDevCmd.bat. Install Visual Studio 2022 with Desktop development with C++."
}

function Quote-Arg {
    param([string] $Value)
    return '"' + $Value.Replace('"', '\"') + '"'
}

function Invoke-VsCommand {
    param([Parameter(Mandatory=$true)][string] $Command)

    $cmd = "call `"$script:VsDevCmd`" -arch=x64 -host_arch=x64 >nul && $Command"
    cmd.exe /d /s /c $cmd
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code $LASTEXITCODE`: $Command"
    }
}

function Invoke-TestCommand {
    param(
        [Parameter(Mandatory=$true)][string] $Command,
        [Parameter(Mandatory=$true)][string] $FailureMessage
    )

    cmd.exe /d /s /c $Command
    if ($LASTEXITCODE -ne 0) {
        throw "$FailureMessage failed with exit code $LASTEXITCODE"
    }
}

Set-Location $Root

Write-Host "CalCurve root: $Root" -ForegroundColor DarkCyan
Write-Host "Build dir:     $BuildPath" -ForegroundColor DarkCyan
Write-Host "JUCE dir:      $JucePath" -ForegroundColor DarkCyan

Require-Command cmake
$script:VsDevCmd = Find-VsDevCmd

if (-not (Test-Path -LiteralPath (Join-Path $JucePath "CMakeLists.txt"))) {
    throw "JUCE was not found at $JucePath. This repo expects JUCE under third_party\JUCE, or pass -JuceDir <path>."
}

Write-Host "Configuring CalCurve..." -ForegroundColor Cyan
Invoke-VsCommand ("cmake -S " + (Quote-Arg $Root) +
    " -B " + (Quote-Arg $BuildPath) +
    " -G " + (Quote-Arg $Generator) +
    " -DCMAKE_BUILD_TYPE=" + (Quote-Arg $Config) +
    " -DJUCE_DIR=" + (Quote-Arg $JucePath))

Write-Host "Building CalCurve VST3 and smoke test..." -ForegroundColor Cyan
Invoke-VsCommand ("cmake --build " + (Quote-Arg $BuildPath) +
    " --target CalCurve_VST3 CalCurveVST3SmokeTest --config " + (Quote-Arg $Config))

$Vst3Bundle = Join-Path $BuildPath "CalCurve_artefacts\$Config\VST3\CalCurve.vst3"
$SmokeTest = Join-Path $BuildPath "CalCurveVST3SmokeTest_artefacts\$Config\CalCurveVST3SmokeTest.exe"

if (-not (Test-Path -LiteralPath $Vst3Bundle)) {
    throw "Built VST3 bundle not found: $Vst3Bundle"
}

if (-not $SkipTests) {
    Write-Host "Running VST3 process smoke test..." -ForegroundColor Cyan
    Invoke-TestCommand ((Quote-Arg $SmokeTest) + " " + (Quote-Arg $Vst3Bundle)) "CalCurveVST3SmokeTest process"

    Write-Host "Running VST3 editor smoke test..." -ForegroundColor Cyan
    Invoke-TestCommand ((Quote-Arg $SmokeTest) + " " + (Quote-Arg $Vst3Bundle) + " --editor") "CalCurveVST3SmokeTest --editor"

    Write-Host "Running VST3 preset roundtrip smoke test..." -ForegroundColor Cyan
    Invoke-TestCommand ((Quote-Arg $SmokeTest) + " " + (Quote-Arg $Vst3Bundle) + " --preset-roundtrip") "CalCurveVST3SmokeTest --preset-roundtrip"

    if ($TestCurve.Trim().Length -gt 0) {
        if (-not (Test-Path -LiteralPath $TestCurve)) {
            throw "Test curve not found: $TestCurve"
        }

        Write-Host "Running FIR phase validation..." -ForegroundColor Cyan
        Invoke-TestCommand ((Quote-Arg $SmokeTest) + " " + (Quote-Arg $Vst3Bundle) + " --fir-test " + (Quote-Arg $TestCurve)) "CalCurveVST3SmokeTest --fir-test"
    } else {
        Write-Host "Skipping FIR phase validation: pass -TestCurve <file> to run it." -ForegroundColor Yellow
    }
}

if (-not $NoBundleCopy) {
    $ReadyDir = Join-Path $Root "CalCurve_VST3"
    $ReadyBundle = Join-Path $ReadyDir "CalCurve.vst3"
    New-Item -ItemType Directory -Path $ReadyDir -Force | Out-Null
    if (Test-Path -LiteralPath $ReadyBundle) {
        Remove-Item -LiteralPath $ReadyBundle -Recurse -Force
    }
    Copy-Item -LiteralPath $Vst3Bundle -Destination $ReadyDir -Recurse -Force
    Write-Host "Updated ready-to-copy bundle: $ReadyBundle" -ForegroundColor Green
}

Write-Host "CalCurve build completed successfully." -ForegroundColor Green
