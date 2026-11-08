# Run interactively in elevated Windows PowerShell. Never supply credentials as arguments.
# Do not run under transcription, tracing, or registry auditing that records values.
#Requires -RunAsAdministrator
$ErrorActionPreference = 'Stop'
$secure = $null
$plain = $null
$bstr = [IntPtr]::Zero
$key = $null
$handoff = $false
try {
    $devices = @(Get-CimInstance Win32_NetworkAdapter | Where-Object {
        $_.ServiceName -eq 'J316Wifi' -and $_.PNPDeviceID -match '^ACPI\\APPL0010(?:\\|$)'
    })
    if ($devices.Count -eq 0) { throw 'No adapter' }
    for ($i = 0; $i -lt $devices.Count; $i++) {
        Write-Host ("[{0}] {1}" -f $i, $devices[$i].NetConnectionID)
    }
    $choice = 0
    if ($devices.Count -gt 1) {
        $answer = Read-Host 'Adapter number'
        if (-not [int]::TryParse($answer, [ref]$choice) -or $choice -lt 0 -or $choice -ge $devices.Count) {
            throw 'Invalid selection'
        }
    }
    $guid = [guid]$devices[$choice].GUID
    $adapter = @(Get-NetAdapter -IncludeHidden | Where-Object { [guid]$_.InterfaceGuid -eq $guid })
    if ($adapter.Count -ne 1) { throw 'Adapter unavailable' }
    $class = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4D36E972-E325-11CE-BFC1-08002BE10318}'
    $keys = @(Get-ChildItem $class | Where-Object {
        $p = Get-ItemProperty $_.PSPath
        $p.NetCfgInstanceId -and $p.NetCfgInstanceId.Trim('{}') -ieq $guid.ToString()
    })
    if ($keys.Count -ne 1) { throw 'Configuration unavailable' }
    $key = $keys[0].PSPath
    $ssid = Read-Host 'SSID'
    $utf8 = New-Object System.Text.UTF8Encoding($false, $true)
    $size = $utf8.GetByteCount($ssid)
    if ($size -lt 1 -or $size -gt 32 -or $ssid.IndexOf([char]0) -ge 0) { throw 'Invalid SSID' }
    $secure = Read-Host 'PSK (8-63 printable ASCII characters)' -AsSecureString
    $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    $plain = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr)
    if (-not ($plain.Length -ge 8 -and $plain.Length -le 63 -and
        $plain -cmatch '^[\x20-\x7e]+$')) { throw 'Invalid PSK' }
    New-ItemProperty -Path $key -Name 'SSID' -PropertyType String -Value $ssid -Force | Out-Null
    # Mark before writing so even a partially failed write triggers cleanup.
    $handoff = $true
    New-ItemProperty -Path $key -Name 'PSK' -PropertyType String -Value $plain -Force | Out-Null
    $plain = $null
    Restart-NetAdapter -InputObject $adapter[0] -Confirm:$false | Out-Null
}
catch {
    # Never emit the original error record: registry cmdlet errors can include values.
    Write-Host 'Configuration failed. Check adapter selection, credential format, and elevation.'
    exit 1
}
finally {
    $plain = $null
    if ($bstr -ne [IntPtr]::Zero) { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr) }
    if ($null -ne $secure) { $secure.Dispose() }
    if ($handoff) {
        try { Remove-ItemProperty -Path $key -Name 'PSK' -ErrorAction Stop }
        catch {
            Write-Host 'Credential cleanup failed. Remove the PSK value from this adapter configuration immediately.'
            exit 1
        }
    }
}
Write-Host 'Adapter restarted; temporary PSK removed. Check link and DHCP status.'
