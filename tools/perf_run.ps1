#Requires -Version 7.0
<#
.SYNOPSIS
  Controlled dkb.exe performance run that writes an evidence bundle under build\agent-perf\<stamp>-<visit>\.
  One-line commands per run type: tools\perf_run.README.md.

.PARAMETER Visit
  first:  a fresh, empty profile and an empty shader cache (the first visit to a build).
  repeat: a copy of a completed first-visit bundle's profile and shader cache (-From).

.PARAMETER From
  repeat only: the completed first-visit bundle folder to reuse.

.PARAMETER Exe
  dkb.exe to run, relative to the repo root (default build\dkb100\dkb.exe, where docs/dkb-testing.md builds it). Its folder's runtime files (exe, DLLs, *.bin, dkb.ini or totk.ini) are copied into
  the bundle's bin\ folder and the copy runs, so the game's logs land in bin\logs\dkb.log (bin\logs\totk.log for a TotK build) and the source folder is
  not written to.

.PARAMETER Exefs
  The extracted ExeFS folder (main, sdk), relative to the repo root or absolute. The default is where
  `tools\extract_nup.py <game>.nup --out build\extracted\dkb100-dump` writes it (docs/dkb-testing.md).

.PARAMETER Romfs
  The packed RomFS folder (it holds romfs.bin and index.json), relative to the repo root or absolute
  (default build\extracted\dkb100\romfs).

.PARAMETER Seconds
  Wall-clock length of the run before the game window is closed (default 180).

.PARAMETER WarmAfter
  Seconds after the first present that the summary counts as warmed gameplay (default 60). Assumption, see README.

.PARAMETER Measure
  flight (default): the flight recorder gives a per-frame record (cfg.c files it under "diagnostics"), so this run
            is NOT diagnostics-off.
  summary:  diagnostics off. No flight recorder; the always-on frametime summaries in the log (every 60 s, and at
            exit) give mean, 1% low, counts over 25/50/100 ms and the worst five frames of each period.

.PARAMETER Script
  walk (default): the menu presses that reach the cave scene, then walk and camera cycles until the run ends.
  idle: the menu presses only (the earlier harness script, kept for comparison).

.PARAMETER Realtime
  Value of TOTK_REALTIME. Unset by default, so the run uses the game's own default (runtime/cfg.c). Pass 0 or 1 to force it;
  builds without the realtime patch set ignore it (the log says so).

.PARAMETER Thumbs
  Verification only: flight thumbnails (320x180) of the frames around each dump, to look at the scene. The thumbnail
  ring costs GPU time and frame time, so a Thumbs run is not a performance run.

.PARAMETER DryRun
  Check the inputs, the process guard and the bundle, write run.json, then stop before the game starts.
#>
[CmdletBinding()]
param(
    [ValidateSet('first', 'repeat')] [string]$Visit = 'first',
    [string]$From = '',
    [string]$Exe = 'build\dkb100\dkb.exe',
    [string]$Exefs = 'build\extracted\dkb100-dump\exefs',
    [string]$Romfs = 'build\extracted\dkb100\romfs',
    [int]$Seconds = 180,
    [int]$SampleSeconds = 5,
    [int]$WarmAfter = 60,
    [ValidateSet('flight', 'summary')] [string]$Measure = 'flight',
    [ValidateSet('walk', 'idle')] [string]$Script = 'walk',
    [string]$Realtime = '',
    [string]$ExtraEnv = '',   # NAME=VALUE pairs separated by ';' (an array does not survive pwsh -File quoting)
    [switch]$Thumbs,
    [string]$Label = '',
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

function Full([string]$p) { if ([IO.Path]::IsPathRooted($p)) { $p } else { Join-Path $repo $p } }
function Say([string]$m) { Write-Host ("[perf_run {0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m) }
# Numbers in the samples CSV must not depend on the user's culture (pt-BR writes "1911,0" and splits the row).
function Inv([string]$format, [object[]]$values) { [string]::Format([Globalization.CultureInfo]::InvariantCulture, $format, $values) }
function Write-Json([object]$obj, [string]$path) { $obj | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $path -Encoding utf8 }

# The menu presses that reached the cave scene in build\native-port-integrate\menu-to-scene.txt (60 Hz ticks).
$menuLines = @('300 A 4', '520 A 4', '700 A 4', '900 A 4', '1100 A 4', '1300 A 4', '1500 A 4')

# Walk script: after the menu, a 600-tick cycle from tick 3000 on. Each cycle walks forward 5 s, walks diagonally
# 2 s, turns the camera right 1.5 s, and walks back 1.5 s. Stick values are +-32767 (runtime/input.c STICK_MAX).
# Sticks are held in ticks, so the same script is the same input at any frame rate.
function New-Script([string]$kind, [int]$ticks) {
    $lines = @("# tools/perf_run.ps1 -Script ${kind}: menu presses (ticks since the first present), then walk and camera cycles")
    $lines += $menuLines
    if ($kind -eq 'walk') {
        for ($c = 3000; $c -lt $ticks; $c += 600) {
            $lines += "$c LSTICK 0 32767 300"
            $lines += "$($c + 300) LSTICK 16384 32767 120"
            $lines += "$($c + 420) RSTICK 32767 0 90"
            $lines += "$($c + 510) LSTICK 0 -32767 90"
        }
    }
    return $lines
}

# ---------------------------------------------------------------- 1. guard: nothing else of ours may be running
$others = @(Get-Process -Name 'dkb' -ErrorAction SilentlyContinue)
if ($others.Count -gt 0) {
    throw ("dkb.exe is already running (PID {0}, started {1}). Not launching and not stopping it. Run again after it exits." -f
        ($others.Id -join ', '), (($others | ForEach-Object { $_.StartTime }) -join ', '))
}

# ---------------------------------------------------------------- 2. inputs
$exeSrc = Full $Exe
$srcFolder = Split-Path -Parent $exeSrc
$exefs = Full $Exefs
$romfs = Full $Romfs
foreach ($req in @($exeSrc, (Join-Path $exefs 'main'), (Join-Path $exefs 'sdk'), (Join-Path $romfs 'romfs.bin'), (Join-Path $romfs 'index.json'))) {
    if (-not (Test-Path -LiteralPath $req)) { throw "Missing input: $req (pass -Exe, -Exefs and -Romfs for your build)" }
}

$fromFull = ''
if ($Visit -eq 'repeat') {
    if (-not $From) { throw '-Visit repeat needs -From <completed first-visit bundle>' }
    $fromFull = Full $From
    $markerPath = Join-Path $fromFull 'visit.json'
    if (-not (Test-Path -LiteralPath $markerPath)) { throw "Not a first-visit bundle (no visit.json): $fromFull" }
    $marker = Get-Content -LiteralPath $markerPath -Raw | ConvertFrom-Json
    if ($marker.visit -ne 'first' -or -not $marker.complete) { throw "$fromFull is not a completed first-visit run (visit=$($marker.visit), complete=$($marker.complete))" }
    foreach ($d in 'profile', 'shader-cache') { if (-not (Test-Path -LiteralPath (Join-Path $fromFull $d))) { throw "missing $d in $fromFull" } }
}

# ---------------------------------------------------------------- 3. bundle, isolated profile and cache, runtime copy
$bundle = Join-Path $repo ('build\agent-perf\{0}-{1}' -f (Get-Date -Format 'yyyyMMdd-HHmmss'), $Visit)
New-Item -ItemType Directory -Path $bundle | Out-Null
$gameProfile = Join-Path $bundle 'profile'
$cache = Join-Path $bundle 'shader-cache'
$flightDir = Join-Path $bundle 'flight'
$binDir = Join-Path $bundle 'bin'
if ($Visit -eq 'first') {
    New-Item -ItemType Directory -Path $gameProfile, $cache | Out-Null
} else {
    Copy-Item -LiteralPath (Join-Path $fromFull 'profile') -Destination $gameProfile -Recurse
    Copy-Item -LiteralPath (Join-Path $fromFull 'shader-cache') -Destination $cache -Recurse
}

# The runtime copy. The game writes its log beside the exe unless its stderr is a disk file (runtime/main.c log_open),
# and the harness used to redirect stderr to a file, which turned the log off. Running a copy keeps stderr on the
# console and the log in bin\logs\dkb.log, and leaves the source folder (its logs, caches, dkb.ini) untouched.
New-Item -ItemType Directory -Path $binDir | Out-Null
$runtimeFiles = Get-ChildItem -LiteralPath $srcFolder -File | Where-Object { $_.Name -eq 'dkb.exe' -or $_.Extension -in '.dll', '.bin' -or $_.Name -in 'dkb.ini', 'totk.ini' }
foreach ($f in $runtimeFiles) { Copy-Item -LiteralPath $f.FullName -Destination $binDir }
$exePath = Join-Path $binDir 'dkb.exe'
$exeFolder = $binDir
$copied = @(Get-ChildItem -LiteralPath $binDir -File | ForEach-Object { [ordered]@{ name = $_.Name; bytes = $_.Length } })

# ---------------------------------------------------------------- 4. the input script
$ticks = $Seconds * 60 + 600
$inputPath = Join-Path $bundle "input-$Script.txt"
New-Script $Script $ticks | Set-Content -LiteralPath $inputPath -Encoding ascii

# ---------------------------------------------------------------- 5. settings: everything from the environment is cleared, then ours are set
$settings = [ordered]@{
    TOTK_ASSUME_FOCUS       = '1'                  # keep the game running when the window is not focused
    TOTK_WINDOW_SIZE        = '1920x1080'
    TOTK_FULLSCREEN         = '0'                  # overrides a dkb.ini [display] fullscreen = 1 (a6 has one)
    TOTK_OUTPUT_RES         = '1920x1080'
    TOTK_RENDER_SCALE       = '100'
    TOTK_FPS_LIMIT          = '60'
    TOTK_VK_PRESENT         = 'mailbox'            # the default, pinned so the run does not depend on it
    TOTK_PERF_OVERLAY       = 'off'
    TOTK_INPUT_SCRIPT       = $inputPath
    TOTK_SHADER_CACHE       = $cache
}
if ($Realtime -ne '') { $settings['TOTK_REALTIME'] = $Realtime }
if ($Measure -eq 'flight') {
    # Per-frame record: the flight recorder, one dump per 1200 presents (the dumps tile the run).
    $settings['TOTK_FLIGHT'] = '1'
    $settings['TOTK_FLIGHT_FRAMES'] = '1200'
    $settings['TOTK_FLIGHT_THUMBS'] = '0'
    $settings['TOTK_FLIGHT_SPIKE_MS'] = '50'
    $settings['TOTK_FLIGHT_DIR'] = $flightDir
    $settings['TOTK_FLIGHT_AT'] = ((1..15 | ForEach-Object { $_ * 1200 }) -join ',')
}
if ($Thumbs) {
    # Verification only: thumbnails of the last 120 frames before each dump, dumps every 300 presents from tick 1800.
    $settings['TOTK_FLIGHT'] = '1'
    $settings['TOTK_FLIGHT_FRAMES'] = '120'
    $settings['TOTK_FLIGHT_THUMBS'] = '1'
    $settings['TOTK_FLIGHT_SPIKE_MS'] = '0'
    $settings['TOTK_FLIGHT_DIR'] = $flightDir
    $settings['TOTK_FLIGHT_AT'] = ((0..14 | ForEach-Object { 2900 + $_ * 240 }) -join ',')
}
# Diagnostics that stay off: no TOTK_TRACE, TOTK_PROFILE, validation, input recording or live input.
$diagnosticsOff = @('TOTK_TRACE', 'TOTK_PROFILE', 'TOTK_PROFILE_RENDER', 'TOTK_PROFILE_CHAINS', 'TOTK_SPEED_PROBE',
    'TOTK_VK_VALIDATION', 'TOTK_INPUT_RECORD', 'TOTK_CRIT', 'DKB_LIVE_INPUT')
if ($Measure -eq 'summary') { $diagnosticsOff += @('TOTK_FLIGHT', 'TOTK_FLIGHT_*') }

# ---------------------------------------------------------------- 6. machine and build metadata
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $exeSrc).Hash
$exeItem = Get-Item -LiteralPath $exeSrc
# nvidia-smi exists only with an NVIDIA driver; with $ErrorActionPreference = 'Stop' a missing command would end the script.
$hasSmi = [bool](Get-Command nvidia-smi -ErrorAction SilentlyContinue)
$gpu = if ($hasSmi) { (& nvidia-smi --query-gpu=name,driver_version,vbios_version --format=csv,noheader 2>$null | Select-Object -First 1) -split ',' } else { @('n/a (no nvidia-smi)') }
$plan = (powercfg /getactivescheme 2>$null) -join ' '
Add-Type -AssemblyName System.Windows.Forms
$ps0 = [System.Windows.Forms.SystemInformation]::PowerStatus
$cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name
$os = Get-CimInstance Win32_OperatingSystem
$ramGb = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB, 1)
$gitHead = ''
try { $gitHead = (git -C $repo rev-parse --short HEAD 2>$null) } catch {}
$iniPath = Join-Path $binDir 'dkb.ini'
if (-not (Test-Path -LiteralPath $iniPath)) { $iniPath = Join-Path $binDir 'totk.ini' }   # a TotK build reads totk.ini
$iniInfo = if (Test-Path -LiteralPath $iniPath) { [ordered]@{ present = $true; sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $iniPath).Hash; text = (Get-Content -LiteralPath $iniPath -Raw) } } else { [ordered]@{ present = $false } }

$run = [ordered]@{
    label             = $Label
    visit             = $Visit
    measure           = $Measure
    script            = $Script
    thumbs            = [bool]$Thumbs
    from              = $fromFull
    status            = 'starting'
    started           = (Get-Date).ToString('o')
    harness_note      = 'Harness test. A 3-minute run is not acceptance evidence for gates 10-14.'
    repo_head         = $gitHead
    exe               = [ordered]@{ source = $exeSrc; run_copy = $exePath; sha256 = $hash; bytes = $exeItem.Length; modified = $exeItem.LastWriteTime.ToString('o') }
    runtime_copied    = $copied
    settings_ini      = $iniInfo
    exefs             = $exefs
    romfs             = $romfs
    settings          = $settings
    diagnostics_off   = $diagnosticsOff
    inherited_cleared = @()
    machine           = [ordered]@{
        cpu = $cpu; ram_gb = $ramGb; os = "$($os.Caption) build $($os.BuildNumber)"
        gpu = ($gpu -join ',').Trim(); power_plan = $plan.Trim()
        power_line_at_start = [string]$ps0.PowerLineStatus; battery_percent_at_start = $ps0.BatteryLifePercent
    }
    seconds_requested = $Seconds; warm_after_s = $WarmAfter; sample_every_s = $SampleSeconds
}
$runPath = Join-Path $bundle 'run.json'
Write-Json $run $runPath

if ($DryRun) {
    $run.status = 'dry-run'; $run.finished = (Get-Date).ToString('o'); Write-Json $run $runPath
    Say "dry run: inputs, guard and bundle ok -> $bundle"
    return
}

# ---------------------------------------------------------------- 7. clear inherited TOTK_/DKB_ variables, set ours; restored in finally
$saved = @{}
Get-ChildItem Env: | Where-Object { $_.Name -match '^(TOTK|DKB)_' } | ForEach-Object { $saved[$_.Name] = $_.Value; Remove-Item -LiteralPath "Env:$($_.Name)" }
$run.inherited_cleared = @($saved.Keys | Sort-Object)
# -ExtraEnv NAME=VALUE (for example a profiler): recorded in run.json settings, so the gate-14 check sees it. The token
# {prof} is replaced by <bundle>\prof, which is created here.
$profDir = Join-Path $bundle 'prof'
New-Item -ItemType Directory -Path $profDir | Out-Null
foreach ($pair in ($ExtraEnv -split ';' | Where-Object { $_ })) {
    $name, $value = $pair -split '=', 2
    if (-not $name -or $value -eq $null) { throw "-ExtraEnv needs NAME=VALUE: $pair" }
    $settings[$name] = $value.Replace('{prof}', $profDir)
}
$run.settings = $settings
foreach ($k in $settings.Keys) { Set-Item -LiteralPath "Env:$k" -Value ([string]$settings[$k]) }

$csv = Join-Path $bundle 'samples.csv'
$game = $null; $shell = $null; $stop = 'duration'; $forced = $false
try {
    # ------------------------------------------------------------ 8. launch: stdout to a file, stderr stays on the (hidden) console
    $stdout = Join-Path $bundle 'stdout.txt'
    $argLine = '/d /c ""{0}" --exefs "{1}" --romfs "{2}" --profile "{3}" >"{4}""' -f $exePath, $exefs, $romfs, $gameProfile, $stdout
    $shell = Start-Process -FilePath (Join-Path $env:SystemRoot 'System32\cmd.exe') -ArgumentList $argLine `
        -WorkingDirectory $exeFolder -WindowStyle Hidden -PassThru
    for ($i = 0; $i -lt 150 -and -not $game; $i++) {
        Start-Sleep -Milliseconds 200
        $child = Get-CimInstance Win32_Process -Filter "ParentProcessId = $($shell.Id) AND Name = 'dkb.exe'" -ErrorAction SilentlyContinue
        if ($child) { $game = Get-Process -Id $child.ProcessId -ErrorAction SilentlyContinue }
        if ($shell.HasExited -and -not $game) { break }
    }
    if (-not $game) { throw "dkb.exe did not start (cmd exit code $($shell.ExitCode))" }
    $run.status = 'running'; $run.game_pid = $game.Id; $run.launched = (Get-Date).ToString('o'); Write-Json $run $runPath
    Say "dkb.exe PID $($game.Id) running from $exeFolder; $Seconds s; samples every $SampleSeconds s"

    # ------------------------------------------------------------ 9. sampler: GPU (nvidia-smi) and process memory, every $SampleSeconds
    'elapsed_s,ac_line,gpu_temp_c,gpu_power_w,gpu_sm_mhz,gpu_sm_max_mhz,gpu_mem_mhz,gpu_util_pct,gpu_mem_used_mib,gpu_throttle_mask,proc_working_set_mib,proc_commit_mib,proc_peak_ws_mib,proc_handles,proc_threads' |
        Set-Content -LiteralPath $csv -Encoding ascii
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $nextSample = 0.0
    while ($true) {
        $t = $sw.Elapsed.TotalSeconds
        if ($t -ge $nextSample) {
            $nextSample += $SampleSeconds
            $g = if (-not $hasSmi) { $null } else { (& nvidia-smi --query-gpu=temperature.gpu,power.draw,clocks.sm,clocks.max.sm,clocks.mem,utilization.gpu,memory.used,clocks_throttle_reasons.active --format=csv,noheader,nounits 2>$null | Select-Object -First 1) }
            $g = if ($g) { ($g -replace '\s', '') } else { 'n/a,n/a,n/a,n/a,n/a,n/a,n/a,n/a' }
            $p = Get-Process -Id $game.Id -ErrorAction SilentlyContinue
            # F1 with the invariant culture: no thousands separator and always a '.', whatever the user's culture is.
            $pv = if ($p) { Inv '{0:F1},{1:F1},{2:F1},{3},{4}' @(($p.WorkingSet64 / 1MB), ($p.PrivateMemorySize64 / 1MB), ($p.PeakWorkingSet64 / 1MB), $p.HandleCount, $p.Threads.Count) } else { 'n/a,n/a,n/a,n/a,n/a' }
            $ac = [System.Windows.Forms.SystemInformation]::PowerStatus.PowerLineStatus
            Add-Content -LiteralPath $csv -Encoding ascii -Value (Inv '{0:F1},{1},{2},{3}' @($t, $ac, $g, $pv))
        }
        if ($game.HasExited) { $stop = 'game exited on its own'; break }
        if ($t -ge $Seconds) { break }
        Start-Sleep -Milliseconds 250
    }

    # ------------------------------------------------------------ 10. close: window close is the game's normal exit path (flushes logs)
    if (-not $game.HasExited) {
        if (-not $game.CloseMainWindow() -or -not $game.WaitForExit(60000)) {
            $forced = $true; $stop = 'killed: no exit 60 s after window close'
            $game.Kill()
        }
    }
    [void]$shell.WaitForExit(10000)
} finally {
    if ($game -and -not $game.HasExited) { $forced = $true; $stop = 'killed by the runner after an error'; $game.Kill() }
    foreach ($k in $settings.Keys) { Remove-Item -LiteralPath "Env:$k" -ErrorAction SilentlyContinue }
    foreach ($k in $saved.Keys) { Set-Item -LiteralPath "Env:$k" -Value $saved[$k] }
    # cmd /c returns the game's exit code; the game's own process object may no longer report one.
    $exitCode = if ($shell -and $shell.HasExited) { $shell.ExitCode } else { $null }
    $run.status = if ($forced -or $stop -ne 'duration') { 'ended-early' } else { 'finished' }
    $run.stop = $stop; $run.forced_close = $forced; $run.game_exit_code = $exitCode
    $run.finished = (Get-Date).ToString('o')
    Write-Json $run $runPath
    $complete = ($run.status -eq 'finished')
    Write-Json ([ordered]@{ visit = $Visit; complete = $complete; from = $fromFull; stop = $stop; exit_code = $exitCode }) (Join-Path $bundle 'visit.json')
    Say "stop: $stop; exit code $exitCode"
}

# ---------------------------------------------------------------- 11. summarise
$py = Join-Path $repo '.venv\Scripts\python.exe'
if (-not (Test-Path -LiteralPath $py)) { $py = 'python' }
& $py -I (Join-Path $repo 'tools\perf_evidence.py') $bundle --warm-after $WarmAfter
Say "bundle: $bundle"
