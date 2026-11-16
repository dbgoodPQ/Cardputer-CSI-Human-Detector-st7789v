# Removes the LAN time server and gives Windows Time back. Run uninstall.cmd.
$Task = 'LAN Time Server'
Get-ScheduledTask -TaskName $Task -ErrorAction SilentlyContinue | Stop-ScheduledTask -ErrorAction SilentlyContinue
Unregister-ScheduledTask -TaskName $Task -Confirm:$false -ErrorAction SilentlyContinue
Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" |
    Where-Object { $_.CommandLine -like '*LanTimeServer\ntp-server.ps1*' } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Get-NetFirewallRule -DisplayName 'LAN Time Server (NTP UDP 123)' -ErrorAction SilentlyContinue | Remove-NetFirewallRule
Set-Service w32time -StartupType Manual -ErrorAction SilentlyContinue
Start-Service w32time -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force (Join-Path $env:ProgramData 'LanTimeServer') -ErrorAction SilentlyContinue
Write-Host 'LAN time server removed, Windows Time service restored.'
