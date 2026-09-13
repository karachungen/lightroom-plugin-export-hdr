# Smoke-test uhdr_repack on Windows — prefers run_uhdr_test.sh via Git Bash (same as CI).
#Requires -Version 5.1
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $false

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $ScriptDir "windows_build_common.ps1")

$bash = Get-BashExe
if ($bash) {
	& $bash (Join-Path $ScriptDir "run_uhdr_test.sh")
	exit $LASTEXITCODE
}

$RepoRoot = Resolve-Path (Join-Path $ScriptDir "..")
$TestDir = Join-Path $RepoRoot "test"
$Hdr = Join-Path $TestDir "hdr-raw.tif"
$Base = Join-Path $TestDir "sdr.jpg"
$Out = Join-Path $TestDir "out_uhdr.jpg"

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
	Write-Error @"
uhdr_repack.exe not found. Build with:
  .\scripts\build_plugin.ps1
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
	$jpegText = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($Path))
	if ($jpegText -notlike '*https://hdr.karachun.by/*') {
		throw "FAIL: expected xmpRights WebStatement https://hdr.karachun.by/ in $Path"
	}
	if ($jpegText -notlike '*https://github.com/karachungen/lightroom-plugin-export-hdr*') {
		throw "FAIL: expected xmpRights UsageTerms GitHub URL in $Path"
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

$CyrDir = Join-Path $TestDir "тест"
New-Item -ItemType Directory -Force -Path $CyrDir | Out-Null
$CyrOut = Join-Path $CyrDir "out_uhdr.jpg"
Remove-Item -Force -ErrorAction SilentlyContinue $CyrOut
Write-Host "==> Cyrillic folder path test ($CyrDir)"
& $Bin --hdr-tiff $Hdr --base $Base --out $CyrOut
if ($LASTEXITCODE -ne 0) {
	throw "Cyrillic folder encode failed (exit $LASTEXITCODE): $Bin --hdr-tiff $Hdr --base $Base --out $CyrOut"
}
Assert-InspectOk $CyrOut
Write-Host "OK: Cyrillic folder encode — UTF-8 paths work."

$SliceOut = Join-Path $TestDir "out_slice_uhdr.jpg"
$SdrCopy = [System.IO.Path]::GetTempFileName() + ".jpg"
Copy-Item -LiteralPath $Base -Destination $SdrCopy -Force
try {
	Remove-Item -Force -ErrorAction SilentlyContinue $SliceOut, (Join-Path $TestDir "out_slice_uhdr_*.jpg")
	Write-Host "==> Slice test (1x1 + 4x5 single-slide Instagram crop, below 1× stays native)"
	& $Bin --hdr-tiff $Hdr --base $SdrCopy --out $SliceOut --slice-aspect 1x1
	if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
	Assert-InspectOk $SliceOut
	$inspect1x1 = (& $Bin --inspect $SliceOut | Out-String)
	$dim1 = [regex]::Match($inspect1x1, '(?m)^dimensions: (\d+)x(\d+)\s*$')
	if ("$($dim1.Groups[1].Value)x$($dim1.Groups[2].Value)" -ne "1000x1000") {
		throw "FAIL: 1x1 native crop was $($dim1.Groups[1].Value)x$($dim1.Groups[2].Value), expected 1000x1000"
	}
	$slices1x1 = @(Get-ChildItem -LiteralPath $TestDir -Filter "out_slice_uhdr_1x1_*.jpg" -File)
	if ($slices1x1.Count -ne 0) {
		throw "FAIL: default 1x1 crop should write only $SliceOut, found numbered slices"
	}

	Remove-Item -Force -ErrorAction SilentlyContinue $SliceOut, (Join-Path $TestDir "out_slice_uhdr_*.jpg")
	& $Bin --hdr-tiff $Hdr --base $SdrCopy --out $SliceOut --slice-aspect 4x5
	if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
	Assert-InspectOk $SliceOut
	$inspect4x5 = (& $Bin --inspect $SliceOut | Out-String)
	$dimMatch = [regex]::Match($inspect4x5, '(?m)^dimensions: (\d+)x(\d+)\s*$')
	$localH = [int]$dimMatch.Groups[2].Value
	$localW = [int]$dimMatch.Groups[1].Value
	if ("${localW}x${localH}" -ne "800x1000") {
		throw "FAIL: 4x5 native crop was ${localW}x${localH}, expected 800x1000"
	}
	$slices4x5 = @(Get-ChildItem -LiteralPath $TestDir -Filter "out_slice_uhdr_4x5_*.jpg" -File)
	if ($slices4x5.Count -ne 0) {
		throw "FAIL: default 4x5 crop should write only $SliceOut, found numbered slices"
	}

	Write-Host "OK: slice encode — crops below 1× stay native (no silent upscale)."

	$Dsc = Join-Path $RepoRoot "test/ui/fixtures/DSC02993.jpg"
	$DscHdr = Join-Path $RepoRoot "test/ui/fixtures/DSC02993.tif"
	if ((Test-Path -LiteralPath $Dsc) -and (Test-Path -LiteralPath $DscHdr)) {
		$FeedOut = Join-Path $TestDir "out_feed_1080.jpg"
		Remove-Item -Force -ErrorAction SilentlyContinue $FeedOut
		Write-Host "==> Optional 4:5 --out-width 1080 on 1152x1440 fixture"
		& $Bin --hdr-tiff $DscHdr --base $Dsc --out $FeedOut --slice-aspect 4x5 --out-width 1080
		if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
		Assert-InspectOk $FeedOut
		$inspectFeed = (& $Bin --inspect $FeedOut | Out-String)
		$dimFeed = [regex]::Match($inspectFeed, '(?m)^dimensions: (\d+)x(\d+)\s*$')
		if ("$($dimFeed.Groups[1].Value)x$($dimFeed.Groups[2].Value)" -ne "1080x1350") {
			throw "FAIL: 4x5 --out-width 1080 was $($dimFeed.Groups[1].Value)x$($dimFeed.Groups[2].Value), expected 1080x1350"
		}
		Write-Host "OK: 4:5 --out-width 1080 scales 1152x1440 to 1080x1350."
		$SmartOut = Join-Path $TestDir "out_feed_smart.jpg"
		Remove-Item -Force -ErrorAction SilentlyContinue $SmartOut
		Write-Host "==> Optional 4:5 smart pick (no --out-width) on 1152x1440 fixture"
		& $Bin --hdr-tiff $DscHdr --base $Dsc --out $SmartOut --slice-aspect 4x5
		if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
		Assert-InspectOk $SmartOut
		$inspectSmart = (& $Bin --inspect $SmartOut | Out-String)
		$dimSmart = [regex]::Match($inspectSmart, '(?m)^dimensions: (\d+)x(\d+)\s*$')
		if ("$($dimSmart.Groups[1].Value)x$($dimSmart.Groups[2].Value)" -ne "1152x1440") {
			throw "FAIL: 4x5 smart pick was $($dimSmart.Groups[1].Value)x$($dimSmart.Groups[2].Value), expected 1152x1440"
		}
		Write-Host "OK: 4:5 smart pick keeps 1152x1440 (native, below 2x)."
	}
}
finally {
	Remove-Item -Force -ErrorAction SilentlyContinue $SdrCopy
}
exit 0
