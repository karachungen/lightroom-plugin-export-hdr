# Unified Windows build orchestrator for ExportHDR.lrplugin.
# Uses the same CMake presets as build_plugin.sh / GitHub Actions.
#Requires -Version 5.1
param(
	[Parameter(Position = 0)]
	[ValidateSet("install-deps", "build", "bundle", "test", "package", "all")]
	[string]$Command = "all",
	[switch]$InstallDeps,
	[switch]$Clean,
	[string]$Preset = "windows-x64-release"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $false

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $ScriptDir "windows_build_common.ps1")

$RepoRoot = Resolve-Path (Join-Path $ScriptDir "..")
$UhdrSrc = Join-Path $RepoRoot "tools\uhdr_repack"
$BuildDir = Join-Path $UhdrSrc "build"
$PluginBin = Join-Path $RepoRoot "ExportHDR.lrplugin\bin"
$PresetName = $Preset

$CmakeExtra = @()
if ($env:UHDR_USE_SYSTEM -eq "1" -or $env:UHDR_USE_SYSTEM -eq "ON") {
	$CmakeExtra += "-DUHDR_USE_SYSTEM=ON"
}
if ($env:UHDR_ROOT) {
	$CmakeExtra += "-DUHDR_ROOT=$($env:UHDR_ROOT)"
}

function Find-BuildExe {
	$candidates = @(
		(Join-Path $BuildDir "uhdr_repack.exe"),
		(Join-Path $BuildDir "Release\uhdr_repack.exe")
	)
	foreach ($c in $candidates) {
		if (Test-Path -LiteralPath $c) { return $c }
	}
	return $null
}

function Clear-PluginBin {
	New-Item -ItemType Directory -Force -Path $PluginBin | Out-Null
	if (Test-Path (Join-Path $PluginBin "uhdr_repack.exe")) {
		Remove-Item (Join-Path $PluginBin "uhdr_repack.exe") -Force
	}
	Get-ChildItem -LiteralPath $PluginBin -File -ErrorAction SilentlyContinue |
		Where-Object {
			$_.Extension -eq ".dylib" -or $_.Extension -eq ".dll" -or
			($_.Name -eq "uhdr_repack" -and $_.Extension -eq "")
		} |
		Remove-Item -Force
}

function Invoke-BundleWindows {
	$buildExe = Find-BuildExe
	if (-not $buildExe) {
		throw "Build failed: missing uhdr_repack.exe under $BuildDir"
	}

	Write-Host "==> Cleaning old Windows bundle in $PluginBin"
	Clear-PluginBin
	Copy-Item -LiteralPath $buildExe -Destination (Join-Path $PluginBin "uhdr_repack.exe") -Force

	$dllRoots = @(
		$BuildDir,
		(Join-Path $BuildDir "Release"),
		(Join-Path $BuildDir "_deps\libultrahdr-build"),
		(Join-Path $BuildDir "_deps\libultrahdr-build\Release")
	)
	$copied = @{}
	foreach ($root in $dllRoots) {
		if (-not (Test-Path -LiteralPath $root)) { continue }
		Get-ChildItem -LiteralPath $root -Filter "*.dll" -File -ErrorAction SilentlyContinue | ForEach-Object {
			if (-not $copied.ContainsKey($_.Name)) {
				Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $PluginBin $_.Name) -Force
				$copied[$_.Name] = $true
				Write-Host "    bundled $($_.Name)"
			}
		}
	}

	$pluginExe = Join-Path $PluginBin "uhdr_repack.exe"
	Write-Host "==> Smoke: uhdr_repack.exe (usage if no args)"
	$prevEap = $ErrorActionPreference
	$ErrorActionPreference = "Continue"
	& $pluginExe 2>&1 | Out-Null
	$ec = $LASTEXITCODE
	$ErrorActionPreference = $prevEap
	if ($ec -ne 1) {
		Write-Warning "Expected usage exit code 1 when run without args; got $ec"
	}
	Write-Host "==> Bundled encoder: $pluginExe"
}

function Invoke-CmakeBuild {
	if ($Clean -and (Test-Path -LiteralPath $BuildDir)) {
		Write-Host "==> Cleaning $BuildDir"
		Remove-Item -LiteralPath $BuildDir -Recurse -Force
	}

	$CmakeExe = Get-CmakeExe
	if (-not $CmakeExe) {
		throw "CMake 3.31.x not found. Run .\scripts\setup_windows_build.ps1"
	}
	if (-not (Import-MsvcDevEnvironment)) {
		throw "MSVC not found. Run .\scripts\setup_windows_build.ps1 as Administrator."
	}

	Write-Host "==> Using CMake: $CmakeExe"
	Write-Host "==> Configuring preset: $PresetName"
	& $CmakeExe --preset $PresetName -S $UhdrSrc @CmakeExtra
	if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

	Write-Host "==> Building preset: $PresetName"
	& $CmakeExe --build $BuildDir
	if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

function Invoke-TestStep {
	$bash = Get-BashExe
	if ($bash) {
		& $bash (Join-Path $ScriptDir "run_uhdr_test.sh")
		if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
		return
	}
	& (Join-Path $ScriptDir "run_uhdr_test.ps1")
	if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

function Invoke-PackageStep {
	$bash = Get-BashExe
	if ($bash) {
		& $bash (Join-Path $ScriptDir "package_plugin.sh") "windows-x64"
		if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
		return
	}

	$artifact = "ExportHDR.lrplugin-windows-x64.zip"
	$outZip = Join-Path $RepoRoot $artifact
	if (Test-Path (Join-Path $PluginBin "uhdr_repack")) {
		Remove-Item (Join-Path $PluginBin "uhdr_repack") -Force
	}
	Get-ChildItem -LiteralPath $PluginBin -Filter "*.dylib" -ErrorAction SilentlyContinue | Remove-Item -Force
	if (-not (Test-Path (Join-Path $PluginBin "uhdr_repack.exe"))) {
		throw "Missing Windows binary: $PluginBin\uhdr_repack.exe"
	}

	$staging = Join-Path $env:TEMP ("uhdr_plugin_pkg_" + [guid]::NewGuid().ToString())
	New-Item -ItemType Directory -Force -Path $staging | Out-Null
	try {
		Copy-Item -LiteralPath (Join-Path $RepoRoot "ExportHDR.lrplugin") -Destination (Join-Path $staging "ExportHDR.lrplugin") -Recurse -Force
		$binStaging = Join-Path $staging "ExportHDR.lrplugin\bin"
		Remove-Item (Join-Path $binStaging ".gitignore") -Force -ErrorAction SilentlyContinue
		Remove-Item (Join-Path $binStaging "README.txt") -Force -ErrorAction SilentlyContinue
		if (Test-Path -LiteralPath $outZip) { Remove-Item -LiteralPath $outZip -Force }
		Compress-Archive -Path (Join-Path $staging "ExportHDR.lrplugin") -DestinationPath $outZip -Force
		Write-Host "Created $outZip"
	} finally {
		Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
	}
}

function Invoke-InstallDeps {
	if ($env:GITHUB_ACTIONS -eq "true") {
		Write-Host "==> Windows CI: dependencies provided by workflow actions (skipping install-deps)"
		return
	}
	& (Join-Path $ScriptDir "setup_windows_build.ps1")
	if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if ($InstallDeps) {
	Ensure-BuildDependencies -ScriptDir $ScriptDir -InstallDeps
}

switch ($Command) {
	"install-deps" { Invoke-InstallDeps }
	"build" {
		Ensure-BuildDependencies -ScriptDir $ScriptDir -InstallDeps:$InstallDeps
		Invoke-CmakeBuild
	}
	"bundle" { Invoke-BundleWindows }
	"test" { Invoke-TestStep }
	"package" { Invoke-PackageStep }
	"all" {
		Ensure-BuildDependencies -ScriptDir $ScriptDir -InstallDeps:$InstallDeps
		Invoke-CmakeBuild
		Invoke-BundleWindows
		Invoke-TestStep
		Invoke-PackageStep
	}
}

exit 0
