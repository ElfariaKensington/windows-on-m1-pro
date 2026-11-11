# Apple M1 Pro GPU (J316s AGX port) package helper.
#   gpu.ps1 install   trust the development certificate, install the driver, enable log collection
#   gpu.ps1 remove    uninstall the driver package (Basic Display returns)
#   gpu.ps1 collect   write GPU diagnostics to <this folder>\logs (runs at every boot once installed)
param([ValidateSet('install','remove','collect')][string]$Action = 'install')
$ErrorActionPreference = 'Continue'
$here = $PSScriptRoot
$logs = Join-Path $here 'logs'
New-Item -ItemType Directory -Force $logs | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$inf = Join-Path $here 'AppleAgxJ316s.inf'
$task = 'AppleAgxJ316sCollect'

function Find-Oem {
    $out = pnputil /enum-drivers
    $cur = $null
    foreach ($l in $out) {
        if ($l -match '^\s*Published Name\s*:\s*(oem\d+\.inf)') { $cur = $Matches[1] }
        if ($l -match '^\s*Original Name\s*:\s*appleagxj316s\.inf' -and $cur) { $cur }
    }
}

# Preserve the first armed start. A graphics-stack reset here can restart the
# adapter after G3Armed has been consumed and replace the original failure.
function Start-StartupTrace {
    $script:traceName = "J316Agx-$stamp"
    $script:tracePath = Join-Path $logs "startup-$stamp.etl"
    & logman start $script:traceName -ets -o $script:tracePath -f bincirc -max 64 -ft 1 -p 'Microsoft-Windows-DxgKrnl' 0xFFFFFFFFFFFFFFFF 5
    $script:traceStarted = ($LASTEXITCODE -eq 0)
    "GPU startup trace enabled: $script:traceStarted"
    if (-not $script:traceStarted) { throw 'Could not start GPU tracing. Driver installation has not begun.' }
}
function Stop-StartupTrace {
    if ($script:traceStarted) {
        & logman stop $script:traceName -ets
        "GPU startup trace stopped: $LASTEXITCODE; $script:tracePath"
        $script:traceStarted = $false
    }
}

if ($Action -eq 'install') {
    Start-Transcript -Path (Join-Path $logs "install-$stamp.log") | Out-Null
    $cer = Join-Path $here 'J316Development.cer'
    certutil -f -addstore Root $cer
    certutil -f -addstore TrustedPublisher $cer
    bcdedit /enum '{current}' | Select-String -Pattern 'testsigning'
    Start-StartupTrace
    try {
    foreach ($old in (Find-Oem)) {
        pnputil /delete-driver $old /uninstall /force
        if ($LASTEXITCODE -ne 0) { throw "Could not remove $old (exit $LASTEXITCODE)" }
    }
    $serviceKey = 'HKLM:\SYSTEM\CurrentControlSet\Services\AppleAgxAdmission'
    # An earlier installer left a DisplayOwner-only key. SetupAPI cannot query it as
    # a service and fails AddService with ERROR_FILE_NOT_FOUND before DriverEntry.
    if ((Test-Path $serviceKey) -and -not (Get-ItemProperty -Path $serviceKey).ImagePath) {
        Remove-Item -Path $serviceKey -Recurse -Force
    }
    # The KMD requires the one-shot G3Armed value (consumed on StartDevice); without it StartDevice
    # returns STATUS_NOT_SUPPORTED (Code 43). Arm right before the install starts the device.
    $dp = 'HKLM:\SYSTEM\CurrentControlSet\Enum\ACPI\APPL0002\0\Device Parameters'
    New-Item -Path $dp -Force | Out-Null
    New-ItemProperty -Path $dp -Name G3Armed -PropertyType DWord -Value 1 -Force | Out-Null
    "armed G3Armed=1"
    pnputil /add-driver $inf /install
    "pnputil exit $LASTEXITCODE"
    if ($LASTEXITCODE -ne 0) { throw "GPU install failed (exit $LASTEXITCODE)" }
    Start-Sleep -Seconds 15
    } finally { Stop-StartupTrace }
    $ps = (Get-Command powershell).Source
    $arg = "-NoProfile -ExecutionPolicy Bypass -File `"$here\gpu.ps1`" collect"
    schtasks /Delete /TN $task /F 2>$null | Out-Null
    schtasks /Create /TN $task /RU SYSTEM /SC ONSTART /DELAY 0002:00 /RL HIGHEST /TR "`"$ps`" $arg" /F
    & $PSCommandPath collect
    Stop-Transcript | Out-Null
    Write-Host 'Done. Device was armed and started once live. Shut down / restart to macOS now; do not rerun.'
}
elseif ($Action -eq 'remove') {
    Start-Transcript -Path (Join-Path $logs "remove-$stamp.log") | Out-Null
    foreach ($old in (Find-Oem)) { pnputil /delete-driver $old /uninstall /force }
    schtasks /Delete /TN $task /F
    Stop-Transcript | Out-Null
    Write-Host 'Removed. Restart Windows.'
}
else {
    $d = Join-Path $logs "collect-$stamp"
    New-Item -ItemType Directory -Force $d | Out-Null
    pnputil /enum-devices /instanceid 'ACPI\APPL0002\0' /problem /drivers /resources > "$d\pnputil-appl0002.txt" 2>&1
    pnputil /enum-devices /class Display /drivers > "$d\pnputil-display.txt" 2>&1
    reg export 'HKLM\SYSTEM\CurrentControlSet\Services\AppleAgxAdmission' "$d\service.reg" /y > $null 2>&1
    reg export 'HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\APPL0002' "$d\enum-appl0002.reg" /y > $null 2>&1
    reg export 'HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}' "$d\display-class.reg" /y > $null 2>&1
    Get-WinEvent -FilterHashtable @{LogName='System'; StartTime=(Get-Date).AddHours(-2)} -ErrorAction SilentlyContinue |
        Select-Object TimeCreated, ProviderName, Id, LevelDisplayName, Message | Format-List | Out-File "$d\system-events.txt" -Width 400
    Get-WinEvent -LogName 'Microsoft-Windows-Kernel-PnP/Configuration' -MaxEvents 200 -ErrorAction SilentlyContinue |
        Select-Object TimeCreated, Id, Message | Format-List | Out-File "$d\pnp-events.txt" -Width 400
    Get-CimInstance Win32_VideoController | Format-List * | Out-File "$d\video-controller.txt" -Width 400
    $setup = "$env:windir\INF\setupapi.dev.log"
    if (Test-Path $setup) { Get-Content $setup -Tail 400 | Out-File "$d\setupapi-tail.txt" }
    if (Test-Path "$env:windir\Minidump") { Copy-Item "$env:windir\Minidump\*" $d -ErrorAction SilentlyContinue }
    if (Test-Path "$env:windir\LiveKernelReports\WATCHDOG") { Copy-Item "$env:windir\LiveKernelReports\WATCHDOG\*" $d -ErrorAction SilentlyContinue }
    $dx = Start-Process -FilePath "$env:windir\System32\dxdiag.exe" -ArgumentList @('/t', "`"$d\dxdiag.txt`"") -PassThru
    if (-not $dx.WaitForExit(30000)) {
        "dxdiag did not finish in 30 seconds" | Out-File "$d\dxdiag-timeout.txt"
        Stop-Process -Id $dx.Id -Force -ErrorAction SilentlyContinue
    }
    "collected $(Get-Date -Format o)" | Out-File "$d\done.txt"
}
