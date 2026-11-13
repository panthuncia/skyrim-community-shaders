param([string]$Configs = 'rx6600-sweep', [string]$Stage = 'p35', [string]$Label = 'sweep', [int]$Seconds = 60, [string]$Set = "")
# Camera sweep: runs a cs-test config and, once its world is ready, moves the mouse side to side (about 120 units every
# 16 ms, reversing each second) for $Seconds to provoke GPU device losses. The config's own exit step ends the run.
# -Set takes --set values separated by ";".
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$status = "$env:USERPROFILE\Documents\My Games\Skyrim Special Edition\SKSE\CommunityShaders-Test.status.json"
$sets = @(); foreach ($s in ($Set -split ";" | Where-Object { $_ })) { $sets += @('--set', $s) }
$job = Start-Job -ScriptBlock {
    param($root, $configs, $stage, $label, $sets)
    Set-Location $root
    & python tools/cs-test/cs_test.py run $configs --stage $stage --label $label @sets 2>&1
} -ArgumentList $root, $Configs, $Stage, $Label, $sets
Add-Type -Namespace Sweep -Name Native -MemberDefinition '[DllImport("user32.dll")] public static extern void mouse_event(uint f, int x, int y, uint d, UIntPtr e);'
$start = Get-Date
$ready = $false
while ($job.State -eq 'Running' -and -not $ready) {
    Start-Sleep -Seconds 2
    try {
        if ((Get-Item $status -ErrorAction Stop).LastWriteTime -gt $start) {
            $json = Get-Content $status -Raw | ConvertFrom-Json
            $ready = $null -ne $json.milestones.'world-ready'
        }
    } catch {}
}
if ($ready) {
    Start-Sleep -Seconds 3
    $sweepStart = Get-Date; $dir = 1; $steps = 0
    while (((Get-Date) - $sweepStart).TotalSeconds -lt $Seconds -and $job.State -eq 'Running' -and (Get-Process SkyrimSE -ErrorAction SilentlyContinue)) {
        [Sweep.Native]::mouse_event(1, 120 * $dir, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 16
        if (++$steps % 60 -eq 0) { $dir = -$dir }
    }
    "swept $([int]((Get-Date) - $sweepStart).TotalSeconds) s"
} else { "the world never became ready; no sweep" }
Receive-Job $job -Wait | Select-Object -Last 6
Remove-Job $job
