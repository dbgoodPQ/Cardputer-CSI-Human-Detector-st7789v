# LAN time server installer for Windows (offline, nothing downloaded). Run install.cmd.
$ErrorActionPreference = 'Stop'
$Here    = Split-Path -Parent $MyInvocation.MyCommand.Path
$Dest    = Join-Path $env:ProgramData 'LanTimeServer'
$Task    = 'LAN Time Server'
$FwRule  = 'LAN Time Server (NTP UDP 123)'

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) { Write-Host 'Run install.cmd (it asks for administrator rights).' -ForegroundColor Red; exit 1 }

Write-Host '1/5 Copying files to' $Dest
New-Item -ItemType Directory -Force $Dest | Out-Null
Copy-Item -Force (Join-Path $Here 'ntp-server.ps1'), (Join-Path $Here 'test-timeserver.ps1') $Dest

Write-Host '2/5 Stopping the Windows Time service (it holds UDP 123 and has no internet source here)'
Get-ScheduledTask -TaskName $Task -ErrorAction SilentlyContinue | Stop-ScheduledTask -ErrorAction SilentlyContinue
Stop-Service w32time -Force -ErrorAction SilentlyContinue
Set-Service w32time -StartupType Disabled -ErrorAction SilentlyContinue

Write-Host '3/5 Opening the firewall for UDP 123 (inbound)'
Get-NetFirewallRule -DisplayName $FwRule -ErrorAction SilentlyContinue | Remove-NetFirewallRule
New-NetFirewallRule -DisplayName $FwRule -Direction Inbound -Protocol UDP -LocalPort 123 `
    -Action Allow -Profile Any | Out-Null

Write-Host '4/5 Creating the startup task' $Task
Unregister-ScheduledTask -TaskName $Task -Confirm:$false -ErrorAction SilentlyContinue
$action    = New-ScheduledTaskAction -Execute 'powershell.exe' `
    -Argument "-NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$Dest\ntp-server.ps1`""
$trigger   = New-ScheduledTaskTrigger -AtStartup
$principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
$settings  = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 999 `
    -RestartInterval (New-TimeSpan -Minutes 1) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable
Register-ScheduledTask -TaskName $Task -Action $action -Trigger $trigger -Principal $principal `
    -Settings $settings -Description 'NTP server on UDP 123 serving this PC clock to the LAN' | Out-Null
Start-ScheduledTask -TaskName $Task

Write-Host '5/5 Self test'
Start-Sleep -Seconds 3
& (Join-Path $Dest 'test-timeserver.ps1') -Server 127.0.0.1

Write-Host ''
Write-Host 'Give clients this server address:' -ForegroundColor Cyan
Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' } |
    ForEach-Object { Write-Host "   $($_.IPAddress)   ($($_.InterfaceAlias))" }
Write-Host ''
Write-Host "This PC's clock is the reference: check it is right (Settings > Time & language > set manually)."
Write-Host "Log: $Dest\server.log"
