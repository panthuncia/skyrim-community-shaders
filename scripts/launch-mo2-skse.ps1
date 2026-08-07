param(
    [string]$Mo2Directory = 'C:\Modding\MO2',
    [string]$Profile = 'Default',
    [string]$ExecutableTitle = 'SKSE',
    [string]$GameProcessName = 'SkyrimSE',
    [string]$LogDirectory = (Join-Path $PSScriptRoot '..\build\game-runs'),
    [int]$StartupTimeoutSeconds = 60,
    [int]$RunTimeoutSeconds = 300,
    [switch]$LeaveRunning
)

$ErrorActionPreference = 'Stop'

function Get-SkyrimDocumentsDirectory {
    $candidates = @(
        (Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'My Games\Skyrim Special Edition'),
        (Join-Path $env:USERPROFILE 'OneDrive\Documents\My Games\Skyrim Special Edition'),
        (Join-Path $env:USERPROFILE 'Documents\My Games\Skyrim Special Edition')
    ) | Select-Object -Unique

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Container) {
            return $candidate
        }
    }
    return $candidates[0]
}

function Copy-NewRunLogs([datetime]$StartedAt, [string]$Destination) {
    $roots = @(
        (Join-Path (Get-SkyrimDocumentsDirectory) 'SKSE'),
        (Join-Path $Mo2Directory 'crashDumps'),
        (Join-Path $Mo2Directory 'overwrite'),
        (Join-Path $Mo2Directory 'logs')
    )

    foreach ($root in $roots) {
        if (-not (Test-Path -LiteralPath $root -PathType Container)) { continue }
        $rootName = ($root -replace '[:\\/ ]', '_').Trim('_')
        $targetRoot = Join-Path $Destination $rootName
        foreach ($file in Get-ChildItem -LiteralPath $root -File -Recurse -ErrorAction SilentlyContinue) {
            if ($file.LastWriteTime -lt $StartedAt.AddSeconds(-2)) { continue }
            $relative = $file.FullName.Substring($root.Length).TrimStart('\')
            $target = Join-Path $targetRoot $relative
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
            Copy-Item -LiteralPath $file.FullName -Destination $target -Force
        }
    }
}

$mo2 = Join-Path $Mo2Directory 'ModOrganizer.exe'
if (-not (Test-Path -LiteralPath $mo2 -PathType Leaf)) {
    throw "ModOrganizer.exe was not found at '$mo2'."
}
if (Get-Process -Name $GameProcessName -ErrorAction SilentlyContinue) {
    throw "$GameProcessName is already running; refusing to confuse two run captures."
}

$startedAt = Get-Date
$runName = $startedAt.ToString('yyyyMMdd-HHmmss')
$runDirectory = Join-Path $LogDirectory $runName
New-Item -ItemType Directory -Force -Path $runDirectory | Out-Null
$summaryPath = Join-Path $runDirectory 'run-summary.txt'

@(
    "started=$($startedAt.ToString('o'))",
    "mo2=$mo2",
    "profile=$Profile",
    "executable=$ExecutableTitle"
) | Set-Content -LiteralPath $summaryPath -Encoding UTF8

Write-Host "Launching '$ExecutableTitle' through MO2 profile '$Profile'..."
$mo2Process = Start-Process -FilePath $mo2 -ArgumentList @('-p', $Profile, 'run', '-e', $ExecutableTitle) -PassThru

$deadline = $startedAt.AddSeconds($StartupTimeoutSeconds)
$game = $null
do {
    Start-Sleep -Milliseconds 250
    $game = Get-Process -Name $GameProcessName -ErrorAction SilentlyContinue |
        Where-Object { $_.StartTime -ge $startedAt.AddSeconds(-2) } |
        Sort-Object StartTime -Descending |
        Select-Object -First 1
} while (-not $game -and (Get-Date) -lt $deadline)

if (-not $game) {
    "result=startup-timeout" | Add-Content -LiteralPath $summaryPath
    Copy-NewRunLogs $startedAt $runDirectory
    throw "$GameProcessName did not start within $StartupTimeoutSeconds seconds. Logs: $runDirectory"
}

"game_pid=$($game.Id)" | Add-Content -LiteralPath $summaryPath
Write-Host "$GameProcessName started as PID $($game.Id)."
# Force Process to retain an OS handle so ExitCode remains available after exit.
$null = $game.Handle

if ($LeaveRunning) {
    "result=left-running" | Add-Content -LiteralPath $summaryPath
    Write-Host "Leaving the game running. Run capture: $runDirectory"
    exit 0
}

$exited = $game.WaitForExit($RunTimeoutSeconds * 1000)
if (-not $exited) {
    "result=still-running-after-timeout" | Add-Content -LiteralPath $summaryPath
    Copy-NewRunLogs $startedAt $runDirectory
    Write-Host "The game remained running for $RunTimeoutSeconds seconds. It was not terminated."
    Write-Host "Run capture: $runDirectory"
    exit 0
}

$endedAt = Get-Date
$exitCode = $game.ExitCode
$exitCodeText = if ($null -ne $exitCode) { [string]$exitCode } else { 'unavailable' }
$exitCodeHex = if ($null -ne $exitCode) { "0x$($exitCode.ToString('X8'))" } else { 'unavailable' }
@(
    "ended=$($endedAt.ToString('o'))",
    "duration_seconds=$([math]::Round(($endedAt - $startedAt).TotalSeconds, 3))",
    "exit_code=$exitCodeText",
    "exit_code_hex=$exitCodeHex",
    "result=process-exited"
) | Add-Content -LiteralPath $summaryPath

Copy-NewRunLogs $startedAt $runDirectory

# Application Error and WER records are committed asynchronously after process exit.
Start-Sleep -Seconds 3
$events = Get-WinEvent -FilterHashtable @{ LogName = 'Application'; StartTime = $startedAt.AddSeconds(-2) } -ErrorAction SilentlyContinue |
    Where-Object { $_.ProviderName -in @('Application Error', 'Windows Error Reporting') -and $_.Message -match 'SkyrimSE\.exe' } |
    Sort-Object TimeCreated
if ($events) {
    $events | Format-List TimeCreated, ProviderName, Id, LevelDisplayName, Message |
        Out-File -LiteralPath (Join-Path $runDirectory 'windows-events.txt') -Encoding UTF8 -Width 4096
}

Write-Host "Game exited after $([math]::Round(($endedAt - $startedAt).TotalSeconds, 1)) seconds with code $exitCodeHex."
Write-Host "Run capture: $runDirectory"
