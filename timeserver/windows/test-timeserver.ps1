# Ask an NTP server for the time and show the offset vs this PC.
#   powershell -ExecutionPolicy Bypass -File test-timeserver.ps1 [-Server 192.168.1.10] [-Port 123]
param([string]$Server = '127.0.0.1', [int]$Port = 123)

$Epoch1900 = (New-Object DateTime 1900, 1, 1, 0, 0, 0, ([DateTimeKind]::Utc))
function Read-NtpTime([byte[]]$b, [int]$off) {
    $s = [byte[]]$b[$off..($off + 3)];       [Array]::Reverse($s)
    $f = [byte[]]$b[($off + 4)..($off + 7)]; [Array]::Reverse($f)
    $sec = [BitConverter]::ToUInt32($s, 0); $frac = [BitConverter]::ToUInt32($f, 0)
    return $Epoch1900.AddTicks([long]$sec * 10000000 + [long]([double]$frac * 10000000 / 4294967296))
}

$udp = New-Object System.Net.Sockets.UdpClient
$udp.Client.ReceiveTimeout = 3000
$req = New-Object byte[] 48
$req[0] = 0x23   # v4, client
try {
    $t1 = [DateTime]::UtcNow
    [void]$udp.Send($req, 48, $Server, $Port)
    $ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
    $resp = $udp.Receive([ref]$ep)
    $t4 = [DateTime]::UtcNow
} catch {
    Write-Host "No answer from ${Server}:$Port (server not running, or a firewall blocks UDP $Port)." -ForegroundColor Red
    exit 1
}
$t2 = Read-NtpTime $resp 32; $t3 = Read-NtpTime $resp 40
$offset = (($t2 - $t1).TotalMilliseconds + ($t3 - $t4).TotalMilliseconds) / 2
$refid = -join ([char[]]$resp[12..15])
Write-Host ("server {0}:{1}  stratum {2}  refid {3}" -f $Server, $Port, $resp[1], $refid) -ForegroundColor Green
Write-Host ("server time: {0} (local)" -f $t3.ToLocalTime().ToString('yyyy-MM-dd HH:mm:ss'))
Write-Host ("offset vs this PC: {0:+0.0;-0.0} ms" -f $offset)
