# Unified Windows build orchestrator for ExportHDR.lrplugin.
# Uses the same CMake presets as build_plugin.sh / GitHub Actions.
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
if ($env:UHDR_ENABLE_GUI -eq "0" -or $env:UHDR_ENABLE_GUI -eq "OFF") {
	$CmakeExtra += "-DUHDR_ENABLE_GUI=OFF"
}

$QtResolved = $false

function Test-QtConfigPresent {
	param([string]$Prefix)
	return Test-Path -LiteralPath (Join-Path $Prefix "lib\cmake\Qt6\Qt6Config.cmake")
}

function Get-QtVersionAtPrefix {
	param([string]$Prefix)
	$qmake = Join-Path $Prefix "bin\qmake6.exe"
	if (-not (Test-Path -LiteralPath $qmake)) {
		$cmd = Get-Command qmake6 -ErrorAction SilentlyContinue
		if ($cmd) { $qmake = $cmd.Source }
		else { return $null }
	}
	try {
		return (& $qmake -query QT_VERSION 2>$null | Select-Object -First 1)
	} catch {
		return $null
	}
}

function Test-QtVersionGe611 {
	param([string]$Version)
	if (-not $Version) { return $false }
	$parts = $Version.Split(".")
	if ($parts.Count -lt 2) { return $false }
	$major = [int]$parts[0]
	$minor = [int]$parts[1]
	return ($major -gt 6) -or ($major -eq 6 -and $minor -ge 11)
}

function Find-QtStaticRoot {
	$candidates = @()
	if ($env:QT_STATIC_ROOT) { $candidates += $env:QT_STATIC_ROOT }
	$qtVersion = if ($env:QT_VERSION) { $env:QT_VERSION } else { "6.11.0" }
	$candidates += (Join-Path $env:USERPROFILE "Qt\$qtVersion-static")
	foreach ($candidate in $candidates) {
		if (Test-QtConfigPresent $candidate) { return $candidate }
	}
	return $null
}

function Find-QtSharedPrefix {
	$seen = @{}
	$candidates = @()

	if ($env:CMAKE_PREFIX_PATH) {
		foreach ($entry in $env:CMAKE_PREFIX_PATH.Split(";")) {
			if ($entry) { $candidates += $entry }
		}
	}

	$qtpaths = Get-Command qtpaths6 -ErrorAction SilentlyContinue
	if ($qtpaths) {
		$prefix = & $qtpaths.Source --install-prefix 2>$null | Select-Object -First 1
		if ($prefix) { $candidates += $prefix }
	}

	$qtRoot = Join-Path $env:USERPROFILE "Qt"
	if (Test-Path -LiteralPath $qtRoot) {
		foreach ($dir in Get-ChildItem -LiteralPath $qtRoot -Directory -ErrorAction SilentlyContinue) {
			foreach ($kit in @("msvc2019_64", "msvc2022_64", "clang_64")) {
				$candidates += (Join-Path $dir.FullName $kit)
			}
		}
	}

	foreach ($candidate in $candidates) {
		if ($seen.ContainsKey($candidate)) { continue }
		$seen[$candidate] = $true
		if (-not (Test-QtConfigPresent $candidate)) { continue }
		$version = Get-QtVersionAtPrefix $candidate
		if ($version -and -not (Test-QtVersionGe611 $version)) { continue }
		return $candidate
	}
	return $null
}

function Resolve-QtForBuild {
	if ($script:QtResolved) { return }

	if ($env:UHDR_ENABLE_GUI -eq "0" -or $env:UHDR_ENABLE_GUI -eq "OFF") {
		$script:QtResolved = $true
		return
	}

	$staticEnabled = -not ($env:UHDR_STATIC_QT -eq "0" -or $env:UHDR_STATIC_QT -eq "OFF")

	if (-not $staticEnabled) {
		$sharedPrefix = Find-QtSharedPrefix
		if (-not $sharedPrefix) {
			throw "UHDR_STATIC_QT=OFF but no Qt 6.11+ installation was found. Install Qt locally or set CMAKE_PREFIX_PATH."
		}
		$script:CmakeExtra += "-DUHDR_STATIC_QT=OFF"
		$script:CmakeExtra += "-DCMAKE_PREFIX_PATH=$sharedPrefix"
		Write-Host "==> Using shared Qt at $sharedPrefix (development build)"
		$script:QtResolved = $true
		return
	}

	$staticRoot = Find-QtStaticRoot
	if ($staticRoot) {
		$script:CmakeExtra += "-DQT_STATIC_ROOT=$staticRoot"
		$script:CmakeExtra += "-DUHDR_STATIC_QT=ON"
		Write-Host "==> Using static Qt at $staticRoot"
		$script:QtResolved = $true
		return
	}

	$sharedPrefix = Find-QtSharedPrefix
	if ($sharedPrefix) {
		$script:CmakeExtra += "-DUHDR_STATIC_QT=OFF"
		$script:CmakeExtra += "-DCMAKE_PREFIX_PATH=$sharedPrefix"
		Write-Host "==> Using shared Qt at $sharedPrefix (local development build)"
		Write-Warning "For a release single-file binary, configure a static Qt 6.11 MSVC kit and set QT_STATIC_ROOT."
		$script:QtResolved = $true
		return
	}

	throw @"
No Qt 6.11+ installation found.
Install Qt locally or configure a static Qt 6.11 MSVC kit and set QT_STATIC_ROOT.
"@
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
	$copyFixtures = Join-Path $ScriptDir "copy_ui_fixtures.sh"
	if (Test-Path -LiteralPath $copyFixtures) {
		Write-Host "==> Copying UI fixtures"
		& bash $copyFixtures
		if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
	}

	$buildExe = Find-BuildExe
	if (-not $buildExe) {
		throw "Build failed: missing uhdr_repack.exe under $BuildDir"
	}

	Write-Host "==> Cleaning old Windows bundle in $PluginBin"
	Clear-PluginBin
	Copy-Item -LiteralPath $buildExe -Destination (Join-Path $PluginBin "uhdr_repack.exe") -Force

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
	Resolve-QtForBuild
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
	"install" {
		Ensure-BuildDependencies -ScriptDir $ScriptDir -InstallDeps:$InstallDeps
		Invoke-CmakeBuild
		Invoke-BundleWindows
		Invoke-TestStep
	}
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
