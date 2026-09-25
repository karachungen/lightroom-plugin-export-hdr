# Unified Windows build orchestrator for ExportHDR.lrplugin.
# Delegates to scripts/build_plugin.sh (same path as macOS / CI).
#Requires -Version 5.1
param(
	[Parameter(Position = 0)]
	[ValidateSet("install-deps", "install", "build", "bundle", "test", "package", "all")]
	[string]$Command = "install",
	[switch]$InstallDeps,
	[switch]$Clean,
	[string]$Preset = "windows-x64-release"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildScript = Join-Path $ScriptDir "build_plugin.sh"

$bash = Get-Command bash -ErrorAction SilentlyContinue
if ($bash) {
	$BashExe = $bash.Source
} elseif (Test-Path -LiteralPath "C:\Program Files\Git\bin\bash.exe") {
	$BashExe = "C:\Program Files\Git\bin\bash.exe"
} elseif (Test-Path -LiteralPath "C:\Program Files\Git\usr\bin\bash.exe") {
	$BashExe = "C:\Program Files\Git\usr\bin\bash.exe"
} else {
	throw "bash is required. Install Git for Windows, then run .\scripts\setup_windows_build.ps1"
}

$bashArgs = @($Command)
if ($Clean) {
	$bashArgs += "--clean"
}
$bashArgs += "--preset", $Preset

if ($InstallDeps -and @("build", "bundle", "test", "package") -contains $Command) {
	& $BashExe $BuildScript install-deps
	if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

& $BashExe $BuildScript @bashArgs
exit $LASTEXITCODE
