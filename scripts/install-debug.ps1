#requires -Version 5.1

[CmdletBinding()]
param(
    [string]$BuildDir,

    [string]$InstallDir = "$HOME\.joyeer\bin",

    [switch]$SkipPathUpdate,

    [string]$SkillDir = "$HOME\.agents\skills\joyeer",

    [switch]$SkipSkillInstall,

    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Add-InstallPath {
    param(
        [AllowEmptyString()][string]$PathValue,
        [string]$Entry,
        [string]$AdditionalPath = ''
    )

    $normalizedEntry = $Entry.Replace('/', '\').TrimEnd([char]'\')
    foreach ($candidate in (($PathValue + ';' + $AdditionalPath) -split ';')) {
        if ([string]::IsNullOrWhiteSpace($candidate)) {
            continue
        }
        $expanded = [Environment]::ExpandEnvironmentVariables($candidate.Trim().Trim([char]'"'))
        if ($expanded.Replace('/', '\').TrimEnd([char]'\') -ieq $normalizedEntry) {
            return $PathValue
        }
    }
    if ([string]::IsNullOrWhiteSpace($PathValue)) {
        return $Entry
    }
    return $PathValue.TrimEnd([char]';') + ';' + $Entry
}

if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
    throw 'This debug installer supports Windows only.'
}

function Get-DefaultDebugBuildDir {
    param(
        [string]$SourceDirectory,
        [string]$NativeArchitecture = [Environment]::GetEnvironmentVariable('PROCESSOR_ARCHITECTURE', 'Machine')
    )

    $preset = switch ($NativeArchitecture) {
        'ARM64' { 'arm64-debug' }
        'AMD64' { 'x64-debug' }
        default { throw "Unsupported Windows architecture '$NativeArchitecture'; specify -BuildDir explicitly." }
    }
    return Join-Path $SourceDirectory "out\build\$preset"
}

$sourceDir = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Get-DefaultDebugBuildDir -SourceDirectory $sourceDir
    Write-Host "Selected native Debug build: $BuildDir"
}
if (-not (Test-Path -LiteralPath $BuildDir -PathType Container)) {
    throw "Debug build directory not found: $BuildDir. Configure and build the matching Debug preset first, or specify -BuildDir."
}
$buildPath = (Resolve-Path -LiteralPath $BuildDir).ProviderPath
$installPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($InstallDir)
$cachePath = Join-Path $buildPath 'CMakeCache.txt'
if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf)) {
    throw "No CMakeCache.txt in $buildPath; configure and build Debug first."
}

$cache = @{}
foreach ($line in Get-Content -LiteralPath $cachePath) {
    if ($line -match '^([^/#][^:]*):[^=]+=(.*)$') {
        $cache[$Matches[1]] = $Matches[2]
    }
}
if ($cache['CMAKE_PROJECT_NAME'] -ne 'joyeer' -or
    -not $cache['CMAKE_HOME_DIRECTORY'] -or
    [IO.Path]::GetFullPath($cache['CMAKE_HOME_DIRECTORY']) -ne $sourceDir) {
    throw 'The build must belong to this Joyeer checkout.'
}

$binaryDir = Join-Path $buildPath 'bin'
if ($cache['CMAKE_CONFIGURATION_TYPES']) {
    if ('Debug' -cnotin ($cache['CMAKE_CONFIGURATION_TYPES'] -split ';')) {
        throw 'The selected build does not support Debug.'
    }
    $binaryDir = Join-Path $buildPath 'Debug\bin'
} elseif ($cache['CMAKE_BUILD_TYPE'] -cne 'Debug') {
    throw 'Only Debug builds can be installed with this script.'
}

$trimCharacters = [char[]]'\/'
$installPath = $installPath.TrimEnd($trimCharacters)
$buildPath = $buildPath.TrimEnd($trimCharacters)
if ($installPath -eq $sourceDir -or $installPath -eq $buildPath -or
    $installPath.StartsWith($buildPath + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Choose an install directory outside the build tree and not the source root.'
}

$symbols = @('joyeer.pdb', 'joyeer-backend.pdb')
foreach ($name in @('joyeer.exe', 'joyeer-backend.dll', 'JoyeerNativeRuntime.lib') + $symbols) {
    $artifact = Join-Path $binaryDir $name
    if (-not (Test-Path -LiteralPath $artifact -PathType Leaf) -or
        (Get-Item -LiteralPath $artifact).Length -eq 0) {
        throw "Missing or empty Debug artifact: $artifact. Build Debug first."
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $buildPath 'cmake_install.cmake') -PathType Leaf)) {
    throw 'Missing cmake_install.cmake; reconfigure the build first.'
}

$skillFilesToCopy = @()
if (-not $SkipSkillInstall) {
    $skillSource = Join-Path $sourceDir 'skills\joyeer'
    $skillPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($SkillDir).TrimEnd($trimCharacters)
    if (-not (Test-Path -LiteralPath (Join-Path $skillSource 'SKILL.md') -PathType Leaf)) {
        throw "Missing Joyeer skill: $skillSource"
    }
    $emptySkillDestination = $true
    if (Test-Path -LiteralPath $skillPath) {
        if (-not (Test-Path -LiteralPath $skillPath -PathType Container)) {
            throw "Skill destination is not a directory: $skillPath"
        }
        $emptySkillDestination = @(Get-ChildItem -LiteralPath $skillPath -Force).Count -eq 0
    }
    foreach ($file in Get-ChildItem -LiteralPath $skillSource -Recurse -File -Force) {
        $relativePath = $file.FullName.Substring($skillSource.Length + 1)
        $destination = Join-Path $skillPath $relativePath
        if ((Test-Path -LiteralPath $destination -PathType Leaf) -and
            (Get-FileHash -LiteralPath $file.FullName).Hash -eq (Get-FileHash -LiteralPath $destination).Hash) {
            continue
        }
        if (-not $emptySkillDestination -and -not $Force) {
            throw "Existing Joyeer skill differs at '$relativePath' in $skillPath. Review and back up your changes before using -Force, choose -SkillDir, or use -SkipSkillInstall."
        }
        if (Test-Path -LiteralPath $destination -PathType Container) {
            throw "Skill file destination is a directory: $destination"
        }
        $parent = Split-Path -Parent $destination
        while ($parent) {
            if (Test-Path -LiteralPath $parent -PathType Leaf) {
                throw "Skill parent path is not a directory: $parent"
            }
            $parent = Split-Path -Parent $parent
        }
        $skillFilesToCopy += @{ Source = $file.FullName; Destination = $destination }
    }
}

$cmake = (Get-Command cmake -CommandType Application -ErrorAction Stop).Source
& $cmake --install $buildPath --config Debug --component JoyeerRuntime --prefix $installPath
if ($LASTEXITCODE -ne 0) {
    throw "CMake installation failed with exit code $LASTEXITCODE."
}
foreach ($name in @('joyeer.exe', 'joyeer-backend.dll', 'JoyeerNativeRuntime.lib', 'licenses\LICENSE')) {
    $artifact = Join-Path $installPath $name
    if (-not (Test-Path -LiteralPath $artifact -PathType Leaf) -or
        (Get-Item -LiteralPath $artifact).Length -eq 0) {
        throw "Missing installed artifact: $artifact. Reconfigure and rebuild this checkout."
    }
}
foreach ($name in $symbols) {
    Copy-Item -LiteralPath (Join-Path $binaryDir $name) -Destination (Join-Path $installPath $name) -Force
}

if (-not $SkipSkillInstall) {
    if ($skillFilesToCopy.Count -gt 0) {
        foreach ($file in $skillFilesToCopy) {
            New-Item -ItemType Directory -Path (Split-Path -Parent $file.Destination) -Force | Out-Null
            Copy-Item -LiteralPath $file.Source -Destination $file.Destination -Force:$Force
        }
        Write-Host "Joyeer skill installed to $skillPath"
    } else {
        Write-Host "Joyeer skill already matches this checkout: $skillPath"
    }
    Write-Host 'Reload your agent session to discover the Joyeer skill.'
}

if (-not $SkipPathUpdate) {
    $userPath = [string][Environment]::GetEnvironmentVariable('Path', 'User')
    $machinePath = [string][Environment]::GetEnvironmentVariable('Path', 'Machine')
    $updatedUserPath = Add-InstallPath -PathValue $userPath -Entry $installPath -AdditionalPath $machinePath
    if ($updatedUserPath -cne $userPath) {
        [Environment]::SetEnvironmentVariable('Path', $updatedUserPath, 'User')
    }
    $env:PATH = Add-InstallPath -PathValue $env:PATH -Entry $installPath
    Write-Host 'Install directory is on PATH. Restart other terminals or IDEs to pick up user PATH changes.'
}

Write-Host "Debug compiler installed to $installPath"
Write-Host 'This installation is for local testing only.'