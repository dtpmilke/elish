# Enable USB Host (OTG) on Xiaomi Pad 5 Pro (elish, SM8250) on Windows 11 ARM64.
# Safely switches ACPI\QCOM0597\0 to Host mode, runs one-shot elishotg.sys (QSCRATCH UTMI_CLK),
# resets RoleSwitchMode=3 in registry as a boot safety net, and restarts USB Root Hub 3.0.
$ErrorActionPreference = 'SilentlyContinue'

$prmKey = "HKLM:\SYSTEM\CurrentControlSet\Enum\ACPI\QCOM0597\0\Device Parameters"
Set-ItemProperty -Path $prmKey -Name RoleSwitchMode -Value 1 -Type DWord
& pnputil /restart-device "ACPI\QCOM0597\0" | Out-Null

# Restore RoleSwitchMode=3 in registry immediately so next boot with a PC cable never conflicts
Set-ItemProperty -Path $prmKey -Name RoleSwitchMode -Value 3 -Type DWord
Start-Sleep -Seconds 2

# Run one-shot elishotg.sys to clear Device VBUS override & select UTMI clock in QSCRATCH
sc.exe create elishotg type= kernel start= demand binPath= C:\Windows\System32\drivers\elishotg.sys | Out-Null
sc.exe start elishotg | Out-Null
Start-Sleep -Milliseconds 500
sc.exe stop elishotg | Out-Null
Start-Sleep -Milliseconds 500

# Restart USB Root Hub 3.0 so attached USB devices enumerate cleanly
$hub = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'USB\ROOT_HUB30\*' } | Select-Object -First 1
if ($hub) {
    & pnputil /restart-device $hub.InstanceId | Out-Null
}
