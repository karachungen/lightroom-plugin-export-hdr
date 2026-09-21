# Build a real static Qt kit for Windows x64 release binaries.
#Requires -Version 5.1
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $ScriptDir "windows_build_common.ps1")

$QtVersion = if ($env:QT_VERSION) { $env:QT_VERSION } else { "6.11.0" }
$Prefix = if ($env:QT_STATIC_ROOT) { $env:QT_STATIC_ROOT } else { Join-Path $env:USERPROFILE "Qt\$QtVersion-static" }
$CacheDir = if ($env:QT_SOURCE_CACHE) { $env:QT_SOURCE_CACHE } else { Join-Path $env:LOCALAPPDATA "uhdr-qt-static" }
$Archive = Join-Path $CacheDir "qt-everywhere-src-$QtVersion.tar.xz"
$SourceDir = Join-Path $CacheDir "qt-everywhere-src-$QtVersion"
$BuildDir = Join-Path $CacheDir "build-$QtVersion-static"
$QtMinor = ($QtVersion -split '\.')[0..1] -join '.'
$Url = "https://download.qt.io/archive/qt/$QtMinor/$QtVersion/single/qt-everywhere-src-$QtVersion.tar.xz"

function Test-QtConfigPresent {
	param([string]$Path)
	return Test-Path -LiteralPath (Join-Path $Path "lib\cmake\Qt6\Qt6Config.cmake")
}

if (Test-QtConfigPresent $Prefix) {
	Write-Host "==> Static Qt already installed at $Prefix"
	exit 0
}

$cmake = Get-CmakeExe
if (-not $cmake) {
	throw "CMake 3.31.x is required. Run the workflow Setup CMake step or .\scripts\setup_windows_build.ps1"
}
if (-not (Import-MsvcDevEnvironment)) {
	throw "MSVC x64 environment is required. Run the workflow Setup MSVC step or .\scripts\setup_windows_build.ps1"
}
Refresh-BuildToolPath
if (-not (Test-CommandAvailable "ninja")) {
	throw "ninja is required on PATH (install via winget/choco or Visual Studio C++ workload)"
}
if (-not (Test-CommandAvailable "tar")) {
	throw "tar is required to extract Qt sources (Windows 10+ tar.exe)"
}

New-Item -ItemType Directory -Force -Path $CacheDir | Out-Null

if (-not (Test-Path -LiteralPath $Archive)) {
	Write-Host "==> Downloading Qt $QtVersion sources"
	$curl = Get-Command curl -ErrorAction SilentlyContinue
	if ($curl) {
		& $curl.Source --fail --location --retry 3 $Url --output $Archive
		if ($LASTEXITCODE -ne 0) {
			throw "Failed to download Qt sources from $Url"
		}
	} else {
		Invoke-WebRequest -Uri $Url -OutFile $Archive -UseBasicParsing
	}
}

if (-not (Test-Path -LiteralPath $SourceDir)) {
	Write-Host "==> Extracting Qt sources"
	& tar -xf $Archive -C $CacheDir
	if ($LASTEXITCODE -ne 0) {
		throw "Failed to extract $Archive"
	}
}

Write-Host "==> Configuring static Qt $QtVersion at $Prefix"
if (Test-Path -LiteralPath $BuildDir) {
	Remove-Item -LiteralPath $BuildDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

$configureBat = Join-Path $SourceDir "configure.bat"
if (-not (Test-Path -LiteralPath $configureBat)) {
	throw "Missing Qt configure script: $configureBat"
}

Push-Location $BuildDir
try {
	& $configureBat `
		-prefix $Prefix `
		-release `
		-static `
		-opensource `
		-confirm-license `
		-nomake examples `
		-nomake tests `
		-submodules qtbase,qtshadertools `
		-- `
		-GNinja `
		-DQT_BUILD_TOOLS_BY_DEFAULT=ON `
		-DQT_BUILD_TESTS=OFF `
		-DQT_BUILD_EXAMPLES=OFF
	if ($LASTEXITCODE -ne 0) {
		throw "Qt configure failed (exit $LASTEXITCODE)"
	}
} finally {
	Pop-Location
}

Write-Host "==> Building and installing static Qt (this can take a while)"
& $cmake --build $BuildDir --parallel
if ($LASTEXITCODE -ne 0) {
	throw "Qt build failed (exit $LASTEXITCODE)"
}
& $cmake --install $BuildDir
if ($LASTEXITCODE -ne 0) {
	throw "Qt install failed (exit $LASTEXITCODE)"
}

Write-Host ""
Write-Host "Static Qt installed. Build the single executable with:"
Write-Host "  `$env:QT_STATIC_ROOT = `"$Prefix`""
Write-Host "  .\scripts\build_plugin.ps1 build -Clean"
