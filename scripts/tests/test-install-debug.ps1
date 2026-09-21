#requires -Version 5.1

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDir
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$sourceDir = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$installer = Join-Path $sourceDir 'scripts\install-debug.ps1'
$buildPath = (Resolve-Path -LiteralPath $BuildDir).ProviderPath
$testDir = Join-Path ([IO.Path]::GetTempPath()) ('joyeer debug tests ' + [guid]::NewGuid().ToString('N'))
$skillDir = Join-Path $testDir 'agent skills\joyeer'
$originalPath = $env:PATH
$originalUserPath = [Environment]::GetEnvironmentVariable('Path', 'User')
$originalMachinePath = [Environment]::GetEnvironmentVariable('Path', 'Machine')

function Assert-Rejected {
    param([scriptblock]$Action, [string]$ExpectedMessage)

    try {
        & $Action | Out-Null
    } catch {
        if ($_.Exception.Message -like "*$ExpectedMessage*") {
            return
        }
        throw
    }
    throw "Expected rejection: $ExpectedMessage"
}

function Write-TestCache {
    param([hashtable]$Entries)

    $lines = foreach ($key in $Entries.Keys) {
        '{0}:STRING={1}' -f $key, $Entries[$key]
    }
    [IO.File]::WriteAllLines((Join-Path $fakeBuild 'CMakeCache.txt'), [string[]]$lines)
}

try {
    $fakeBuild = Join-Path $testDir 'fake build'
    $rejectedInstall = Join-Path $testDir 'must not exist'
    New-Item -ItemType Directory -Path $fakeBuild | Out-Null
    Assert-Rejected { & $installer -BuildDir $fakeBuild -InstallDir $rejectedInstall -SkillDir $skillDir -SkipPathUpdate } 'No CMakeCache.txt'

    $cache = @{
        CMAKE_PROJECT_NAME = 'joyeer'
        CMAKE_HOME_DIRECTORY = $sourceDir
        CMAKE_BUILD_TYPE = 'Release'
    }
    Write-TestCache $cache
    Assert-Rejected { & $installer -BuildDir $fakeBuild -InstallDir $rejectedInstall -SkillDir $skillDir -SkipPathUpdate } 'Only Debug builds'

    $cache.CMAKE_BUILD_TYPE = 'Debug'
    $cache.CMAKE_PROJECT_NAME = 'another-project'
    Write-TestCache $cache
    Assert-Rejected { & $installer -BuildDir $fakeBuild -InstallDir $rejectedInstall -SkillDir $skillDir -SkipPathUpdate } 'this Joyeer checkout'

    $cache.CMAKE_PROJECT_NAME = 'joyeer'
    $cache.CMAKE_HOME_DIRECTORY = $testDir
    Write-TestCache $cache
    Assert-Rejected { & $installer -BuildDir $fakeBuild -InstallDir $rejectedInstall -SkillDir $skillDir -SkipPathUpdate } 'this Joyeer checkout'

    $cache.CMAKE_HOME_DIRECTORY = $sourceDir
    $cache.CMAKE_CONFIGURATION_TYPES = 'Release;RelWithDebInfo'
    Write-TestCache $cache
    Assert-Rejected { & $installer -BuildDir $fakeBuild -InstallDir $rejectedInstall -SkillDir $skillDir -SkipPathUpdate } 'does not support Debug'

    $cache.Remove('CMAKE_CONFIGURATION_TYPES')
    Write-TestCache $cache
    Assert-Rejected { & $installer -BuildDir $fakeBuild -InstallDir $sourceDir -SkillDir $skillDir -SkipPathUpdate } 'Choose an install directory'
    Assert-Rejected { & $installer -BuildDir $fakeBuild -InstallDir $fakeBuild -SkillDir $skillDir -SkipPathUpdate } 'Choose an install directory'
    Assert-Rejected { & $installer -BuildDir $fakeBuild -InstallDir (Join-Path $fakeBuild 'bin') -SkillDir $skillDir -SkipPathUpdate } 'Choose an install directory'
    Assert-Rejected { & $installer -BuildDir $fakeBuild -InstallDir $rejectedInstall -SkillDir $skillDir -SkipPathUpdate } 'Missing or empty Debug artifact'
    if (Test-Path -LiteralPath $rejectedInstall) {
        throw 'A rejected install created its destination.'
    }

    $installDir = Join-Path $testDir 'installed compiler'
    & {
        . $installer -BuildDir $buildPath -InstallDir $installDir -SkillDir $skillDir -SkipPathUpdate
        foreach ($architectureCase in @(
            @{ Architecture = 'ARM64'; Preset = 'arm64-debug' },
            @{ Architecture = 'AMD64'; Preset = 'x64-debug' }
        )) {
            $selectedBuild = Get-DefaultDebugBuildDir -SourceDirectory $sourceDir -NativeArchitecture $architectureCase.Architecture
            if ($selectedBuild -ne (Join-Path $sourceDir ('out\build\' + $architectureCase.Preset))) {
                throw "Incorrect default build for $($architectureCase.Architecture)."
            }
        }
        Assert-Rejected { Get-DefaultDebugBuildDir -SourceDirectory $sourceDir -NativeArchitecture 'x86' } 'Unsupported Windows architecture'
        Assert-Rejected { Get-DefaultDebugBuildDir -SourceDirectory $sourceDir -NativeArchitecture '' } 'Unsupported Windows architecture'
        Assert-Rejected { & $installer -BuildDir (Join-Path $testDir 'missing build') -InstallDir $rejectedInstall -SkillDir $skillDir -SkipPathUpdate } 'Debug build directory not found'
        $nativeBuild = Get-DefaultDebugBuildDir -SourceDirectory $sourceDir
        $automaticInstall = Join-Path $testDir 'automatic install'
        $skippedSkillDir = Join-Path $testDir 'skipped skill'
        & $installer -InstallDir $automaticInstall -SkillDir $skippedSkillDir -SkipSkillInstall -SkipPathUpdate
        if (Test-Path -LiteralPath $skippedSkillDir) {
            throw 'Skipping skill installation created a directory.'
        }
        foreach ($name in @('joyeer.exe', 'joyeer-backend.dll', 'JoyeerNativeRuntime.lib', 'joyeer.pdb', 'joyeer-backend.pdb')) {
            if ((Get-FileHash -LiteralPath (Join-Path $automaticInstall $name)).Hash -ne
                (Get-FileHash -LiteralPath (Join-Path $nativeBuild ('bin\' + $name))).Hash) {
                throw "Automatic installation did not use the native Debug build: $name"
            }
        }
        $pathTests = @(
            @{ Path = ''; Expected = $installDir },
            @{ Path = $null; Expected = $installDir },
            @{ Path = 'C:\Other'; Expected = "C:\Other;$installDir" },
            @{ Path = 'C:\Other;'; Expected = "C:\Other;$installDir" },
            @{ Path = ';C:\Other;; '; Expected = ";C:\Other;; ;$installDir" },
            @{ Path = ";;$installDir;;"; Expected = ";;$installDir;;" },
            @{ Path = "C:\Other;$installDir"; Expected = "C:\Other;$installDir" },
            @{ Path = $installDir.ToUpperInvariant() + '\'; Expected = $installDir.ToUpperInvariant() + '\' },
            @{ Path = $installDir.Replace('\', '/'); Expected = $installDir.Replace('\', '/') },
            @{ Path = '"' + $installDir + '"'; Expected = '"' + $installDir + '"' },
            @{ Path = $installDir + '-other'; Expected = "$installDir-other;$installDir" }
        )
        foreach ($testCase in $pathTests) {
            $actual = Add-InstallPath -PathValue $testCase.Path -Entry $installDir
            if ($actual -cne $testCase.Expected) {
                throw "PATH update mismatch for: $($testCase.Path)"
            }
            if ((Add-InstallPath -PathValue $actual -Entry $installDir) -cne $actual) {
                throw 'Repeated PATH updates must be idempotent.'
            }
        }
        $variablePath = '%USERPROFILE%\.joyeer\bin'
        $expandedPath = [Environment]::ExpandEnvironmentVariables($variablePath)
        if ((Add-InstallPath -PathValue $variablePath -Entry $expandedPath) -cne $variablePath) {
            throw 'PATH comparison must expand environment variables.'
        }
        if ((Add-InstallPath -PathValue 'C:\Other' -Entry $installDir -AdditionalPath $installDir) -cne 'C:\Other') {
            throw 'An existing machine PATH entry must not be duplicated in user PATH.'
        }
    }
    $payload = @('joyeer.exe', 'joyeer-backend.dll', 'JoyeerNativeRuntime.lib',
        'joyeer.pdb', 'joyeer-backend.pdb', 'licenses\LICENSE')
    $installed = @(Get-ChildItem -LiteralPath $installDir -Recurse -File | ForEach-Object {
        $_.FullName.Substring($installDir.Length + 1)
    })
    if (Compare-Object $payload $installed) {
        throw 'The installation payload differs from the expected runtime and debug symbols.'
    }

    $sentinel = Join-Path $installDir 'unrelated.txt'
    [IO.File]::WriteAllText($sentinel, 'preserve me')
    $skillSentinel = Join-Path $skillDir 'user-notes.txt'
    [IO.File]::WriteAllText($skillSentinel, 'preserve skill notes')
    $otherSkill = Join-Path (Split-Path -Parent $skillDir) 'other-skill.txt'
    [IO.File]::WriteAllText($otherSkill, 'preserve other skills')
    & $installer -BuildDir $buildPath -InstallDir $installDir -SkillDir $skillDir -SkipPathUpdate
    if ([IO.File]::ReadAllText($skillSentinel) -ne 'preserve skill notes' -or
        [IO.File]::ReadAllText($otherSkill) -ne 'preserve other skills') {
        throw 'Reinstallation changed unrelated skill files.'
    }
    $skillSource = Join-Path $sourceDir 'skills\joyeer'
    $expectedSkillFiles = @(Get-ChildItem -LiteralPath $skillSource -Recurse -File -Force | ForEach-Object {
        $relativePath = $_.FullName.Substring($skillSource.Length + 1)
        if ((Get-FileHash -LiteralPath $_.FullName).Hash -ne
            (Get-FileHash -LiteralPath (Join-Path $skillDir $relativePath)).Hash) {
            throw "Installed skill does not match its source: $relativePath"
        }
        $relativePath
    })
    $actualSkillFiles = @(Get-ChildItem -LiteralPath $skillDir -Recurse -File -Force | ForEach-Object {
        $_.FullName.Substring($skillDir.Length + 1)
    })
    if (Compare-Object ($expectedSkillFiles + 'user-notes.txt') $actualSkillFiles) {
        throw 'Skill installation has missing, extra, or incorrectly nested files.'
    }
    $skillEntry = Join-Path $skillDir 'SKILL.md'
    [IO.File]::WriteAllText($skillEntry, 'customized skill')
    Assert-Rejected { & $installer -BuildDir $buildPath -InstallDir $rejectedInstall -SkillDir $skillDir -SkipPathUpdate } 'Existing Joyeer skill differs'
    if ([IO.File]::ReadAllText($skillEntry) -ne 'customized skill' -or
        (Test-Path -LiteralPath $rejectedInstall)) {
        throw 'A skill conflict changed files before rejection.'
    }
    & $installer -BuildDir $buildPath -InstallDir $installDir -SkillDir $skillDir -SkipSkillInstall -SkipPathUpdate
    if ([IO.File]::ReadAllText($skillEntry) -ne 'customized skill') {
        throw 'Skipping skill installation modified the existing skill.'
    }
    Remove-Item -LiteralPath $skillEntry
    Assert-Rejected { & $installer -BuildDir $buildPath -InstallDir $rejectedInstall -SkillDir $skillDir -SkipPathUpdate } 'Existing Joyeer skill differs'
    Assert-Rejected { & $installer -BuildDir $buildPath -InstallDir $rejectedInstall -SkillDir $skillSentinel -SkipPathUpdate } 'Skill destination is not a directory'
    if ((Test-Path -LiteralPath $rejectedInstall) -or (Test-Path -LiteralPath $skillEntry)) {
        throw 'Rejected skill installation changed the destination.'
    }
    $changedReference = Join-Path $skillDir 'references\cli.md'
    [IO.File]::WriteAllText($changedReference, 'old reference content')
    & $installer -BuildDir $buildPath -InstallDir $installDir -SkillDir $skillDir -Force -SkipSkillInstall -SkipPathUpdate
    if ((Test-Path -LiteralPath $skillEntry) -or
        [IO.File]::ReadAllText($changedReference) -ne 'old reference content') {
        throw '-SkipSkillInstall must take precedence over -Force.'
    }
    & $installer -BuildDir $buildPath -InstallDir $installDir -SkillDir $skillDir -Force -SkipPathUpdate
    foreach ($relativePath in $expectedSkillFiles) {
        if ((Get-FileHash -LiteralPath (Join-Path $skillSource $relativePath)).Hash -ne
            (Get-FileHash -LiteralPath (Join-Path $skillDir $relativePath)).Hash) {
            throw "Forced skill update did not restore source content: $relativePath"
        }
    }
    if ([IO.File]::ReadAllText($skillSentinel) -ne 'preserve skill notes' -or
        [IO.File]::ReadAllText($otherSkill) -ne 'preserve other skills') {
        throw 'Forced skill update modified unrelated files.'
    }
    $emptySkillDir = Join-Path $testDir 'empty skill\joyeer'
    New-Item -ItemType Directory -Path $emptySkillDir | Out-Null
    & $installer -BuildDir $buildPath -InstallDir $installDir -SkillDir $emptySkillDir -SkipPathUpdate
    $emptyInstallFiles = @(Get-ChildItem -LiteralPath $emptySkillDir -Recurse -File -Force | ForEach-Object {
        $_.FullName.Substring($emptySkillDir.Length + 1)
    })
    if (Compare-Object $expectedSkillFiles $emptyInstallFiles) {
        throw 'Installing into an empty directory added missing, extra, or nested skill files.'
    }
    foreach ($relativePath in $expectedSkillFiles) {
        if ((Get-FileHash -LiteralPath (Join-Path $skillSource $relativePath)).Hash -ne
            (Get-FileHash -LiteralPath (Join-Path $emptySkillDir $relativePath)).Hash) {
            throw "Empty-directory installation changed skill content: $relativePath"
        }
    }
    $collisionSkillDir = Join-Path $testDir 'skill collisions\joyeer'
    $blockedFile = Join-Path $collisionSkillDir 'SKILL.md'
    New-Item -ItemType Directory -Path $blockedFile | Out-Null
    Assert-Rejected {
        & $installer -BuildDir $buildPath -InstallDir $rejectedInstall -SkillDir $collisionSkillDir -Force -SkipPathUpdate
    } 'Skill file destination is a directory'
    if (-not (Test-Path -LiteralPath $blockedFile -PathType Container) -or
        @(Get-ChildItem -LiteralPath $blockedFile -Force).Count -ne 0) {
        throw 'A forced update changed a directory conflicting with a skill file.'
    }
    Remove-Item -LiteralPath $blockedFile
    $blockedParent = Join-Path $collisionSkillDir 'references'
    [IO.File]::WriteAllText($blockedParent, 'preserve conflicting file')
    Assert-Rejected {
        & $installer -BuildDir $buildPath -InstallDir $rejectedInstall -SkillDir $collisionSkillDir -Force -SkipPathUpdate
    } 'Skill parent path is not a directory'
    if ([IO.File]::ReadAllText($blockedParent) -ne 'preserve conflicting file' -or
        (Test-Path -LiteralPath $rejectedInstall) -or (Test-Path -LiteralPath $blockedFile)) {
        throw 'A forced update with a parent path conflict changed files before rejection.'
    }
    if ([IO.File]::ReadAllText($sentinel) -ne 'preserve me') {
        throw 'Reinstallation modified an unrelated file.'
    }
    $binaryDir = Join-Path $buildPath 'bin'
    if (-not (Test-Path -LiteralPath (Join-Path $binaryDir 'joyeer.exe'))) {
        $binaryDir = Join-Path $buildPath 'Debug\bin'
    }
    foreach ($name in $payload) {
        $original = Join-Path $binaryDir $name
        if ($name -eq 'licenses\LICENSE') {
            $original = Join-Path $sourceDir 'LICENSE'
        }
        if ((Get-FileHash -LiteralPath $original).Hash -ne
            (Get-FileHash -LiteralPath (Join-Path $installDir $name)).Hash) {
            throw "Installed file does not match build: $name"
        }
    }

    $compiler = Join-Path $installDir 'joyeer.exe'
    $program = Join-Path $testDir 'hello.exe'
    & $compiler -O0 -gfull -gcodeview -o $program (Join-Path $sourceDir 'tests\native\hello.joyeer')
    if ($LASTEXITCODE -ne 0) { throw 'Installed compiler failed to compile the native fixture.' }
    $output = & $program
    if ($LASTEXITCODE -ne 0 -or ($output -join "`n") -ne "42`nnative`n8`n42") {
        throw 'Native fixture output did not match.'
    }
    if (-not (Test-Path -LiteralPath (Join-Path $testDir 'hello.pdb') -PathType Leaf)) {
        throw 'The installed compiler did not produce debug symbols.'
    }
    if ($env:PATH -cne $originalPath -or
        [Environment]::GetEnvironmentVariable('Path', 'User') -cne $originalUserPath -or
        [Environment]::GetEnvironmentVariable('Path', 'Machine') -cne $originalMachinePath) {
        throw 'The installer modified PATH.'
    }
    Write-Output 'PASS: Native architecture selection, Debug and skill installation, forced updates, empty destinations, conflict protection, symbols, reinstallation, native execution, invalid builds, destination guards and PATH deduplication.'
} finally {
    if (Test-Path -LiteralPath $testDir) {
        Remove-Item -LiteralPath $testDir -Recurse -Force
    }
}