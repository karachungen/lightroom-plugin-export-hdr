# Encode the HDR chart and require the Ultra HDR JPEG to match the committed snapshot.
#Requires -Version 5.1
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $false

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Resolve-Path (Join-Path $ScriptDir "..")
$ChartDir = Join-Path $RepoRoot "test\hdr-chart"
$Snapshot = Join-Path $ChartDir "chart-uhdr.snapshot.jpg"
$Encoded = Join-Path $ChartDir "chart-uhdr.jpg"

if (-not (Test-Path -LiteralPath $Snapshot)) {
	Write-Error "missing snapshot: $Snapshot"
}

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
& $Bin --check-hdr-chart $ChartDir *> $log
$status = $LASTEXITCODE
Get-Content -LiteralPath $log | Write-Output

if (-not (Test-Path -LiteralPath $Encoded)) {
	Remove-Item -Force -ErrorAction SilentlyContinue $log
	Write-Error "encode did not write $Encoded (exit $status)"
}
$logText = Get-Content -LiteralPath $log -Raw
Remove-Item -Force -ErrorAction SilentlyContinue $log
if ($logText -notmatch "R2020 \+4 gain RGB .* PASS") {
	Write-Error "R2020 +4 chromatic gain gate did not pass"
}

$left = [System.IO.File]::ReadAllBytes($Snapshot)
$right = [System.IO.File]::ReadAllBytes($Encoded)
$diff = Compare-Object -ReferenceObject ([System.BitConverter]::ToString($left)) -DifferenceObject ([System.BitConverter]::ToString($right))
if ($null -ne $diff) {
	Write-Host "HDR chart snapshot mismatch"
	Write-Host "snapshot: $($left.Length) bytes"
	Write-Host "encoded:  $($right.Length) bytes"
	exit 1
}

Write-Host "OK: chart-uhdr.jpg matches chart-uhdr.snapshot.jpg"
exit 0
