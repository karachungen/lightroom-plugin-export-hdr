# Smoke-test uhdr_repack on Windows: encode defaults + --inspect (+ optional slices).
#Requires -Version 5.1
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $false

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Resolve-Path (Join-Path $ScriptDir "..")
$TestDir = Join-Path $RepoRoot "test"
$Hdr = Join-Path $TestDir "hdr-raw.tif"
$Base = Join-Path $TestDir "sdr.jpg"
$Out = Join-Path $TestDir "out_uhdr.jpg"

# Prefer bundled plugin binary (uhdr.dll next to exe); build-tree exe alone often lacks DLLs on Windows.
$BinCandidates = @(
	(Join-Path $RepoRoot "ExportHDR.lrplugin\bin\uhdr_repack.exe"),
	(Join-Path $RepoRoot "tools\uhdr_repack\build\Release\uhdr_repack.exe"),
	(Join-Path $RepoRoot "tools\uhdr_repack\build\uhdr_repack.exe")
)
$Bin = $null
foreach ($candidate in $BinCandidates) {
	if (Test-Path -LiteralPath $candidate) {
		$Bin = $candidate
		break
	}
}
if (-not $Bin) {
	Write-Error @"
uhdr_repack.exe not found. Build with:
  .\scripts\bundle_uhdr_for_plugin_windows.ps1
or manually (Visual Studio generator, not NMake):
  cmake -S tools/uhdr_repack -B tools/uhdr_repack/build -G "Visual Studio 17 2022" -A x64
  cmake --build tools/uhdr_repack/build --config Release
"@
}

$BinDir = Split-Path -Parent $Bin
$env:PATH = "$BinDir;$env:PATH"

if (-not (Test-Path -LiteralPath $Hdr) -or -not (Test-Path -LiteralPath $Base)) {
	Write-Error "Missing test inputs. See test/README.md — need:`n  $Hdr`n  $Base"
}

function Assert-InspectOk {
	param([string]$Path)
	$inspect = (& $Bin --inspect $Path | Out-String)
	$inspect | Write-Output

	$dimMatch = [regex]::Match($inspect, '(?m)^dimensions: (\d+)x(\d+)\s*$')
	$gmMatch = [regex]::Match($inspect, '(?m)^gainmap_size: (\d+)x(\d+)\s*$')
	$xmpMatch = [regex]::Match($inspect, '(?m)^markers: (.+)\s*$')

	if (-not $dimMatch.Success -or -not $gmMatch.Success) {
		throw "inspect: could not parse dimensions / gainmap_size for $Path"
	}
	$dimText = "$($dimMatch.Groups[1].Value)x$($dimMatch.Groups[2].Value)"
	$gmText = "$($gmMatch.Groups[1].Value)x$($gmMatch.Groups[2].Value)"
	if ($dimText -ne $gmText) {
		throw "FAIL: gainmap_size ($gmText) != dimensions ($dimText) for $Path"
	}
	if ($xmpMatch.Success -and $xmpMatch.Groups[1].Value -notmatch 'primary_xmp=(yes|1)') {
		throw "FAIL: expected primary_xmp=yes or primary_xmp=1 for $Path"
	}
	if ($inspect -notmatch '(?m)^is_ultra_hdr: yes\s*$') {
		throw "FAIL: expected is_ultra_hdr: yes for $Path"
	}
}

Write-Host "==> Using $Bin"
Remove-Item -Force -ErrorAction SilentlyContinue $Out, (Join-Path $TestDir "out_uhdr_*.jpg")
& $Bin --hdr-tiff $Hdr --base $Base --out $Out
if ($LASTEXITCODE -ne 0) {
	throw "encode failed (exit $LASTEXITCODE): $Bin --hdr-tiff $Hdr --base $Base --out $Out"
}
Assert-InspectOk $Out
Write-Host "OK: default encode — gain map matches dimensions and primary_xmp is present."

$SliceOut = Join-Path $TestDir "out_slice_uhdr.jpg"
$SdrCopy = [System.IO.Path]::GetTempFileName() + ".jpg"
Copy-Item -LiteralPath $Base -Destination $SdrCopy -Force
try {
	Remove-Item -Force -ErrorAction SilentlyContinue $SliceOut, (Join-Path $TestDir "out_slice_uhdr_*.jpg")
	Write-Host "==> Slice test (1x1 + 4x5, SDR copy preserved like Lightroom plug-in)"
	& $Bin --hdr-tiff $Hdr --base $SdrCopy --out $SliceOut --slice-aspect 1x1
	if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
	Assert-InspectOk $SliceOut

	$slices1x1 = Get-ChildItem -LiteralPath $TestDir -Filter "out_slice_uhdr_1x1_*.jpg" -File
	if ($slices1x1.Count -lt 1) {
		throw "FAIL: expected at least one 1x1 slice next to $SliceOut"
	}
	foreach ($p in $slices1x1) {
		Write-Host "==> inspect slice $($p.FullName)"
		Assert-InspectOk $p.FullName
	}

	Remove-Item -Force -ErrorAction SilentlyContinue $SliceOut, (Join-Path $TestDir "out_slice_uhdr_*.jpg")
	& $Bin --hdr-tiff $Hdr --base $SdrCopy --out $SliceOut --slice-aspect 4x5
	if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
	Assert-InspectOk $SliceOut

	$slices4x5 = Get-ChildItem -LiteralPath $TestDir -Filter "out_slice_uhdr_4x5_*.jpg" -File
	if ($slices4x5.Count -lt 1) {
		throw "FAIL: expected at least one 4x5 slice next to $SliceOut"
	}
	foreach ($p in $slices4x5) {
		Write-Host "==> inspect slice $($p.FullName)"
		Assert-InspectOk $p.FullName
		$inspect = (& $Bin --inspect $p.FullName | Out-String)
		$dimMatch = [regex]::Match($inspect, '(?m)^dimensions: (\d+)x(\d+)\s*$')
		$localH = [int]$dimMatch.Groups[2].Value
		$localW = [int]$dimMatch.Groups[1].Value
		$expectedW = [math]::Floor(($localH * 4 / 5) / 2) * 2
		if ($localW -ne $expectedW) {
			throw "FAIL: 4x5 slice width $localW != expected even floor(H*4/5)=$expectedW"
		}
	}

	Write-Host "OK: slice encode — original + numbered slices are valid Ultra HDR with gain maps."
}
finally {
	Remove-Item -Force -ErrorAction SilentlyContinue $SdrCopy
}
exit 0
