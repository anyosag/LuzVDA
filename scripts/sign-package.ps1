#Requires -RunAsAdministrator
<#
.SYNOPSIS
Signs a LuzVDA build from CI with your certificate (new-signing-cert.ps1), on this PC.

.DESCRIPTION
1. Checks the zip against the SHA256SUMS.txt CI published with it, and its build provenance with
   "gh attestation verify", so you sign exactly what CI built from the repository.
2. Signs LuzVDA.dll, luzvda-ctl.exe and the catalog luzvda.cat with signtool, from the Windows SDK.
   The catalog is what Windows checks when installing; it covers the INF and the DLL.
3. Adds your public certificate, luzvda.cer, and writes LuzVDA-<version>.zip with its SHA256.

Upload that zip to a LuzVDA release, then put its URL and SHA256 in Apollo's build-windows.yml.

.EXAMPLE
.\sign-package.ps1 -Zip .\LuzVDA-0.1.0.12-unsigned.zip
#>
param(
  [Parameter(Mandatory)] [string]$Zip,
  [string]$Sha256Sums,
  [string]$Repository = 'anyosag/LuzVDA',
  [string]$Subject = "CN=LuzVDA driver signing ($env:COMPUTERNAME)",
  [string]$TimestampUrl = 'http://timestamp.digicert.com',
  # Only if gh isn't installed and you've checked the build some other way
  [switch]$SkipAttestation
)

$ErrorActionPreference = 'Stop'

$Zip = (Resolve-Path $Zip).Path
$name = [IO.Path]::GetFileNameWithoutExtension($Zip)
if ($name -notmatch '^LuzVDA-(\d+\.\d+\.\d+\.\d+)-unsigned$') { throw "Expected LuzVDA-<version>-unsigned.zip, got $name.zip" }
$version = $Matches[1]

# 1. Is it what CI built?
if (-not $Sha256Sums) { $Sha256Sums = Join-Path (Split-Path $Zip) 'SHA256SUMS.txt' }
$expected = (Get-Content $Sha256Sums | Where-Object { $_ -match [regex]::Escape("$name.zip") }) -split '\s+' | Select-Object -First 1
$actual = (Get-FileHash $Zip -Algorithm SHA256).Hash.ToLower()
if (-not $expected -or $expected -ne $actual) { throw "SHA256 mismatch: SHA256SUMS.txt says '$expected', the zip is $actual" }
Write-Host "SHA256 matches: $actual"

if ($SkipAttestation) {
  Write-Warning 'Skipping the build provenance check'
} else {
  if (-not (Get-Command gh -ErrorAction SilentlyContinue)) { throw 'gh (GitHub CLI) is needed to check build provenance: winget install GitHub.cli' }
  gh attestation verify $Zip --repo $Repository
  if ($LASTEXITCODE -ne 0) { throw 'Build provenance check failed' }
}

# 2. Sign
$cert = Get-ChildItem Cert:\LocalMachine\My |
  Where-Object { $_.Subject -eq $Subject -and $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date) } |
  Sort-Object NotAfter -Descending |
  Select-Object -First 1
if (-not $cert) { throw "No certificate '$Subject' with a private key; run new-signing-cert.ps1 first" }

$signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" -ErrorAction SilentlyContinue |
  Sort-Object FullName -Descending |
  Select-Object -First 1
if (-not $signtool) { throw 'signtool.exe not found: install the Windows SDK (its "Signing Tools for Desktop Apps" part is enough)' }

$work = Join-Path ([IO.Path]::GetTempPath()) "luzvda-sign-$version"
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
Expand-Archive $Zip -DestinationPath $work

$timestamp = @()
if ($TimestampUrl) { $timestamp = @('/tr', $TimestampUrl, '/td', 'sha256') }

# The DLL and the tool first; the catalog hashes PE files without their signatures, so it stays valid
foreach ($file in 'LuzVDA.dll', 'luzvda-ctl.exe', 'luzvda.cat') {
  & $signtool.FullName sign /fd sha256 /sm /sha1 $cert.Thumbprint @timestamp (Join-Path $work $file)
  if ($LASTEXITCODE -ne 0) { throw "Signing $file failed" }
}

& $signtool.FullName verify /pa /c (Join-Path $work 'luzvda.cat') (Join-Path $work 'LuzVDA.dll')
if ($LASTEXITCODE -ne 0) { throw 'The catalog does not cover LuzVDA.dll' }

# 3. Package
Export-Certificate -Cert $cert -FilePath (Join-Path $work 'luzvda.cer') -Type CERT | Out-Null

$signed = Join-Path (Split-Path $Zip) "LuzVDA-$version.zip"
Remove-Item $signed -ErrorAction SilentlyContinue
Compress-Archive -Path "$work\*" -DestinationPath $signed
$hash = (Get-FileHash $signed -Algorithm SHA256).Hash.ToLower()
"$hash  LuzVDA-$version.zip" | Set-Content "$signed.sha256"
Remove-Item $work -Recurse -Force

Write-Host ""
Write-Host "Signed:     $signed"
Write-Host "SHA256:     $hash"
Write-Host "Thumbprint: $($cert.Thumbprint)"
Write-Host "Upload it to a release of $Repository, then set LUZVDA_RELEASE and its SHA256 in Apollo's build-windows.yml."
