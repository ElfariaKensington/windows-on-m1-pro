# J316 Wi-Fi mode switch for ACPI\APPL0010. Run elevated (the .cmd files do that).
#   wifi      native Wi-Fi adapter (J316WifiCx): scan and connect from Windows Settings.
#   ethernet  Ethernet bridge (J316Wifi): m1n1 joins Wi-Fi before Windows starts; Windows
#             sees a wired adapter. Use configure.ps1 (in ethernet\) for another network.
#   status    show which driver the device uses.
# Both packages are test-signed with the bundled CN=J316 WiFi Development certificate.
# The host keeps native mode until the next boot, so a switch to ethernet needs a restart.
param([ValidateSet('wifi', 'ethernet', 'status', 'menu')][string]$Mode = 'menu')
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$device = 'ACPI\APPL0010\0'

function Show-Status {
    $svc = $null
    try { $svc = (Get-PnpDeviceProperty -InstanceId $device -KeyName DEVPKEY_Device_Service -ErrorAction Stop).Data } catch { }
    $ver = $null
    try { $ver = (Get-PnpDeviceProperty -InstanceId $device -KeyName DEVPKEY_Device_DriverVersion -ErrorAction Stop).Data } catch { }
    $mode = switch ($svc) { 'J316WifiCx' { 'Wi-Fi (native)' } 'J316Wifi' { 'Ethernet bridge' } default { 'no J316 driver' } }
    Write-Host "Current mode: $mode  (service '$svc', driver $ver)"
    return $svc
}

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run as administrator.' }

if ($Mode -eq 'menu') {
    $null = Show-Status
    Write-Host ''
    Write-Host '  1  Wi-Fi mode      (Windows Wi-Fi adapter, connect from Settings)'
    Write-Host '  2  Ethernet mode   (m1n1 connects; Windows sees a wired network)'
    Write-Host '  3  Exit'
    $choice = Read-Host 'Choose 1, 2 or 3'
    switch ($choice) { '1' { $Mode = 'wifi' } '2' { $Mode = 'ethernet' } default { exit 0 } }
}
if ($Mode -eq 'status') { $null = Show-Status; exit 0 }

if (-not ((bcdedit /enum '{current}') -match 'testsigning\s+Yes')) {
    throw 'Test signing is off: run "bcdedit /set testsigning on" as administrator, restart, then run this again.'
}
if ($Mode -eq 'wifi') { $dir = Join-Path $here 'wifi'; $inf = 'J316WifiCx.inf'; $service = 'J316WifiCx' }
else { $dir = Join-Path $here 'ethernet'; $inf = 'J316Wifi.inf'; $service = 'J316Wifi' }
foreach ($f in $inf, ($inf -replace '\.inf$', '.sys'), ($inf -replace '\.inf$', '.cat')) {
    if (-not (Test-Path (Join-Path $dir $f))) { throw "Missing $dir\$f" }
}
$cer = Join-Path $here 'J316WifiCx.cer'
foreach ($store in 'Root', 'TrustedPublisher') {
    certutil -f -addstore $store $cer | Out-Null
    if ($LASTEXITCODE) { throw "certutil -addstore $store failed: $LASTEXITCODE" }
}

# Remove every staged copy of both packages (older versions included), then install the
# selected one, so PnP cannot rank an old package above it.
$enum = pnputil /enum-drivers | Out-String
foreach ($block in ($enum -split "(\r?\n){2,}")) {
    if ($block -match 'Original Name:\s+(j316wifi|j316wificx)\.inf' -and $block -match 'Published Name:\s+(oem\d+\.inf)') {
        $published = $Matches[1]
        Write-Host "Removing staged package $published"
        pnputil /delete-driver $published /uninstall /force | Out-Null
    }
}
pnputil /add-driver (Join-Path $dir $inf) /install | Out-Host
if ($LASTEXITCODE -and $LASTEXITCODE -ne 3010) { throw "pnputil /add-driver failed: $LASTEXITCODE (see C:\Windows\INF\setupapi.dev.log)" }
sc.exe config $service start= demand | Out-Null
pnputil /scan-devices | Out-Null

if ($Mode -eq 'wifi') {
    Set-Service WlanSvc -StartupType Automatic
    Start-Service WlanSvc
}
Start-Sleep 3
$null = Show-Status
Write-Host ''
if ($Mode -eq 'ethernet') {
    Write-Host 'Ethernet mode is installed. Restart Windows now: m1n1 joins the Wi-Fi network built into'
    Write-Host 'the boot image before Windows starts. For another network run ethernet\configure.ps1.'
} else {
    Write-Host 'Wi-Fi mode is installed. Restart Windows, then pick a network in Settings > Network > Wi-Fi.'
    Write-Host "Driver log: $env:SystemRoot\Temp\J316WifiCx.log"
}
$answer = Read-Host 'Restart now? (y/n)'
if ($answer -match '^[yYeE]') { shutdown /r /t 5 }
