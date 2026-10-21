# Run a built Windows Studio runtime with its own portable configuration.
# Example: pwsh -File tools/run-studio-windows.ps1 -RuntimeRoot C:\Studio
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $RuntimeRoot,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $ObsArguments
)
$ErrorActionPreference = 'Stop'
$studioRoot = (Resolve-Path -LiteralPath $RuntimeRoot).Path
$studioExe = Join-Path $studioRoot 'bin\64bit\obs64.exe'
if (-not (Test-Path -LiteralPath $studioExe -PathType Leaf)) {
    throw "Build and install the Windows Studio runtime first: $studioExe"
}
# OBS resolves portable config relative to bin/64bit, not to APPDATA.
$studioConfig = Join-Path $studioRoot 'config\obs-studio'
New-Item -ItemType Directory -Force -Path $studioConfig | Out-Null
$studioIni = Join-Path $studioConfig 'user.ini'
if (-not (Test-Path -LiteralPath $studioIni)) {
    $studioSeed = @'
[General]
FirstRun=true
Language=pt-BR

[Appearance]
Theme=org.zyon.Studio

[BasicWindow]
StudioWorkspace=true
SideDocks=true
VerticalVolumeControl=true
'@
    [System.IO.File]::WriteAllText($studioIni, $studioSeed, [System.Text.UTF8Encoding]::new($false))
}
Push-Location -LiteralPath (Split-Path -Parent $studioExe)
try {
    & $studioExe --portable --multi --disable-shutdown-check --disable-updater @ObsArguments
} finally {
    Pop-Location
}
