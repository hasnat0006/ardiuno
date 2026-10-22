# Runs tools/pc_link.py in the background every time you log in to Windows.
#
#   Install:    powershell -ExecutionPolicy Bypass -File tools\install_autostart.ps1
#   Uninstall:  powershell -ExecutionPolicy Bypass -File tools\install_autostart.ps1 -Uninstall
#
# It creates a Task Scheduler task called "OLED PC Link" for your user only
# (no admin rights needed). Stop / start it by hand with:
#   schtasks /End /TN "OLED PC Link"
#   schtasks /Run /TN "OLED PC Link"

param([switch]$Uninstall)

$TaskName = "OLED PC Link"

if ($Uninstall) {
    Stop-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
    Write-Output "Removed '$TaskName'."
    return
}

$script = Join-Path $PSScriptRoot "pc_link.py"
$python = (Get-Command python -ErrorAction Stop).Source
$pythonw = Join-Path (Split-Path $python) "pythonw.exe"  # same Python, but no console window
if (-not (Test-Path $pythonw)) { throw "pythonw.exe not found next to $python" }

$action = New-ScheduledTaskAction -Execute $pythonw -Argument "`"$script`"" -WorkingDirectory $PSScriptRoot
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
$settings = New-ScheduledTaskSettingsSet `
    -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit ([TimeSpan]::Zero) `
    -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1) `
    -MultipleInstances IgnoreNew

Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger -Settings $settings `
    -Description "Sends time, Claude session status and usage to the ESP32 OLED over WiFi." -Force | Out-Null
Start-ScheduledTask -TaskName $TaskName

Write-Output "Installed '$TaskName': it is running now and will start at every login."
Write-Output "Log file: $(Join-Path $PSScriptRoot 'pc_link.log')"
