# Install the shared Qt kit described by scripts/qt/windows-x64.stamp.
#Requires -Version 5.1
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Resolve-Path (Join-Path $ScriptDir "..")
$StampPath = Join-Path $RepoRoot "scripts\qt\windows-x64.stamp"
if (-not (Test-Path -LiteralPath $StampPath)) {
	throw "Missing Qt stamp: $StampPath"
}
$stamp = @{}
foreach ($line in Get-Content -LiteralPath $StampPath) {
	if ($line -match '^(?<key>[^=]+)=(?<val>.*)$') {
		$stamp[$Matches.key] = $Matches.val
	}
}
foreach ($key in @("version", "linkage", "aqt_arch", "modules")) {
	if (-not $stamp.ContainsKey($key) -or $stamp[$key] -eq "") {
		throw "Stamp $StampPath missing $key"
	}
}
if ($stamp["linkage"] -ne "shared") {
	throw "Windows stamp linkage must be shared"
}
if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
	throw "python is required to install Qt with aqt"
}
$prefix = $env:QT_ROOT_DIR
if (-not $prefix) {
	$prefix = Join-Path $env:SystemDrive "Qt\$($stamp['version'])\msvc2022_64"
}
$versionDir = Split-Path -Parent $prefix
$outputDir = Split-Path -Parent $versionDir
if ((Split-Path -Leaf $versionDir) -ne $stamp["version"]) {
	throw "QT_ROOT_DIR must be <output>\<version>\msvc2022_64 (got $prefix)"
}
python -m pip install --upgrade pip "git+https://github.com/miurahr/aqtinstall.git"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
python -m aqt install-qt windows desktop $stamp["version"] $stamp["aqt_arch"] --outputdir $outputDir --external 7z
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
python -m aqt install-qt windows desktop $stamp["version"] $stamp["aqt_arch"] -m $stamp["modules"] --outputdir $outputDir --external 7z
exit $LASTEXITCODE
