# Create a WPA2-Personal profile WITH its key and connect (native Wi-Fi mode), then show proof.
# Windows runs the WPA2 handshake itself and needs the passphrase from a saved profile:
# a connection started without one ("Connection to a secure network without a profile" in the
# WLAN-AutoConfig log) can never complete, whatever the driver does.
# usage (administrator): wifi-connect.ps1 [-Ssid name] [-Password pass] [-Open]
param([string]$Ssid, [string]$Password, [switch]$Open)
$ErrorActionPreference = 'Stop'
function New-J316WlanProfile([string]$Ssid, [string]$Password, [switch]$Open) {
    $ssidBytes = [Text.Encoding]::UTF8.GetBytes($Ssid)
    if ($ssidBytes.Length -lt 1 -or $ssidBytes.Length -gt 32) { throw 'An SSID has 1 to 32 UTF-8 bytes.' }
    $name = [Security.SecurityElement]::Escape($Ssid)
    $hex = [BitConverter]::ToString($ssidBytes).Replace('-', '')
    if ($Open) {
        $security = '<authEncryption><authentication>open</authentication><encryption>none</encryption><useOneX>false</useOneX></authEncryption>'
    } else {
        if ($Password.Length -lt 8 -or $Password.Length -gt 63) { throw 'A WPA2 passphrase has 8 to 63 characters.' }
        $key = [Security.SecurityElement]::Escape($Password)
        $security = '<authEncryption><authentication>WPA2PSK</authentication><encryption>AES</encryption><useOneX>false</useOneX></authEncryption>' +
                    "<sharedKey><keyType>passPhrase</keyType><protected>false</protected><keyMaterial>$key</keyMaterial></sharedKey>"
    }
    return '<?xml version="1.0"?><WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">' +
           "<name>$name</name><SSIDConfig><SSID><hex>$hex</hex><name>$name</name></SSID></SSIDConfig>" +
           '<connectionType>ESS</connectionType><connectionMode>auto</connectionMode>' +
           "<MSM><security>$security</security></MSM></WLANProfile>"
}
$log = Join-Path $env:SystemRoot 'Temp\J316WifiConnect.txt'
try { Start-Transcript -Path $log -Force | Out-Null } catch { }
function Show($t) { Write-Host $t }
try {
    if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole('Administrator')) {
        throw 'Run this as administrator (right-click > Run as administrator).'
    }
    $svc = Get-Service WlanSvc -ErrorAction SilentlyContinue
    if (-not $svc) { throw 'WLAN AutoConfig service not present: run "Use Wi-Fi.cmd" first and restart.' }
    if ($svc.Status -ne 'Running') { Start-Service WlanSvc; Start-Sleep 2 }
    $ifs = (netsh wlan show interfaces) -join "`n"
    if ($ifs -notmatch 'Name') { throw 'No Wi-Fi interface: run "Use Wi-Fi.cmd", restart, then run this again.' }

    if (-not $Ssid) { $Ssid = Read-Host 'Network name (SSID)' }
    if (-not $Open -and -not $Password) {
        $sec = Read-Host 'Password (empty for an open network)' -AsSecureString
        $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec)
        try { $Password = [Runtime.InteropServices.Marshal]::PtrToStringAuto($bstr) }
        finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr) }
    }
    if ($Open -or [string]::IsNullOrEmpty($Password)) { $Open = $true }
    $xml = New-J316WlanProfile -Ssid $Ssid -Password $Password -Open:$Open
    $tmp = Join-Path $env:TEMP ('j316-wlan-' + [guid]::NewGuid().ToString('N') + '.xml')
    try {
        Set-Content -Path $tmp -Value $xml -Encoding UTF8
        $r = netsh wlan add profile filename="$tmp" user=all
        $importExit = $LASTEXITCODE
        Show ($r -join ' ')
        if ($importExit -ne 0) { throw "Windows rejected the profile (netsh exit $importExit). Existing profiles were preserved." }
    } finally { Remove-Item $tmp -Force -ErrorAction SilentlyContinue }
    $prof = (netsh wlan show profile name="$Ssid") -join "`n"
    if ($LASTEXITCODE -ne 0 -or $prof -notmatch [regex]::Escape($Ssid)) {
        Show ($xml -replace '<keyMaterial>.*</keyMaterial>', '<keyMaterial>(hidden)</keyMaterial>')
        throw "Windows rejected the profile for '$Ssid' (see the netsh message above)."
    }
    $since = Get-Date

    Show "Connecting to '$Ssid' ..."
    Show ((netsh wlan connect name="$Ssid" ssid="$Ssid") -join ' ')
    $ok = $false
    for ($i = 0; $i -lt 30; $i++) {
        Start-Sleep 1
        $s = (netsh wlan show interfaces) -join "`n"
        if ($s -match 'State\s*:\s*connected') { $ok = $true; break }
    }
    Show ''
    Show ((netsh wlan show interfaces) -join "`n")
    Show ((netsh wlan show profile name="$Ssid" | Select-String 'Authentication|Cipher|Security key') -join "`n")
    if ($ok) {
        Start-Sleep 4
        Show ''
        Show ((Get-NetIPConfiguration -ErrorAction SilentlyContinue | Where-Object { $_.NetAdapter.InterfaceDescription -match 'J316 Wi-Fi' } | Out-String))
        Show 'CONNECTED.'
    } else {
        Show 'NOT CONNECTED within 30 s.'
        try {
            Get-WinEvent -LogName 'Microsoft-Windows-WLAN-AutoConfig/Operational' -MaxEvents 60 -ErrorAction Stop |
                Where-Object { $_.TimeCreated -ge $since.AddSeconds(-2) -and $_.Id -in 8000,8001,8002,8003,11001,11004,11005,11006,11010,12011,12012,12013 } |
                Sort-Object TimeCreated | ForEach-Object { Show ("{0:HH:mm:ss} {1} {2}" -f $_.TimeCreated, $_.Id, (($_.Message -split "`n" | Where-Object { $_ -match 'Reason|Mode|Auth|Hint' }) -join ' | ')) }
        } catch { Show ("event log: " + $_.Exception.Message) }
        $drv = Join-Path $env:SystemRoot 'Temp\J316WifiCx.log'
        if (Test-Path $drv) {
            Show 'Driver connect/EAPOL lines:'
            Show ((Get-Content $drv | Select-String 'cmd 6 |cmd 8 |M4 6[4-9]|EAPOL|conn|join|assoc|link|peer|data path|key|fw event' | Select-Object -Last 60 | ForEach-Object { $_.Line }) -join "`n")
            Show 'Driver log tail:'
            Show ((Get-Content $drv -Tail 25) -join "`n")
        }
        Show ("Send or keep: $log and $drv")
    }
} catch {
    Show ("ERROR: " + $_.Exception.Message)
} finally {
    try { Stop-Transcript | Out-Null } catch { }
}
