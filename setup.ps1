<#
.SYNOPSIS
    Checks build and runtime requirements for victus-control, installs a C++17
    compiler (w64devkit MinGW-w64) if none is present, and builds victus.exe.

.PARAMETER CheckOnly
    Check requirements without downloading, installing, or building.

.PARAMETER AddToPath
    Also add this repository folder to the current user's PATH.
#>
[CmdletBinding()]
param(
    [switch]$CheckOnly,
    [switch]$AddToPath
)

$ErrorActionPreference = 'Stop'
$RepoDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$W64Root = Join-Path $env:LOCALAPPDATA 'Programs'
$W64Bin  = Join-Path $W64Root 'w64devkit\bin'

function Write-Step($msg) { Write-Host "[*] $msg" -ForegroundColor Cyan }
function Write-Ok($msg)   { Write-Host "[OK] $msg" -ForegroundColor Green }
function Write-Warn($msg) { Write-Host "[WARN] $msg" -ForegroundColor Yellow }

function Find-CppCompiler {
    foreach ($name in @('cl', 'g++', 'clang++')) {
        $cmd = Get-Command $name -ErrorAction SilentlyContinue
        if ($cmd) {
            return [PSCustomObject]@{ Name = $name; Path = $cmd.Source }
        }
    }
    $localGpp = Join-Path $W64Bin 'g++.exe'
    if (Test-Path $localGpp) {
        return [PSCustomObject]@{ Name = 'g++'; Path = $localGpp }
    }
    return $null
}

function Add-UserPathEntry([string]$Dir) {
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $entries = @()
    if ($userPath) {
        $entries = $userPath -split ';' | Where-Object { $_ -ne '' }
    }
    if ($entries -notcontains $Dir) {
        $entries += $Dir
        [Environment]::SetEnvironmentVariable('Path', ($entries -join ';'), 'User')
        Write-Ok "Added to User PATH: $Dir"
    }
    if (($env:Path -split ';') -notcontains $Dir) {
        $env:Path = "$Dir;$env:Path"
    }
}

function Install-W64Devkit {
    Write-Step "Fetching latest w64devkit release metadata from GitHub..."
    $api = 'https://api.github.com/repos/skeeto/w64devkit/releases/latest'
    $release = Invoke-RestMethod -Uri $api -Headers @{ 'User-Agent' = 'victus-control-setup' }
    $asset = $release.assets | Where-Object { $_.name -match '^w64devkit-x64-.*\.7z\.exe$' } | Select-Object -First 1
    if (-not $asset) {
        throw "Could not find w64devkit-x64-*.7z.exe in latest GitHub release."
    }

    New-Item -ItemType Directory -Force -Path $W64Root | Out-Null
    $installer = Join-Path ([System.IO.Path]::GetTempPath()) $asset.name
    $sizeMB = [math]::Round($asset.size / 1MB, 1)
    Write-Step "Downloading $($asset.name) ($sizeMB MB)..."

    $curl = Get-Command curl.exe -ErrorAction SilentlyContinue
    if ($curl) {
        & $curl.Source -L --fail --retry 2 -o $installer $asset.browser_download_url
        if ($LASTEXITCODE -ne 0) { throw "Download failed with curl exit code $LASTEXITCODE." }
    } else {
        Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $installer -UseBasicParsing
    }

    Write-Step "Extracting to $W64Root..."
    $proc = Start-Process -FilePath $installer -ArgumentList '-y', "-o`"$W64Root`"" -Wait -PassThru -NoNewWindow
    if ($proc.ExitCode -ne 0) {
        throw "Self-extractor exited with code $($proc.ExitCode)."
    }

    if (Test-Path -LiteralPath $installer) {
        Remove-Item -LiteralPath $installer -Force -ErrorAction SilentlyContinue
    }

    $gpp = Join-Path $W64Bin 'g++.exe'
    if (-not (Test-Path $gpp)) {
        throw "Extraction succeeded but g++.exe not found at $gpp."
    }
    Add-UserPathEntry $W64Bin
    Write-Ok "Installed w64devkit compiler: $gpp"
}

Write-Step "Checking system requirements..."

if (-not [Environment]::Is64BitOperatingSystem) {
    throw "64-bit Windows is required."
}
Write-Ok "64-bit Windows detected."

# Check HP WMI BIOS class (only warn if missing, since build can still proceed on other machines)
$wmiClass = Get-CimClass -Namespace 'root\wmi' -ClassName 'hpqBIntM' -ErrorAction SilentlyContinue
if ($wmiClass) {
    Write-Ok "HP WMI BIOS class 'hpqBIntM' found in root\wmi."
} else {
    Write-Warn "HP WMI BIOS class 'hpqBIntM' not found (expected only on HP OMEN/Victus laptops)."
}

# Check C++ compiler
$compiler = Find-CppCompiler
if ($compiler) {
    Write-Ok "C++ compiler found: $($compiler.Name) ($($compiler.Path))"
    if ($compiler.Path -like "$W64Bin*") {
        Add-UserPathEntry $W64Bin
    }
} else {
    Write-Warn "No C++ compiler found (checked cl, g++, clang++)."
    if ($CheckOnly) {
        Write-Host "Run .\setup.ps1 without -CheckOnly to install w64devkit (MinGW-w64 g++) automatically."
        exit 1
    }
    Install-W64Devkit
    $compiler = Find-CppCompiler
}

if ($CheckOnly) {
    Write-Ok "All build requirements satisfied."
    exit 0
}

if ($AddToPath) {
    Add-UserPathEntry $RepoDir
}

Write-Step "Building victus.exe..."
Push-Location $RepoDir
try {
    & cmd.exe /c build.bat
    if ($LASTEXITCODE -ne 0) {
        throw "build.bat failed with exit code $LASTEXITCODE."
    }

    $exe = Join-Path $RepoDir 'victus.exe'
    if (-not (Test-Path $exe)) {
        throw "victus.exe was not produced."
    }
    Write-Step "Verifying victus.exe --help..."
    & $exe --help
    if ($LASTEXITCODE -ne 0) {
        throw "victus.exe --help exited with code $LASTEXITCODE."
    }
    Write-Ok "Ready. Open Terminal (Admin) and run: .\victus.exe status"
} finally {
    Pop-Location
}
