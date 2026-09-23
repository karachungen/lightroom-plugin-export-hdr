#Requires -Version 5.1
# Open the Ultra HDR editor on the committed stop/color chart.
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = (Resolve-Path (Join-Path $ScriptDir "..")).Path
$Hdr = Join-Path $RepoRoot "test\hdr-chart\hdr-chart.tif"
$Sdr = Join-Path $RepoRoot "test\hdr-chart\sdr-chart.jpg"
$Session = Join-Path $RepoRoot "test\hdr-chart\session.json"

function Test-EditGui {
	param([string]$Bin)
	$prev = $ErrorActionPreference
	$ErrorActionPreference = "Continue"
	$out = & $Bin --edit 2>&1 | Out-String
	$ErrorActionPreference = $prev
	return ($out -notmatch "without GUI support")
}

if (-not (Test-Path -LiteralPath $Hdr) -or -not (Test-Path -LiteralPath $Sdr)) {
	throw "Missing chart pair. Generate with: uhdr_repack --write-hdr-chart test\hdr-chart"
}

$candidates = @(
	(Join-Path $RepoRoot "ExportHDR.lrplugin\bin\uhdr_repack.exe"),
	(Join-Path $RepoRoot "tools\uhdr_repack\build\uhdr_repack.exe"),
	(Join-Path $RepoRoot "tools\uhdr_repack\build\Release\uhdr_repack.exe")
)
$Bin = $null
foreach ($c in $candidates) {
	if ((Test-Path -LiteralPath $c) -and (Test-EditGui $c)) {
		$Bin = $c
		break
	}
}
if (-not $Bin) {
	throw @"
uhdr_repack with --edit was not found. Build the GUI binary (do not set UHDR_ENABLE_GUI=OFF):
  .\scripts\build_plugin.ps1 build
"@
}

Set-Location -LiteralPath $RepoRoot
Write-Host "Opening Ultra HDR editor with the stop/color chart"
Write-Host "  $Bin"
Write-Host "  $Hdr"
& $Bin --edit --session $Session
exit $LASTEXITCODE
