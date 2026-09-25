# Install the WebView2 SDK layout the encoder links.
#Requires -Version 5.1
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$sdkRoot = "C:\WebView2Sdk"
$marker = Join-Path $sdkRoot "include\WebView2.h"
if (Test-Path -LiteralPath $marker) {
	Write-Host "WebView2 SDK already present at $sdkRoot"
	exit 0
}
$pkgRoot = Join-Path $env:TEMP "webview2-nuget"
$nuget = Join-Path $env:TEMP "nuget.exe"
if (-not (Test-Path -LiteralPath $nuget)) {
	Invoke-WebRequest https://dist.nuget.org/win-x86-commandline/latest/nuget.exe -OutFile $nuget
}
if (Test-Path -LiteralPath $pkgRoot) {
	Remove-Item -LiteralPath $pkgRoot -Recurse -Force
}
& $nuget install Microsoft.Web.WebView2 -OutputDirectory $pkgRoot -ExcludeVersion
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$pkg = Join-Path $pkgRoot "Microsoft.Web.WebView2"
New-Item -ItemType Directory -Force -Path (Join-Path $sdkRoot "include"), (Join-Path $sdkRoot "lib\x64") | Out-Null
Copy-Item -Path (Join-Path $pkg "build\native\include\*") -Destination (Join-Path $sdkRoot "include") -Recurse -Force
Copy-Item -Path (Join-Path $pkg "build\native\x64\WebView2LoaderStatic.lib") -Destination (Join-Path $sdkRoot "lib\x64") -Force
Write-Host "WebView2 SDK installed at $sdkRoot"
