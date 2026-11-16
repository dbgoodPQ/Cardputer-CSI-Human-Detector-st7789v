# LAN time server: tiny NTP server (UDP 123) serving this PC's clock.
# Installed by install.ps1 as a startup task (runs as SYSTEM). No dependencies.
# Manual run (admin):  powershell -ExecutionPolicy Bypass -File ntp-server.ps1 [-Port 123]
param([int]$Port = 123)

$ErrorActionPreference = 'Stop'
$LogDir  = Join-Path $env:ProgramData 'LanTimeServer'
$LogFile = Join-Path $LogDir 'server.log'
New-Item -ItemType Directory -Force $LogDir | Out-Null

function Write-Log([string]$msg) {
    try {
        if ((Test-Path $LogFile) -and (Get-Item $LogFile).Length -gt 1MB) {
            Move-Item -Force $LogFile "$LogFile.old"
        }
        Add-Content -Path $LogFile -Value ("{0}  {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $msg)
    } catch {}
}

$Epoch1900 = (New-Object DateTime 1900, 1, 1, 0, 0, 0, ([DateTimeKind]::Utc)).Ticks

function Get-NtpBytes([DateTime]$utc) {
    $ticks = $utc.Ticks - $Epoch1900
    $sec   = [uint32]([math]::Floor($ticks / 10000000) % 4294967296)
    $frac  = [uint32]([math]::Floor(($ticks % 10000000) * 4294967296 / 10000000))
    $a = [BitConverter]::GetBytes($sec);  [Array]::Reverse($a)
    $b = [BitConverter]::GetBytes($frac); [Array]::Reverse($b)
    return [byte[]]($a + $b)
}

$udp = New-Object System.Net.Sockets.UdpClient($Port)
# Don't let an ICMP "port unreachable" from a gone client break Receive() (WSAECONNRESET).
try { [void]$udp.Client.IOControl(-1744830452, [byte[]](0, 0, 0, 0), $null) } catch {}
Write-Log "started on UDP $Port, clock now $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') local"

$lastLog = @{}
$remote = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
while ($true) {
    try {
        $req = $udp.Receive([ref]$remote)
        $rx  = [DateTime]::UtcNow
        if ($req.Length -lt 48 -or ($req[0] -band 7) -ne 3) { continue }   # client requests only

        $vn = ($req[0] -shr 3) -band 7
        if ($vn -lt 1 -or $vn -gt 4) { $vn = 4 }
        $resp = New-Object byte[] 48
        $resp[0] = [byte](($vn -shl 3) -bor 4)   # LI 0, mode 4 (server)
        $resp[1] = 2                              # stratum 2 (0 = "kiss of death", rejected by clients)
        $resp[2] = $req[2]                        # poll
        $resp[3] = 0xEC                           # precision -20
        $resp[10] = 0x02; $resp[11] = 0x90        # root dispersion ~10 ms
        $resp[12] = 0x4C; $resp[13] = 0x4F; $resp[14] = 0x43; $resp[15] = 0x4C   # refid "LOCL"
        $now = [DateTime]::UtcNow
        [Array]::Copy((Get-NtpBytes $now.AddSeconds(-($now.Second))), 0, $resp, 16, 8)  # reference
        [Array]::Copy($req, 40, $resp, 24, 8)                                             # originate
        [Array]::Copy((Get-NtpBytes $rx), 0, $resp, 32, 8)                                # receive
        [Array]::Copy((Get-NtpBytes ([DateTime]::UtcNow)), 0, $resp, 40, 8)               # transmit
        [void]$udp.Send($resp, 48, $remote)

        $ip = $remote.Address.ToString()
        if (-not $lastLog.ContainsKey($ip) -or ($rx - $lastLog[$ip]).TotalHours -ge 1) {
            $lastLog[$ip] = $rx
            Write-Log "served $ip"
        }
    } catch {
        Write-Log "error: $($_.Exception.Message)"
        Start-Sleep -Milliseconds 100
    }
}
