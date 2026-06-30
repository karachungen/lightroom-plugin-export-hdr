# Install local Windows build dependencies for uhdr_repack / ExportHDR.lrplugin.
#Requires -Version 5.1
param(
	[switch]$VsOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $ScriptDir "windows_build_common.ps1")

function Test-IsAdministrator {
	$principal = New-Object Security.Principal.WindowsPrincipal(
		[Security.Principal.WindowsIdentity]::GetCurrent()
	)
	return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-WingetExe {
	$cmd = Get-Command winget -ErrorAction SilentlyContinue
	if ($cmd) { return $cmd.Source }
	return $null
}

function Test-WingetPackageInstalled {
	param(
		[string]$Id,
		[string]$Version = ""
	)
	$winget = Get-WingetExe
	if (-not $winget) { return $false }
	$listArgs = @("list", "--id", $Id, "--exact", "--disable-interactivity")
	if ($Version -ne "") {
		$listArgs += @("--version", $Version)
	}
	$output = & $winget @listArgs 2>&1 | Out-String
	return ($LASTEXITCODE -eq 0 -and $output -match [regex]::Escape($Id))
}

function Install-WingetPackage {
	param(
		[string]$Id,
		[string[]]$ExtraArgs = @()
	)
	$winget = Get-WingetExe
	if (-not $winget) {
		throw "winget is required to install build dependencies. Install App Installer from the Microsoft Store."
	}
	$installArgs = @(
		"install", "-e", "--id", $Id,
		"--accept-package-agreements", "--accept-source-agreements",
		"--disable-interactivity"
	) + $ExtraArgs
	Write-Host "==> winget install $Id $($ExtraArgs -join ' ')"
	& $winget @installArgs
	if ($LASTEXITCODE -ne 0) {
		throw "winget install failed for $Id (exit $LASTEXITCODE)"
	}
}

function Get-VisualStudioInstancePaths {
	$paths = @()
	$instancesRoot = "C:\ProgramData\Microsoft\VisualStudio\Packages\_Instances"
	if (-not (Test-Path -LiteralPath $instancesRoot)) { return $paths }
	Get-ChildItem -LiteralPath $instancesRoot -Directory -ErrorAction SilentlyContinue | ForEach-Object {
		$stateFile = Join-Path $_.FullName "state.json"
		if (-not (Test-Path -LiteralPath $stateFile)) { return }
		try {
			$state = Get-Content -LiteralPath $stateFile -Raw | ConvertFrom-Json
			if ($state.installationPath) {
				$paths += $state.installationPath
			}
		} catch {
		}
	}
	return ($paths | Select-Object -Unique)
}

function Add-VcToolsToExistingVisualStudio {
	param([string]$ScriptDir)

	$setup = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\setup.exe"
	if (-not (Test-Path -LiteralPath $setup)) { return $false }

	$config = Join-Path $ScriptDir "windows-vctools.vsconfig"
	if (-not (Test-Path -LiteralPath $config)) { return $false }

	$modified = $false
	foreach ($installPath in (Get-VisualStudioInstancePaths)) {
		if (-not (Test-Path -LiteralPath $installPath)) { continue }
		Write-Host "==> Adding C++ workload to existing Visual Studio at $installPath"
		Push-Location $env:TEMP
		try {
			$p = Start-Process -FilePath $setup -ArgumentList @(
				"modify",
				"--installPath", $installPath,
				"--config", $config,
				"--passive", "--wait", "--norestart"
			) -Wait -PassThru
			Write-Host "    setup.exe modify exit: $($p.ExitCode)"
			if ($p.ExitCode -eq 0) { $modified = $true }
		} finally {
			Pop-Location
		}
	}
	return $modified
}

function Get-SystemDriveFreeSpaceGb {
	$systemDrive = $env:SystemDrive
	if (-not $systemDrive) { $systemDrive = "C:" }
	$drive = Get-PSDrive -Name $systemDrive.TrimEnd(':') -ErrorAction SilentlyContinue
	if (-not $drive) { return $null }
	return [math]::Round($drive.Free / 1GB, 2)
}

function Assert-SufficientDiskSpace {
	param(
		[double]$RequiredGb = 10
	)
	$freeGb = Get-SystemDriveFreeSpaceGb
	if ($null -eq $freeGb) { return }
	if ($freeGb -lt $RequiredGb) {
		throw @"
Not enough free space on $env:SystemDrive ($freeGb GB free; need at least $RequiredGb GB).

Visual Studio C++ Build Tools need several GB for MSVC, Windows SDK, and CMake components.
Free disk space, then run:
  .\scripts\setup_windows_build.ps1
"@
	}
}

function Install-MsvcBuildTools {
	param([string]$ScriptDir)

	if (Test-MsvcInstalled) {
		Write-Host "    MSVC: OK"
		return
	}

	Assert-SufficientDiskSpace -RequiredGb 10

	if (-not (Test-IsAdministrator)) {
		Write-Host "==> MSVC install requires Administrator privileges."
		Write-Host "    Re-launching elevated setup (accept the UAC prompt)..."
		$self = $MyInvocation.MyCommand.Path
		$argList = "-NoProfile -ExecutionPolicy Bypass -File `"$self`" -VsOnly"
		Start-Process -FilePath "powershell.exe" -ArgumentList $argList -Verb RunAs -Wait
		if (-not (Test-MsvcInstalled)) {
			throw @"
MSVC is still unavailable after elevated setup.

Install manually (Administrator PowerShell):
  winget install -e --id Microsoft.VisualStudio.2022.BuildTools --override `"--passive --wait --add Microsoft.VisualStudio.Workload.VCTools;includeRecommended`"
"@
		}
		return
	}

	if (Add-VcToolsToExistingVisualStudio -ScriptDir $ScriptDir) {
		Start-Sleep -Seconds 3
		if (Test-MsvcInstalled) {
			Write-Host "    MSVC: OK (added to existing Visual Studio)"
			return
		}
	}

	Write-Host "==> Installing Visual Studio 2022 Build Tools (MSVC x64). This can take several minutes."
	$vsOverride = "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools;includeRecommended"
	Install-WingetPackage -Id "Microsoft.VisualStudio.2022.BuildTools" -ExtraArgs @("--override", $vsOverride)

	Start-Sleep -Seconds 3
	if (-not (Test-MsvcInstalled)) {
		$freeGb = Get-SystemDriveFreeSpaceGb
		throw @"
Visual Studio Build Tools finished but MSVC x64 was not detected.

Common causes:
  - Low disk space on $env:SystemDrive (currently about $freeGb GB free; need ~10+ GB)
  - Install still running in Visual Studio Installer (check the taskbar)
  - Reboot required after a partial install

If you already have Visual Studio, open Visual Studio Installer, Modify your install,
and enable the ""Desktop development with C++"" workload, then run:
  .\scripts\build_plugin.ps1
"@
	}
	Write-Host "    MSVC: OK"
}

Write-Host "==> Checking local Windows build dependencies"

if (-not $VsOnly) {
	if (-not (Test-CommandAvailable "git")) {
		if (-not (Test-WingetPackageInstalled -Id "Git.Git")) {
			Install-WingetPackage -Id "Git.Git"
		}
	} else {
		Write-Host "    git: OK"
	}

	$cmakeVersion = "3.31.6"
	$cmakeOk = $false
	if (Test-CommandAvailable "cmake") {
		$verLine = (& cmake --version 2>$null | Select-Object -First 1)
		if ($verLine -match "3\.31\.") {
			$cmakeOk = $true
			Write-Host "    cmake: OK ($verLine)"
		} else {
			Write-Host "    cmake: found but wrong version ($verLine); need 3.31.x (CMake 4.x breaks vendored libjpeg-turbo)"
		}
	}
	if (-not $cmakeOk) {
		if (-not (Test-WingetPackageInstalled -Id "Kitware.CMake" -Version $cmakeVersion)) {
			Install-WingetPackage -Id "Kitware.CMake" -ExtraArgs @("--version", $cmakeVersion)
		} else {
			Write-Host "    cmake ${cmakeVersion}: installed via winget (refresh PATH or open a new shell if cmake is missing)"
		}
	}

	if (-not (Test-CommandAvailable "ninja")) {
		if (-not (Test-WingetPackageInstalled -Id "Ninja-build.Ninja")) {
			Install-WingetPackage -Id "Ninja-build.Ninja"
		}
	} else {
		Write-Host "    ninja: OK"
	}
}

Install-MsvcBuildTools -ScriptDir $ScriptDir

Write-Host ""
Write-Host "==> Dependency setup complete."
Write-Host "    Open a new PowerShell window (or restart the terminal) so PATH includes Git, CMake, and Ninja."
Write-Host "    Then run: .\scripts\build_plugin.ps1"
exit 0
