$ErrorActionPreference = 'Stop'

$fsSectors = 64
$fsBytes = $fsSectors * 512
$maxEntries = 32
$headerSize = 64
$entrySize = 64
$dataOffset = 4096
$dataSize = $fsBytes - $dataOffset

[byte[]]$buf = New-Object byte[] $fsBytes

function Write-U32([byte[]]$arr, [int]$off, [uint32]$v) {
    [byte[]]$tmp = [System.BitConverter]::GetBytes($v)
    [Array]::Copy($tmp, 0, $arr, $off, 4)
}

function Write-Ascii([byte[]]$arr, [int]$off, [int]$len, [string]$text) {
    [byte[]]$tmp = [System.Text.Encoding]::ASCII.GetBytes($text)
    $n = [Math]::Min($len, $tmp.Length)
    [Array]::Copy($tmp, 0, $arr, $off, $n)
}

Write-Ascii $buf 0 4 'NXFS'
Write-U32 $buf 4 1
Write-U32 $buf 8 $maxEntries
Write-U32 $buf 12 $dataOffset
Write-U32 $buf 16 $dataSize
Write-U32 $buf 20 0
Write-U32 $buf 24 0
Write-U32 $buf 28 0

$entryIndex = 0
$usedBytes = 0

function Add-Entry([string]$name, [byte]$type, [byte]$parent, [string]$text) {
    if ($script:entryIndex -ge $maxEntries) { throw 'No free entries left in NXFS image' }

    $entryOff = $headerSize + ($script:entryIndex * $entrySize)
    Write-Ascii $buf $entryOff 32 $name

    if ($type -eq 1) {
        [byte[]]$payload = [System.Text.Encoding]::ASCII.GetBytes($text)
        if ($script:usedBytes + $payload.Length -gt $dataSize) {
            throw "Not enough data area for file $name"
        }
        Write-U32 $buf ($entryOff + 32) $script:usedBytes
        Write-U32 $buf ($entryOff + 36) $payload.Length
        [Array]::Copy($payload, 0, $buf, $dataOffset + $script:usedBytes, $payload.Length)
        $script:usedBytes += $payload.Length
    } else {
        Write-U32 $buf ($entryOff + 32) 0
        Write-U32 $buf ($entryOff + 36) 0
    }

    $buf[$entryOff + 40] = 1      # used
    $buf[$entryOff + 41] = $type  # type: 1 file, 2 dir
    $buf[$entryOff + 42] = if ($type -eq 2) { 63 } else { 54 }   # perms 077 or 066
    $buf[$entryOff + 43] = 0      # owner uid
    $buf[$entryOff + 44] = $parent

    $idx = $script:entryIndex
    $script:entryIndex++
    return [byte]$idx
}

$systemDir = Add-Entry -name 'system' -type 2 -parent 255 -text ''
$modulesDir = Add-Entry -name 'modules' -type 2 -parent $systemDir -text ''
$appsDir = Add-Entry -name 'apps' -type 2 -parent $systemDir -text ''

Add-Entry -name 'readme.txt' -type 1 -parent 255 -text "Welcome to NoxisFS.`nUse help to see shell commands.`n" | Out-Null
Add-Entry -name 'notes.txt' -type 1 -parent 255 -text "This is a writable custom filesystem.`nTry: write notes.txt hello`n" | Out-Null
Add-Entry -name 'hello.mod' -type 1 -parent $modulesDir -text "command=hello`nmessage=Hello from /system/modules module!`n" | Out-Null
Add-Entry -name 'hello.nxapp' -type 1 -parent $appsDir -text "NXAPP-1`nname=Hello App`nversion=0.1`n---`necho Running hello.nxapp`nhello`necho passed args: `$ARGS`n" | Out-Null

Write-U32 $buf 20 $entryIndex
Write-U32 $buf 24 $usedBytes

[System.IO.File]::WriteAllBytes('fs.bin', $buf)
Write-Host "Created fs.bin ($fsBytes bytes), entries=$entryIndex, used=$usedBytes"
