# Build uhdr_repack (Windows x64) and install into ExportHDR.lrplugin/bin with bundled DLLs.
#Requires -Version 5.1
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Resolve-Path (Join-Path $ScriptDir "..")
$UhdrSrc = Join-Path $RepoRoot "tools\uhdr_repack"
$BuildDir = Join-Path $UhdrSrc "build"
$PluginBin = Join-Path $RepoRoot "ExportHDR.lrplugin\bin"

function Test-CommandAvailable {
	param([string]$Name)
	return $null -ne (Get-Command $Name -ErrorAction SilentlyContinue)
}

$CmakeExtra = @()
if ($env:UHDR_USE_SYSTEM -eq "1" -or $env:UHDR_USE_SYSTEM -eq "ON") {
	$CmakeExtra += "-DUHDR_USE_SYSTEM=ON"
}
if ($env:UHDR_ROOT) {
	$CmakeExtra += "-DUHDR_ROOT=$($env:UHDR_ROOT)"
}

$script:CmakeBuildUsesConfig = $true

function Reset-BuildDir {
	param([string]$BuildDir)
	if (Test-Path -LiteralPath $BuildDir) {
		Remove-Item -LiteralPath $BuildDir -Recurse -Force
	}
}

function Invoke-CmakeConfigure {
	param(
		[string]$SourceDir,
		[string]$BuildDir,
		[string[]]$ExtraArgs
	)

	# GitHub windows-latest (VS 2026) + pinned CMake 3.31.x: use Ninja with MSVC in PATH.
	if ((Test-CommandAvailable "cl.exe") -and (Test-CommandAvailable "ninja")) {
		Reset-BuildDir $BuildDir
		Write-Host "    using Ninja + MSVC (CMAKE_BUILD_TYPE=Release)"
		& cmake -S $SourceDir -B $BuildDir -G Ninja -DCMAKE_BUILD_TYPE=Release @ExtraArgs
		if ($LASTEXITCODE -eq 0) {
			$script:CmakeBuildUsesConfig = $false
			return $true
		}
	}

	# Full Visual Studio installs: multi-config generators (VS 18 2026 needs CMake 4.2+).
	$generatorCandidates = @(
		@{ G = "Visual Studio 18 2026"; A = "x64" },
		@{ G = "Visual Studio 17 2022"; A = "x64" },
		@{ G = "Visual Studio 16 2019"; A = "x64" }
	)

	foreach ($candidate in $generatorCandidates) {
		Reset-BuildDir $BuildDir
		Write-Host "    trying generator $($candidate.G) -A $($candidate.A)"
		& cmake -S $SourceDir -B $BuildDir -G $candidate.G -A $candidate.A @ExtraArgs
		if ($LASTEXITCODE -eq 0) {
			$script:CmakeBuildUsesConfig = $true
			return $true
		}
	}

	return $false
}

Write-Host "==> Configuring CMake (Release, Windows x64)"
if (-not (Invoke-CmakeConfigure -SourceDir $UhdrSrc -BuildDir $BuildDir -ExtraArgs $CmakeExtra)) {
	throw "CMake configure failed. On CI, MSVC must be on PATH (Ninja). Locally, install Visual Studio 2022+ with the C++ workload (x64) or open a Developer shell."
}

Write-Host "==> Building uhdr_repack"
if ($CmakeBuildUsesConfig) {
	& cmake --build $BuildDir --config Release
} else {
	& cmake --build $BuildDir
}
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$BuildExeCandidates = @(
	(Join-Path $BuildDir "Release\uhdr_repack.exe"),
	(Join-Path $BuildDir "uhdr_repack.exe")
)
$BuildExe = $null
foreach ($candidate in $BuildExeCandidates) {
	if (Test-Path -LiteralPath $candidate) {
		$BuildExe = $candidate
		break
	}
}
if (-not $BuildExe) {
	Write-Error "Build failed: missing uhdr_repack.exe under $BuildDir"
}

New-Item -ItemType Directory -Force -Path $PluginBin | Out-Null
Write-Host "==> Cleaning old Windows bundle in $PluginBin"
Get-ChildItem -LiteralPath $PluginBin -File -ErrorAction SilentlyContinue |
	Where-Object {
		$_.Name -eq "uhdr_repack" -or $_.Name -eq "uhdr_repack.exe" -or $_.Extension -eq ".dylib" -or $_.Extension -eq ".dll"
	} |
	Remove-Item -Force

Copy-Item -LiteralPath $BuildExe -Destination (Join-Path $PluginBin "uhdr_repack.exe") -Force

$DllSearchRoots = @(
	$BuildDir,
	(Join-Path $BuildDir "Release"),
	(Join-Path $BuildDir "_deps\libultrahdr-build"),
	(Join-Path $BuildDir "_deps\libultrahdr-build\Release")
)
$CopiedDlls = @{}
foreach ($root in $DllSearchRoots) {
	if (-not (Test-Path -LiteralPath $root)) { continue }
	Get-ChildItem -LiteralPath $root -Filter "*.dll" -File -ErrorAction SilentlyContinue | ForEach-Object {
		if (-not $CopiedDlls.ContainsKey($_.Name)) {
			Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $PluginBin $_.Name) -Force
			$CopiedDlls[$_.Name] = $true
			Write-Host "    bundled $($_.Name)"
		}
	}
}

$PluginExe = Join-Path $PluginBin "uhdr_repack.exe"
Write-Host "==> Smoke: uhdr_repack.exe --inspect (usage if no args)"
& $PluginExe 2>&1 | Out-Null
if ($LASTEXITCODE -ne 1) {
	Write-Warning "Expected usage exit code 1 when run without args; got $LASTEXITCODE"
}

Write-Host "==> Done. Bundled encoder: $PluginExe"
if ($CopiedDlls.Count -gt 0) {
	Write-Host "    DLLs: $($CopiedDlls.Keys -join ', ')"
}
