# Encode the HDR chart and require the stop/color gate to pass.
# JPEG bytes differ between NEON and the scalar/SSE libjpeg-turbo paths, so this
# does not compare the file to chart-uhdr.snapshot.jpg.
#Requires -Version 5.1
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $false

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Resolve-Path (Join-Path $ScriptDir "..")
$ChartDir = Join-Path $RepoRoot "test\hdr-chart"
$Encoded = Join-Path $ChartDir "chart-uhdr.jpg"

$BinCandidates = @(
	(Join-Path $RepoRoot "ExportHDR.lrplugin\bin\uhdr_repack.exe"),
	(Join-Path $RepoRoot "ExportHDR.lrplugin\bin\uhdr_repack"),
	(Join-Path $RepoRoot "tools\uhdr_repack\build\uhdr_repack.exe"),
	(Join-Path $RepoRoot "tools\uhdr_repack\build\Release\uhdr_repack.exe")
)
$Bin = $null
foreach ($candidate in $BinCandidates) {
	if (Test-Path -LiteralPath $candidate) {
		$Bin = $candidate
		break
	}
}
if (-not $Bin) {
	Write-Error "uhdr_repack.exe not found. Build with .\scripts\build_plugin.ps1"
}

$BinDir = Split-Path -Parent $Bin
$env:PATH = "$BinDir;$env:PATH"

$log = Join-Path ([System.IO.Path]::GetTempPath()) ("hdr-chart-snapshot-" + [guid]::NewGuid().ToString() + ".log")
# Windows PowerShell 5.1 turns redirected native stderr into an error record.
# The encoder logs progress on stderr, so Stop would abort a successful run.
$previousErrorAction = $ErrorActionPreference
$ErrorActionPreference = "Continue"
& $Bin --check-hdr-chart $ChartDir > $log 2>&1
$status = $LASTEXITCODE
$ErrorActionPreference = $previousErrorAction
Get-Content -LiteralPath $log | Write-Output

if (-not (Test-Path -LiteralPath $Encoded)) {
	Remove-Item -Force -ErrorAction SilentlyContinue $log
	Write-Error "encode did not write $Encoded (exit $status)"
}
$logText = Get-Content -LiteralPath $log -Raw
Remove-Item -Force -ErrorAction SilentlyContinue $log
if ($status -ne 0) {
	Write-Error "--check-hdr-chart failed (exit $status)"
}
if ($logText -notmatch "R2020 \+4 gain RGB .* PASS") {
	Write-Error "R2020 +4 chromatic gain gate did not pass"
}

Write-Host "OK: HDR chart stop/color gate passed."
exit 0
