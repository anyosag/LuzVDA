#Requires -RunAsAdministrator
<#
.SYNOPSIS
Creates the certificate that signs your LuzVDA builds. Run it once, on the PC that will sign them.

.DESCRIPTION
The private key is created in this PC's certificate store and can't be exported, so it never leaves
the PC: not to GitHub, not to a file. Anyone who could still take it from the store already has
administrator rights here.

The script also makes this PC trust the certificate for drivers (Trusted Root and Trusted Publisher),
which Windows needs to install a driver signed by it. Its code-signing-only usage limits what that
trust covers, but any PC that trusts it will install drivers and run software you sign, so add it
only to your own machines.

Writes the public certificate, luzvda.cer, next to this script. It holds no secret: commit it to
Apollo (src_assets/windows/drivers/luzvda/) so installs can trust your builds.

.EXAMPLE
.\new-signing-cert.ps1
#>
param(
  [string]$Subject = "CN=LuzVDA driver signing ($env:COMPUTERNAME)",
  [int]$Years = 5,
  [string]$CerPath = (Join-Path $PSScriptRoot 'luzvda.cer')
)

$ErrorActionPreference = 'Stop'

$cert = Get-ChildItem Cert:\LocalMachine\My |
  Where-Object { $_.Subject -eq $Subject -and $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date) } |
  Sort-Object NotAfter -Descending |
  Select-Object -First 1

if ($cert) {
  Write-Host "Using the existing certificate $($cert.Thumbprint), valid until $($cert.NotAfter)"
} else {
  $cert = New-SelfSignedCertificate `
    -Type CodeSigningCert `
    -Subject $Subject `
    -CertStoreLocation Cert:\LocalMachine\My `
    -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 `
    -KeyExportPolicy NonExportable `
    -NotAfter (Get-Date).AddYears($Years)
  Write-Host "Created $($cert.Thumbprint), valid until $($cert.NotAfter)"
}

Export-Certificate -Cert $cert -FilePath $CerPath -Type CERT | Out-Null

foreach ($store in 'Root', 'TrustedPublisher') {
  if (-not (Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object Thumbprint -eq $cert.Thumbprint)) {
    Import-Certificate -FilePath $CerPath -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
    Write-Host "Trusted in LocalMachine\$store"
  }
}

Write-Host ""
Write-Host "Subject:     $Subject"
Write-Host "Thumbprint:  $($cert.Thumbprint)"
Write-Host "Public cert: $CerPath"
