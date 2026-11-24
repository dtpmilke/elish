# USB OTG (Host Mode) + Зарядка + Игровые утилиты на Xiaomi Pad 5 Pro (`elish`, SM8250)

Пошаговая инструкция по поднятию рабочего **USB OTG (Host Mode)** (мышки, клавиатуры, флешки, геймпады через USB-C хаб с подачей питания), ускоренной зарядки `elishbat` и сенсорного наэкранного геймпада на Windows 11 ARM64.

---

## 1. Аппаратные особенности `elish` (важно знать заранее)

1. **Порт USB-C на Xiaomi Pad 5 Pro 11" (`elish` / `M2105K81AC`) аппаратно только USB 2.0**:
   - На плате не разведены линии SuperSpeed TX/RX, поэтому **USB 3.0** и **DisplayPort Alt Mode (обычный Type-C → HDMI хаб)** аппаратно не работают ни в Android, ни в Windows.
   - Для вывода на внешний HDMI-монитор нужен либо USB 2.0 → HDMI адаптер на чипе **DisplayLink** (есть официальный драйвер под Windows ARM64), либо передача по Wi-Fi (Spacedesk / Miracast / Sunshine+Moonlight).
2. **Питание 5 В наружу (OTG VBUS Boost)**:
   - На текущей конфигурации PMIC PM8150B не поднимает 5 В на пассивный OTG-переходник автоматически — используйте **USB-C хаб с подачей питания (PD pass-through)** (одновременно питает периферию и заряжает планшет).
3. **Почему раньше USB Host вызывал ребут / не видел устройства**:
   - Фоновый драйвер `elishusb.sys` (device-режим) опрашивал MMIO DWC3 (`0x0A600000`) каждые 200 мс и при сбросе контроллера в Host-режим вызывал мгновенный NoC hang / ребут. Его нужно отключить (`Start = 4`).
   - Штатный `QcXhciFilter8250.sys` из `WOA-Project/windows_silicon_qcom_kona` внутри является переименованным `QcXhciFilter8180.sys` (от SM8150) и вызывает `BSOD CLOCK_WATCHDOG_TIMEOUT` — ставить его **нельзя**.
   - Без настройки `QSCRATCH` (`0x0A6F8800`) контроллер `USBXHCI` видит физическое подключение (`portsc1 = 0x0603`), но зависает в `ConnectionStatus = 9 (DeviceEnumerating)`.
   - Одноразовый драйвер **`elishotg.sys`** применяет два фикса из Linux `dwc3-qcom.c`:
     1. Сбрасывает биты Device VBUS-override (`QSCRATCH_HS_PHY_CTRL` `0x10`, `QSCRATCH_SS_PHY_CTRL` `0x30`).
     2. Переключает `QSCRATCH_GENERAL_CFG` (`0x08`) на UTMI-тактирование (`PIPE_UTMI_CLK_SEL | PIPE3_PHYSTATUS_SW`).
   - После перезапуска `USB\ROOT_HUB30` подключённые USB 2.0 устройства мгновенно энумерируются (`ConnectionStatus = 1`).

---

## 2. Сборка и подпись `elishotg.sys`

В WSL (Ubuntu) с установленным WDK ARM64 (`10.0.28000+`), `clang`, `lld-link` и `osslsigncode`:

```bash
CERT_DIR=~/your-test-cert bash build.sh
```

На выходе получится подписанный `out/elishotg.sys` (~6,4 КБ). На планшете должен быть включён Test Signing (`bcdedit /set testsigning on`) и установлен корневой сертификат в `Trusted Root Certification Authorities`.

---

## 3. Установка на планшет (PowerShell от администратора)

### Шаг 1. Отключить конфликтующий `elishusb.sys`
```powershell
Set-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Services\elishusb" -Name Start -Value 4 -Type DWord
# Оставить безопасный RoleSwitchMode = 3 на время загрузки (чтобы не было конфликта с кабелем ПК)
Set-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Enum\ACPI\QCOM0597\0\Device Parameters" -Name RoleSwitchMode -Value 3 -Type DWord
```
После отключения `elishusb` требуется одна перезагрузка (т.к. старый `elishusb.sys` не имеет `DriverUnload`).

### Шаг 2. Скопировать драйвер и скрипт активации
Скопируйте:
- `elishotg.sys` → `C:\Windows\System32\drivers\elishotg.sys`
- `enable_otg.ps1` → `C:\elish\enable_otg.ps1`
- `touch_gamepad.ps1` → `C:\elish\touch_gamepad.ps1` (опционально — наэкранный сенсорный геймпад)

### Шаг 3. Настроить автозапуск USB OTG при старте Windows
```powershell
$act = New-ScheduledTaskAction -Execute "powershell.exe" -Argument "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\elish\enable_otg.ps1"
$trg = New-ScheduledTaskTrigger -AtStartup
$trg.Delay = "PT15S"
$pr  = New-ScheduledTaskPrincipal -UserId "NT AUTHORITY\SYSTEM" -LogonType ServiceAccount -RunLevel Highest
$cfg = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask -TaskPath "\elish\" -TaskName "AutoUSBOTG" -Action $act -Trigger $trg -Principal $pr -Settings $cfg -Force
```

Опционально — ярлык `USB OTG` на рабочий стол (для ручного перезапуска при смене устройств):
```powershell
$w = New-Object -ComObject WScript.Shell
$s = $w.CreateShortcut("C:\Users\user\Desktop\USB OTG.lnk")
$s.TargetPath = "powershell.exe"
$s.Arguments = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\elish\enable_otg.ps1"
$s.Save()
```

---

## 4. Ускорение зарядки (`elishbat`)

По умолчанию в реестре `elishbat` может стоять ограничение `ChgIclForce = 0x10` (800 мА ≈ 4 Вт). Чтобы поднять до безопасного предела драйвера **1500 мА (~7,5 Вт)** с автоматическим термоконтролем (снижение тока при ≥ 43 °C):

```powershell
$bat = "HKLM:\SYSTEM\CurrentControlSet\Services\elishbat"
Set-ItemProperty -Path $bat -Name "ChgIclForce" -Value 0x1E -Type DWord
Set-ItemProperty -Path $bat -Name "ChgIclMax"   -Value 0x1E -Type DWord
Set-ItemProperty -Path $bat -Name "ChgTempCtl"  -Value 1    -Type DWord
pnputil /restart-device "ROOT\ELISHBAT\0000"
```

---

## 5. Игровой набор и рекомендации для игр на `elish`

1. **Обязательные библиотеки и кодеки** (через `winget` / `dism`):
   - `Microsoft.VCRedist.2015+.x86` / `.x64` / `.arm64` (+ `2005`–`2013` x86)
   - DirectX End-User Runtimes (June 2010)
   - `dism /online /enable-feature /featurename:DirectPlay /all /norestart`
   - `CodecGuide.K-LiteCodecPack.Basic` (DirectShow x86/x64 для роликов)
   - `Nvidia.PhysX` (CPU PhysX), `Microsoft.DotNet.DesktopRuntime.8` (x86/arm64), OpenAL (`oalinst.exe`)
2. **Портретная матрица `1600×2560` и разрешение `1280×800`**:
   - В старых D3D9-играх (например, FlatOut 2) D3D9On12 показывает меньшие режимы перевёрнутыми. Выбирайте в настройках `2560×1600`, либо используйте обёртку `set_res.ps1` (переключает рабочий стол в `1280×800` на время игры и возвращает `2560×1600` при выходе — в 4 раза ниже нагрузка на GPU без чёрных полос).
3. **Наэкранный сенсорный геймпад (`touch_gamepad.ps1`)**:
   - Полноэкранный полупрозрачный оверлей (`WS_EX_NOACTIVATE | WS_EX_TOPMOST`) поверх игр — не забирает фокус у окна игры и шлёт аппаратные Scan Codes (`SendInput` с `KEYEVENTF_SCANCODE`), поэтому работает даже со старыми DirectInput-играми без клавиатуры.
