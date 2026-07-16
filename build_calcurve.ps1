$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Script = Join-Path $Root "scripts\build-calcurve-vst3.ps1"
& $Script @args
exit $LASTEXITCODE
