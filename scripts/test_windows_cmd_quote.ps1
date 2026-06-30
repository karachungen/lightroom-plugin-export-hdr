# Regression test: Windows cmd.exe quoting for plug-in paths with spaces and "(1)".
# Mirrors ExportHDR.lrplugin/Command.lua runShell + buildEncodeCommand (fixed, no nested quote).
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

$stagingRoot = Join-Path $env:TEMP ("uhdr_cmd_quote_test (1)_" + [guid]::NewGuid().ToString("N"))
$pluginBin = Join-Path $stagingRoot "ExportHDR.lrplugin\bin"
New-Item -ItemType Directory -Force -Path $pluginBin | Out-Null

try {
	Copy-Item -LiteralPath $Bin -Destination (Join-Path $pluginBin "uhdr_repack.exe") -Force
	$binDir = $pluginBin
	Get-ChildItem -LiteralPath (Split-Path -Parent $Bin) -Filter "*.dll" -File -ErrorAction SilentlyContinue |
		ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $pluginBin -Force }

	$workDir = Join-Path $env:TEMP ("uhdr_cmd_quote_work_" + [guid]::NewGuid().ToString("N"))
	New-Item -ItemType Directory -Force -Path $workDir | Out-Null
	$encodeHdr = Join-Path $workDir "uhdr_hdr_encode.tif"
	$encodeBase = Join-Path $workDir "uhdr_sdr_base_copy.jpg"
	$encodeOut = Join-Path $workDir "uhdr_out_encode.jpg"
	Copy-Item -LiteralPath $Hdr -Destination $encodeHdr -Force
	Copy-Item -LiteralPath $Base -Destination $encodeBase -Force

	$inner = @(
		"uhdr_repack.exe",
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
	$inner = $inner + " > " + (Invoke-LuaShellQuote $capturePath) + " 2>&1"
	$wrapped = 'cd /d ' + (Invoke-LuaShellQuote $binDir) + ' && ' + $inner
	$cmdLine = 'cmd /c ' + $wrapped

	Write-Host '==> Staged plug-in bin under path with (1):'
	Write-Host "    $binDir"
	Write-Host "==> Command: $cmdLine"

	$proc = Start-Process -FilePath "cmd.exe" -ArgumentList "/c", $wrapped -Wait -PassThru -NoNewWindow
	$exitCode = $proc.ExitCode

	$capture = ""
	if (Test-Path -LiteralPath $capturePath) {
		$capture = Get-Content -LiteralPath $capturePath -Raw -ErrorAction SilentlyContinue
		if ($capture) {
			Write-Host $capture
		}
	}

	if ($capture -match "internal or external command") {
		throw ("FAIL: cmd.exe mangled the command (shell could not run uhdr_repack.exe)." + [Environment]::NewLine + $capture)
	}

	if ($exitCode -ne 0) {
		throw ("FAIL: uhdr_repack exited $exitCode (expected 0)." + [Environment]::NewLine + $capture)
	}

	if (-not (Test-Path -LiteralPath $encodeOut)) {
		throw "FAIL: staged output JPEG missing: $encodeOut"
	}

	if ($capture -notmatch "dimensions:") {
		throw ("FAIL: encoder output missing expected dimensions line." + [Environment]::NewLine + $capture)
	}

	Write-Host 'OK: cmd /c quoting works with plug-in path containing space and (1).'
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
