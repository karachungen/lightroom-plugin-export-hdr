# Regression test: Windows shell invocation for plug-in paths with spaces and "(N)".
# Mirrors ExportHDR.lrplugin/Command.lua runShell (full quoted exe path, no cmd /c cd).
#Requires -Version 5.1
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $false

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Resolve-Path (Join-Path $ScriptDir "..")
$TestDir = Join-Path $RepoRoot "test"
$Hdr = Join-Path $TestDir "hdr-raw.tif"
$Base = Join-Path $TestDir "sdr.jpg"

function Invoke-LuaShellQuote {
	param([string]$Path)
	if (-not $Path) { return '""' }
	return '"' + ($Path.Replace('"', '""')) + '"'
}

function Get-UhdrBinary {
	$BinCandidates = @(
		(Join-Path $RepoRoot "ExportHDR.lrplugin\bin\uhdr_repack.exe"),
		(Join-Path $RepoRoot "tools\uhdr_repack\build\uhdr_repack.exe"),
		(Join-Path $RepoRoot "tools\uhdr_repack\build\Release\uhdr_repack.exe")
	)
	foreach ($candidate in $BinCandidates) {
		if (Test-Path -LiteralPath $candidate) {
			return $candidate
		}
	}
	return $null
}

function Invoke-CmdCapture {
	param(
		[string]$CommandLine,
		[string]$CapturePath
	)
	if (Test-Path -LiteralPath $CapturePath) {
		Remove-Item -LiteralPath $CapturePath -Force
	}
	# Lightroom LrTasks.execute: one cmd.exe /c layer (system()), not nested cmd /c.
	$proc = Start-Process -FilePath "cmd.exe" -ArgumentList "/c", $CommandLine -Wait -PassThru -NoNewWindow
	$capture = ""
	if (Test-Path -LiteralPath $CapturePath) {
		$capture = Get-Content -LiteralPath $CapturePath -Raw -ErrorAction SilentlyContinue
	}
	return @{ ExitCode = $proc.ExitCode; Capture = $capture }
}

$Bin = Get-UhdrBinary
if (-not $Bin) {
	Write-Error @"
uhdr_repack.exe not found. Build with:
  .\scripts\build_plugin.ps1
"@
}

if (-not (Test-Path -LiteralPath $Hdr) -or -not (Test-Path -LiteralPath $Base)) {
	Write-Error ("Missing test inputs. See test/README.md - need:" + [Environment]::NewLine + "  $Hdr" + [Environment]::NewLine + "  $Base")
}

$stagingRoot = Join-Path $env:TEMP ("uhdr_cmd_quote_test (2)_" + [guid]::NewGuid().ToString("N"))
$pluginBin = Join-Path $stagingRoot "ExportHDR.lrplugin\bin"
New-Item -ItemType Directory -Force -Path $pluginBin | Out-Null

$workDir = $null
try {
	Copy-Item -LiteralPath $Bin -Destination (Join-Path $pluginBin "uhdr_repack.exe") -Force
	$stagedExe = Join-Path $pluginBin "uhdr_repack.exe"
	Get-ChildItem -LiteralPath (Split-Path -Parent $Bin) -Filter "*.dll" -File -ErrorAction SilentlyContinue |
		ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $pluginBin -Force }

	$workDir = Join-Path $env:TEMP ("uhdr_cmd_quote_work_" + [guid]::NewGuid().ToString("N"))
	New-Item -ItemType Directory -Force -Path $workDir | Out-Null
	$encodeHdr = Join-Path $workDir "uhdr_hdr_encode.tif"
	$encodeBase = Join-Path $workDir "uhdr_sdr_base_copy.jpg"
	$encodeOut = Join-Path $workDir "uhdr_out_encode.jpg"
	Copy-Item -LiteralPath $Hdr -Destination $encodeHdr -Force
	Copy-Item -LiteralPath $Base -Destination $encodeBase -Force

	$argsTail = @(
		"--hdr-tiff", (Invoke-LuaShellQuote $encodeHdr),
		"--base", (Invoke-LuaShellQuote $encodeBase),
		"--out", (Invoke-LuaShellQuote $encodeOut),
		"--base-quality", "92",
		"--gainmap-quality", "85",
		"--gainmap-scale", "1",
		"--min-content-boost", "1",
		"--max-content-boost", "1000",
		"--target-display-peak", "1000"
	) -join " "

	$capturePath = Join-Path $workDir "uhdr_run_capture.txt"
	$redirect = " > " + (Invoke-LuaShellQuote $capturePath) + " 2>&1"

	Write-Host '==> Staged plug-in bin under path with (2):'
	Write-Host "    $pluginBin"

	# Current plug-in behavior: full quoted exe path, single shell layer.
	$resolved = (Invoke-LuaShellQuote $stagedExe) + " " + $argsTail + $redirect
	Write-Host "==> Execute (resolved): $resolved"
	$ok = Invoke-CmdCapture -CommandLine $resolved -CapturePath $capturePath
	if ($ok.Capture) { Write-Host $ok.Capture }

	if ($ok.Capture -match "internal or external command") {
		throw ("FAIL: shell could not run quoted uhdr_repack.exe." + [Environment]::NewLine + $ok.Capture)
	}
	if ($ok.ExitCode -ne 0) {
		throw ("FAIL: uhdr_repack exited $($ok.ExitCode) (expected 0)." + [Environment]::NewLine + $ok.Capture)
	}
	if (-not (Test-Path -LiteralPath $encodeOut)) {
		throw "FAIL: staged output JPEG missing: $encodeOut"
	}
	if ($ok.Capture -notmatch "dimensions:") {
		throw ("FAIL: encoder output missing expected dimensions line." + [Environment]::NewLine + $ok.Capture)
	}
	Write-Host "OK: full-path invocation works with plug-in path containing space and (2)."

	# Legacy v2.0.1 pattern: cmd /c cd /d ... && relative exe (should fail without encoder output).
	$legacyCapturePath = Join-Path $workDir "uhdr_run_legacy_capture.txt"
	$legacyInner = "uhdr_repack.exe " + $argsTail + " > " + (Invoke-LuaShellQuote $legacyCapturePath) + " 2>&1"
	$legacyWrapped = 'cd /d ' + (Invoke-LuaShellQuote $pluginBin) + ' && ' + $legacyInner
	$legacyCmd = 'cmd /c ' + $legacyWrapped
	Write-Host '==> Legacy pattern (cd + relative exe + nested cmd /c): expect failure'
	$legacy = Invoke-CmdCapture -CommandLine $legacyCmd -CapturePath $legacyCapturePath
	if ($legacy.Capture) { Write-Host $legacy.Capture }

	$legacyBroken = ($legacy.ExitCode -ne 0) -or ($legacy.Capture -match "internal or external command")
	if (-not $legacyBroken) {
		throw "FAIL: legacy cd/relative/cmd pattern unexpectedly succeeded; regression guard is stale."
	}
	Write-Host "OK: legacy cd/relative/cmd pattern fails as expected (documents why plug-in avoids it)."
}
finally {
	if (Test-Path -LiteralPath $stagingRoot) {
		Remove-Item -LiteralPath $stagingRoot -Recurse -Force -ErrorAction SilentlyContinue
	}
	if ($workDir -and (Test-Path -LiteralPath $workDir)) {
		Remove-Item -LiteralPath $workDir -Recurse -Force -ErrorAction SilentlyContinue
	}
}

exit 0
