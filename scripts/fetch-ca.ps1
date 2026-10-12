<#
.SYNOPSIS
Fetch the CA bundle that the Windows build needs.

.DESCRIPTION
WHY this script exists
---------------------
The Windows build links MSYS2's libcurl, which is GnuTLS-backed. GnuTLS reads
its trust anchors from a *file*. Windows has no such file: its trust store is a
registry-backed database that only schannel-backed curl can consult. So on
Windows every HTTPS request dies with CURLE_PEER_FAILED_VERIFICATION (curl error
60) and cfspeed measures nothing at all.

src/net.c therefore looks for a bundle named `cfspeed-ca.pem` sitting next to
the executable and points CURLOPT_CAINFO at it. CI has to fetch one and put it
there. Turning verification off instead would be a security regression on a
tool whose entire job is trusting a remote endpoint.

WHY this script fails hard
--------------------------
A truncated or zero-byte CA file is *worse* than no file. It looks present to the
loader, so the failure stops being loud and becomes silent. So the download is
validated in a temp file and only moved into place once it has been proven to be
a real PEM bundle. A hard failure here is the good outcome.

This is the same contract as scripts/fetch-ca.sh, which covers the Linux and
macOS legs; the MSYS2/Windows leg needs Invoke-WebRequest instead of curl.

.NOTES
PowerShell 5.1 compatible. No null-coalescing operator, no ternary.
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$OutPath = 'cfspeed-ca.pem'
)

$ErrorActionPreference = 'Stop'

$CaUrl = 'https://curl.se/ca/cacert.pem'

$TmpFile = $null

function Write-Fail([string]$Message) {
    Write-Host ('fetch-ca: error: ' + $Message) -ForegroundColor Red
    exit 1
}

function Remove-TmpFile {
    if ($null -ne $script:TmpFile) {
        if (Test-Path -LiteralPath $script:TmpFile) {
            Remove-Item -LiteralPath $script:TmpFile -Force -ErrorAction SilentlyContinue
        }
        $script:TmpFile = $null
    }
}

try {
    $script:TmpFile = Join-Path ([System.IO.Path]::GetTempPath()) ('cfspeed-ca.' + [System.Guid]::NewGuid().ToString('N') + '.pem')
}
catch {
    Write-Fail ('cannot create a temp file: ' + $_.Exception.Message)
}

# --- download -------------------------------------------------------------------

Write-Host ('fetch-ca: downloading ' + $CaUrl)

try {
    # Windows PowerShell 5.1 defaults to SSL3/TLS1.0 on many hosts, which curl.se
    # will not negotiate. -bor keeps whatever the host already allowed.
    [System.Net.ServicePointManager]::SecurityProtocol = `
        [System.Net.ServicePointManager]::SecurityProtocol -bor `
        [System.Net.SecurityProtocolType]::Tls12

    # -UseBasicParsing keeps this off the IE engine, which is not always present.
    Invoke-WebRequest -Uri $CaUrl -OutFile $script:TmpFile -UseBasicParsing -MaximumRedirection 5
}
catch {
    $detail = $_.Exception.Message
    Remove-TmpFile
    Write-Fail ('could not download the CA bundle from ' + $CaUrl + ': ' + $detail)
}

# --- validation, before anything is installed -----------------------------------

if (-not (Test-Path -LiteralPath $script:TmpFile)) {
    Remove-TmpFile
    Write-Fail 'the download did not produce a file'
}

$Length = (Get-Item -LiteralPath $script:TmpFile).Length

if ($Length -le 0) {
    Remove-TmpFile
    Write-Fail 'the download is empty - an empty CA bundle silently disables verification'
}

$Text = Get-Content -LiteralPath $script:TmpFile -Raw -ErrorAction Stop

if ($Text -notlike '*BEGIN CERTIFICATE*') {
    Remove-TmpFile
    Write-Fail 'the download contains no BEGIN CERTIFICATE marker - almost certainly an error page, not a bundle'
}

# A 10-byte slice of a bundle starts with "-----BEGIN" and would otherwise sail
# through the check above. Requiring the closing marker catches that.
if ($Text -notlike '*END CERTIFICATE*') {
    Remove-TmpFile
    Write-Fail 'the download has BEGIN CERTIFICATE but no END CERTIFICATE - it looks truncated'
}

$CertCount = ([regex]::Matches($Text, 'BEGIN CERTIFICATE')).Count

# --- install --------------------------------------------------------------------

# Only now, with a proven-good file in hand, is the destination touched. An
# existing good bundle survives every failure above.
try {
    $FullPath = [System.IO.Path]::GetFullPath($OutPath)
    Move-Item -LiteralPath $script:TmpFile -Destination $FullPath -Force
    $script:TmpFile = $null
}
catch {
    $detail = $_.Exception.Message
    Remove-TmpFile
    Write-Fail ('validated the bundle but could not install it at ' + $OutPath + ': ' + $detail)
}

Write-Host ('fetch-ca: ' + $CertCount + ' certificates (' + $Length + ' bytes) -> ' + $FullPath)
exit 0