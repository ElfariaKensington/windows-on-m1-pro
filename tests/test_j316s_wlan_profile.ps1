param([string]$ScriptPath = "$PSScriptRoot/../drivers/j316-wificx/wifi-connect.ps1")
$ErrorActionPreference = 'Stop'
$ast = [Management.Automation.Language.Parser]::ParseFile((Resolve-Path $ScriptPath), [ref]$null, [ref]$null)
$function = $ast.Find({ param($a) $a -is [Management.Automation.Language.FunctionDefinitionAst] -and $a.Name -eq 'New-J316WlanProfile' }, $true)
if (-not $function) { throw 'Profile builder missing' }
. ([scriptblock]::Create($function.Extent.Text))
function Assert($condition, $message) { if (-not $condition) { throw $message } }
foreach ($ssid in @('TurkTelekom_ZNVMH', 'A&B <office>', ('a' * 32), ('a' * 30 + [char]0xe9))) {
    $password = 'test&<"''>$1234'
    [xml]$xml = New-J316WlanProfile -Ssid $ssid -Password $password
    $p = $xml.WLANProfile
    Assert ($p.NamespaceURI -ceq 'http://www.microsoft.com/networking/WLAN/profile/v1') 'WLAN namespace'
    Assert ($p.MSM.security.sharedKey.keyType -ceq 'passPhrase') 'Windows keyType is case-sensitive'
    Assert ($p.MSM.security.sharedKey.keyMaterial -ceq $password) 'Key must survive XML escaping'
    Assert ($p.name -ceq $ssid -and $p.SSIDConfig.SSID.name -ceq $ssid) 'SSID must survive XML escaping'
    Assert ($p.SSIDConfig.SSID.hex -ceq [BitConverter]::ToString([Text.Encoding]::UTF8.GetBytes($ssid)).Replace('-', '')) 'SSID bytes'
    Assert ($p.MSM.security.authEncryption.authentication -ceq 'WPA2PSK') 'WPA2 authentication'
}
[xml]$open = New-J316WlanProfile -Ssid 'open' -Open
Assert ($null -eq $open.WLANProfile.MSM.security.sharedKey) 'Open networks have no key'
foreach ($length in @(8, 63)) { [xml]$null = New-J316WlanProfile -Ssid 'test' -Password ('a' * $length) }
foreach ($length in @(0, 7, 64)) {
    $rejected = $false
    try { New-J316WlanProfile -Ssid 'test' -Password ('a' * $length) | Out-Null } catch { $rejected = $true }
    Assert $rejected "Invalid password length $length"
}
foreach ($ssid in @('', ('a' * 33), ('a' * 31 + [char]0xe9))) {
    $rejected = $false
    try { New-J316WlanProfile -Ssid $ssid -Open | Out-Null } catch { $rejected = $true }
    Assert $rejected 'Invalid SSID byte length'
}
Write-Output 'PASS: WPA2/open profile generation, case-sensitive keyType, XML escaping, SSID bytes and bounds.'
