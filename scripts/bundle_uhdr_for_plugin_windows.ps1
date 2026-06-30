# Backward-compatible alias: build + bundle (no test/package).
# Prefer: .\scripts\build_plugin.ps1
#Requires -Version 5.1
param(
	[switch]$InstallDeps
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$params = @{ Command = "build" }
if ($InstallDeps) { $params.InstallDeps = $true }
& (Join-Path $ScriptDir "build_plugin.ps1") @params
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $ScriptDir "build_plugin.ps1") bundle
exit $LASTEXITCODE
