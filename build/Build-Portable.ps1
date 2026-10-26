[CmdletBinding()]
param(
    [string]$FileeSource = (Join-Path $PSScriptRoot '..\..\..\Filee'),
    [string]$Preset = 'windows-2026-release',
    [string[]]$Engines = @('rhwp', '7zip', 'ffmpeg'),
    [switch]$SkipNativeBuild
)

$ErrorActionPreference = 'Stop'
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$FileeSource = (Resolve-Path $FileeSource).Path
$output = Join-Path $root 'out\portable'
$engineOutput = Join-Path $output 'conversion'
$patch = Join-Path $root 'conversion\Filee-tiff-depth.patch'
& git -C $FileeSource apply --reverse --check $patch 2>$null
if ($LASTEXITCODE -ne 0) {
    & git -C $FileeSource apply --check $patch
    if ($LASTEXITCODE -ne 0) { throw 'The Filee TIFF-depth correction does not apply cleanly; preserve local edits and reconcile it first.' }
    & git -C $FileeSource apply $patch
    if ($LASTEXITCODE -ne 0) { throw 'The Filee TIFF-depth correction could not be applied.' }
}

if (-not $SkipNativeBuild) {
    Push-Location $root
    try {
        & cmake --preset $Preset
        if ($LASTEXITCODE -ne 0) { throw 'Native CMake configuration failed.' }
        & cmake --build --preset $Preset --parallel 4 -- /verbosity:minimal
        if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }
        & ctest --preset $Preset --output-on-failure -j 2
        if ($LASTEXITCODE -ne 0) { throw 'Native regression tests failed.' }
    } finally { Pop-Location }
}

& dotnet publish (Join-Path $root 'conversion\WeaponCycle.Conversion.csproj') `
    -c Release -r win-x64 --self-contained true `
    "-p:FileeSource=$FileeSource" -o $engineOutput --nologo -v minimal
if ($LASTEXITCODE -ne 0) { throw 'Conversion engine publish failed.' }

$helper = Join-Path $engineOutput 'WeaponCycle.Conversion.exe'
foreach ($engine in $Engines) {
    if ($engine -notin @('rhwp', '7zip', 'ffmpeg', 'pandoc', 'ghostscript', 'libreoffice', 'calibre', 'tesseract')) {
        throw "Unsupported engine package: $engine"
    }
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = $helper
    $start.Arguments = "install $engine"
    $start.UseShellExecute = $false
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = [Diagnostics.Process]::Start($start)
    try {
        $errors = $process.StandardError.ReadToEndAsync()
        while (($line = $process.StandardOutput.ReadLine()) -ne $null) {
            Write-Host $line
        }
        $process.WaitForExit()
        if ($process.ExitCode -ne 0) { throw "Preparing $engine failed: $($errors.Result)" }
    } finally { $process.Dispose() }
}

$cache = Join-Path $env:LOCALAPPDATA 'OrbitDeck\conversion\engines'
$components = @($Engines + @('vcruntime'))
if ($Engines -contains 'libreoffice') { $components += @('jre') }
foreach ($component in ($components | Select-Object -Unique)) {
    $source = Join-Path $cache $component
    if (Test-Path -LiteralPath $source) {
        $destination = Join-Path $engineOutput "engines\$component"
        New-Item -ItemType Directory -Path $destination -Force | Out-Null
        Get-ChildItem -LiteralPath $source -Force | Copy-Item -Destination $destination -Recurse -Force
    }
}
$runtime = Join-Path $cache 'vcruntime'
if (Test-Path -LiteralPath $runtime) {
    Get-ChildItem -LiteralPath $runtime -Filter '*.dll' | Copy-Item -Destination $engineOutput -Force
    $rhwp = Join-Path $engineOutput 'engines\rhwp'
    if (Test-Path -LiteralPath $rhwp) {
        Get-ChildItem -LiteralPath $runtime -Filter '*.dll' | Copy-Item -Destination $rhwp -Force
    }
}

Copy-Item -LiteralPath (Join-Path $FileeSource 'LICENSE') `
    -Destination (Join-Path $engineOutput 'LICENSE-Filee.txt') -Force
Copy-Item -LiteralPath (Join-Path $FileeSource 'THIRD-PARTY-NOTICES.md') `
    -Destination (Join-Path $engineOutput 'THIRD-PARTY-NOTICES-Filee.md') -Force
Copy-Item -LiteralPath $patch -Destination (Join-Path $engineOutput 'Filee-tiff-depth.patch') -Force
$revision = & git -C $FileeSource rev-parse HEAD
if ($LASTEXITCODE -ne 0) { throw 'Cannot record the conversion-engine source revision.' }
[IO.File]::WriteAllText((Join-Path $engineOutput 'SOURCE.txt'),
    "Filee.Core and Filee.Engines: https://github.com/KnifeLemon/Filee`r`nBase revision: $revision`r`nLocal correction: use 8-bit TIFF samples rather than PNG palette index bit depth (ImageEncoder.cs).`r`n")

$assets = Get-Content -LiteralPath (Join-Path $root 'conversion\obj\project.assets.json') -Raw | ConvertFrom-Json
foreach ($library in $assets.libraries.PSObject.Properties) {
    if ($library.Value.type -ne 'package') { continue }
    foreach ($folder in $assets.packageFolders.PSObject.Properties.Name) {
        $package = Join-Path $folder $library.Value.path
        if (-not (Test-Path -LiteralPath $package)) { continue }
        $notices = @(Get-ChildItem -LiteralPath $package -File |
            Where-Object { $_.Name -match '^(LICENSE|LICENCE|NOTICE|COPYING)' -or $_.Extension -eq '.nuspec' })
        if ($notices.Count -gt 0) {
            $destination = Join-Path $engineOutput ('licenses\' + ($library.Name -replace '/', '-'))
            New-Item -ItemType Directory -Path $destination -Force | Out-Null
            $notices | Copy-Item -Destination $destination -Force
        }
    }
}

$native = Join-Path $root "out\build\$Preset\Release\OrbitDeck.exe"
Copy-Item -LiteralPath $native -Destination (Join-Path $output 'WeaponCycle.exe') -Force
Copy-Item -LiteralPath (Join-Path $root 'README.md') -Destination (Join-Path $output 'README.md') -Force
Copy-Item -LiteralPath (Join-Path $root 'CONTRIBUTING.md') -Destination (Join-Path $output 'CONTRIBUTING.md') -Force
Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination (Join-Path $output 'LICENSE') -Force
Copy-Item -LiteralPath (Join-Path $root 'THIRD_PARTY_NOTICES.md') `
    -Destination (Join-Path $output 'THIRD_PARTY_NOTICES.md') -Force
if ((Get-FileHash -LiteralPath $native -Algorithm SHA256).Hash -ne
    (Get-FileHash -LiteralPath (Join-Path $output 'WeaponCycle.exe') -Algorithm SHA256).Hash) {
    throw 'The portable executable does not match the built release.'
}
Write-Host "Ready: $output (distribute this complete folder, not WeaponCycle.exe alone)"
