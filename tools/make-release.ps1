# Build the files of a release into out\release-<version>:
#   tailscale-<version>.elf       the payload
#   tailscale-<version>.elf.sig   its signature, which the status page's "Install" checks
#   SHA256SUMS.txt
# and updates payloads.json at the repository root (commit it after publishing).
#
#   .\tools\make-release.ps1 -Version 1.2.3
#
# Needs the release signing key on this machine (see docs/BUILDING.md).
param(
    [Parameter(Mandatory = $true)][string]$Version,
    # Also write the payload and its signature under the plain names
    # tailscale.elf and tailscale.elf.sig, which is what the Install button of
    # 0.6.0 looks for. Attach them as well to let 0.6.0 install this release.
    [switch]$PlainName
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'env.ps1')

if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "version must look like 1.2.3, not '$Version'" }

& (Join-Path $PSScriptRoot 'build-payload.ps1') -GoDir tsd -Name tailscale -Version $Version -HomeIcon -RemotePlay

$rel = Join-Path $DevRoot "out\release-$Version"
New-Item -ItemType Directory -Force $rel | Out-Null
$name = "tailscale-$Version.elf"   # the name the status page's Install looks for
$elf = Join-Path $rel $name
Copy-Item (Join-Path $DevRoot 'out\tailscale.elf') $elf -Force

Push-Location (Join-Path $DevRoot 'tsd')
try {
    go run ./cmd/signrelease sign -version $Version -file $elf
    if ($LASTEXITCODE -ne 0) { throw 'signing failed' }
    go run ./cmd/signrelease verify -file $elf
    if ($LASTEXITCODE -ne 0) { throw 'the signature does not verify' }
    # The payload must carry the public half of the key it was signed with,
    # or consoles running it could never install the release after it.
    $pub = go run ./cmd/signrelease pubkey
    if (-not (Select-String -Path 'selfupdate.go' -SimpleMatch $pub -Quiet)) {
        throw "tsd\selfupdate.go does not have this machine's public key ($pub) as updatePublicKey"
    }
} finally { Pop-Location }

$hash = (Get-FileHash $elf -Algorithm SHA256).Hash.ToLower()
$sums = "$hash  $name`n"
if ($PlainName) {
    # The same bytes, so the same signature is valid for both.
    Copy-Item $elf (Join-Path $rel 'tailscale.elf') -Force
    Copy-Item "$elf.sig" (Join-Path $rel 'tailscale.elf.sig') -Force
    $sums += "$hash  tailscale.elf`n"
}
[IO.File]::WriteAllText((Join-Path $rel 'SHA256SUMS.txt'), $sums)

# The Payload Manager catalog (CUSTOM_REPOSITORIES.md in its repository),
# committed at the repository root after the release is published. The file
# name stays the same across versions so that autoload settings keep working.
$catalog = [ordered]@{
    name     = 'neokuze/ps5-tailscale'
    payloads = @([ordered]@{
        name        = 'Tailscale (Remote Play)'
        filename    = 'ps5-tailscale.elf'
        url         = "https://github.com/neokuze/ps5-tailscale/releases/download/v$Version/$name"
        description = 'Puts the PS5 on your Tailscale network for Remote Play from anywhere. Pair Chiaki with a PIN from the status page on port 8090.'
        version     = "v$Version"
        category    = 'Networking'
        checksum    = $hash
    })
}
[IO.File]::WriteAllText((Join-Path $DevRoot 'payloads.json'), ($catalog | ConvertTo-Json -Depth 4) + "`n")

Get-ChildItem $rel | Select-Object Name, Length | Format-Table -AutoSize
Write-Host "release files are in $rel"
