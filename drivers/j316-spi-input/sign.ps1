param([Parameter(Mandatory=$true)][string]$Package,
      [Parameter(Mandatory=$true)][string]$CertificateThumbprint)
$ErrorActionPreference = 'Stop'
$inf2cat = (Get-Command inf2cat.exe -ErrorAction Stop).Source
$signtool = (Get-Command signtool.exe -ErrorAction Stop).Source
& $signtool sign /sha1 $CertificateThumbprint /fd SHA256 /ph "$Package\J316SpiInput.sys"
if ($LASTEXITCODE) { throw "Driver signing failed: $LASTEXITCODE" }
& $inf2cat "/driver:$Package" /os:10_GE_ARM64 /uselocaltime
if ($LASTEXITCODE) { throw "Inf2Cat failed: $LASTEXITCODE" }
& $signtool sign /sha1 $CertificateThumbprint /fd SHA256 "$Package\J316SpiInput.cat"
if ($LASTEXITCODE) { throw "Catalog signing failed: $LASTEXITCODE" }
& $signtool verify /pa /v /c "$Package\J316SpiInput.cat" "$Package\J316SpiInput.sys"
if ($LASTEXITCODE) { throw "Package verification failed: $LASTEXITCODE" }
