# Shared Windows build helpers for setup and bundle scripts.
#Requires -Version 5.1

function Test-CommandAvailable {
	param([string]$Name)
	return $null -ne (Get-Command $Name -ErrorAction SilentlyContinue)
}

function Add-PathEntryIfMissing {
	param([string]$PathEntry)
	if (-not $PathEntry -or -not (Test-Path -LiteralPath $PathEntry)) { return }
	$parts = $env:PATH -split ';' | Where-Object { $_ -ne "" }
	if ($parts -notcontains $PathEntry) {
		$env:PATH = "$PathEntry;$env:PATH"
	}
}

function Refresh-BuildToolPath {
	$machinePath = [System.Environment]::GetEnvironmentVariable("PATH", "Machine")
	$userPath = [System.Environment]::GetEnvironmentVariable("PATH", "User")
	if ($machinePath) { $env:PATH = "$machinePath;$userPath" }
	elseif ($userPath) { $env:PATH = $userPath }

	Add-PathEntryIfMissing "C:\Program Files\CMake\bin"
	Add-PathEntryIfMissing "C:\Program Files\Git\cmd"
	Add-PathEntryIfMissing "C:\Program Files\Ninja"
}

function Find-VcVars64Batch {
	$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
	if (Test-Path -LiteralPath $vswhere) {
		$installPath = & $vswhere -all -products * -prerelease -latest `
			-requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
			-property installationPath 2>$null
		if ($installPath) {
			$candidate = Join-Path $installPath "VC\Auxiliary\Build\vcvars64.bat"
			if (Test-Path -LiteralPath $candidate) { return $candidate }
		}
	}

	$roots = @(
		"C:\Program Files\Microsoft Visual Studio\2022",
		"C:\Program Files (x86)\Microsoft Visual Studio\2022",
		"C:\Program Files\Microsoft Visual Studio\18",
		"C:\Program Files\Microsoft Visual Studio\2026"
	)
	foreach ($root in $roots) {
		if (-not (Test-Path -LiteralPath $root)) { continue }
		$matches = Get-ChildItem -LiteralPath $root -Recurse -Filter "vcvars64.bat" -ErrorAction SilentlyContinue |
			Where-Object { $_.FullName -match "\\VC\\Auxiliary\\Build\\vcvars64\.bat$" }
		if ($matches) {
			return ($matches | Select-Object -First 1).FullName
		}
	}
	return $null
}

function Import-MsvcDevEnvironment {
	if (Test-CommandAvailable "cl.exe") { return $true }

	$vcvars = Find-VcVars64Batch
	if (-not $vcvars) { return $false }

	Write-Host "    loading MSVC environment from $vcvars"
	cmd /c "`"$vcvars`" >nul && set" | ForEach-Object {
		if ($_ -match '^(?<key>[^=]+)=(?<val>.*)$') {
			Set-Item -Path "Env:$($Matches.key)" -Value $Matches.val
		}
	}
	return (Test-CommandAvailable "cl.exe")
}

function Get-CmakeExe {
	Refresh-BuildToolPath
	if (Test-CommandAvailable "cmake") {
		$verLine = (& cmake --version 2>$null | Select-Object -First 1)
		if ($verLine -match "version 3\.31\.") {
			return (Get-Command cmake).Source
		}
		if ($verLine) {
			Write-Warning "cmake on PATH is not 3.31.x ($verLine). CMake 4.x breaks vendored libjpeg-turbo; run .\scripts\setup_windows_build.ps1"
		}
	}

	$candidates = @(
		"C:\Program Files\CMake\bin\cmake.exe",
		"C:\Program Files (x86)\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
		"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
		"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
		"C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
	)
	foreach ($candidate in $candidates) {
		if (-not (Test-Path -LiteralPath $candidate)) { continue }
		$verLine = (& $candidate --version 2>$null | Select-Object -First 1)
		if ($verLine -match "version 3\.31\.") {
			Add-PathEntryIfMissing (Split-Path -Parent $candidate)
			return $candidate
		}
	}
	return $null
}

function Get-BashExe {
	Refresh-BuildToolPath
	$cmd = Get-Command bash -ErrorAction SilentlyContinue
	if ($cmd) { return $cmd.Source }
	$candidates = @(
		"C:\Program Files\Git\bin\bash.exe",
		"C:\Program Files\Git\usr\bin\bash.exe"
	)
	foreach ($candidate in $candidates) {
		if (Test-Path -LiteralPath $candidate) { return $candidate }
	}
	return $null
}

function Test-MsvcInstalled {
	if (Test-CommandAvailable "cl.exe") { return $true }
	return ($null -ne (Find-VcVars64Batch))
}

function Ensure-BuildDependencies {
	param(
		[string]$ScriptDir,
		[switch]$InstallDeps
	)

	$missing = @()
	if (-not (Get-CmakeExe)) { $missing += "CMake 3.31.x" }
	if (-not (Test-MsvcInstalled)) { $missing += "MSVC (Visual Studio 2022+ / Build Tools, x64)" }
	Refresh-BuildToolPath
	if (-not (Test-CommandAvailable "git")) { $missing += "Git" }

	if ($missing.Count -eq 0) { return }

	if ($InstallDeps) {
		Write-Host "==> Missing dependencies: $($missing -join ', '). Running setup..."
		& (Join-Path $ScriptDir "setup_windows_build.ps1")
		if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
		Refresh-BuildToolPath
		if (-not (Import-MsvcDevEnvironment)) {
			throw "MSVC still unavailable after setup. Open a new terminal or run .\scripts\setup_windows_build.ps1 as Administrator."
		}
		if (-not (Get-CmakeExe)) {
			throw "CMake 3.31.x still unavailable after setup. Open a new terminal or run .\scripts\setup_windows_build.ps1"
		}
		if (-not (Test-CommandAvailable "git")) {
			throw "Git still unavailable after setup. Open a new terminal or run .\scripts\setup_windows_build.ps1"
		}
		return
	}

	throw @"
Missing build dependencies: $($missing -join ', ')

Run once:
  .\scripts\setup_windows_build.ps1

Or build with automatic install:
  .\scripts\build_plugin.ps1 -InstallDeps
"@
}
