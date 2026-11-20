# Windows на Xiaomi Pad 5 Pro (elish)

[English](README.md)

В репозитории собраны исходники и патчи для elish. Для воспроизведения нужны оригинальные файлы из вашей копии прошивки или пакета драйверов. Для скриптов достаточно Python 3 со стандартной библиотекой. Запускайте команды из корня репозитория, кроме команд для DSDT и UEFI, для которых ниже указан рабочий каталог.

## DSDT и Bluetooth UART

За основу взята Qualcomm DSDT для платы 8250_MTP. Чтобы получить DSDT из дерева порта, скопируйте эталонный файл `8250/builtin/DSDT_8250_MTP.aml` в отдельную папку под именем `DSDT.aml`. В этой папке примените [патч](acpi/elish-dsdt.patch).

```sh
git apply /path/to/elish/acpi/elish-dsdt.patch
```

| DSDT | SHA-256 |
| --- | --- |
| Исходная 8250_MTP | `8c181ad8dd93e37fbc08d2ecc6b7146781375213ba12683d819f18c59feb751a` |
| После применения патча | `2c3c823a010ee6d93fa49b90658bf354d3d01c9b61c47fbb2e29f19327536125` |

В экспериментальных загрузочных образах использовались и более поздние варианты. Для тех, кто пишет свой PEP, в [исходнике SSDT](acpi/elish-SSDT_BT.asl) описаны тактирование и выводы UART для Bluetooth на elish. Устройство UR18 использует QUP0 SE6 и GPIO 16–19. При переносе на Pad 6 (pipa) сверьте контроллер, прерывание и выводы с Device Tree своей платы.

## Bluetooth QCA6390

На тестовом elish радио появилось в Windows с исправлением UART ×16, адаптированным HCI-драйвером и NVM на 3 Мбит/с. Чтобы воспроизвести эти три файла, выполните команды ниже.

```sh
python bluetooth/patch_bluetooth.py uart /path/to/qcuart8250.sys qcuart8250.patched.sys
python bluetooth/patch_bluetooth.py hci /path/to/qcbtfmuart8250.sys qcbtfmuart8250.patched.sys
python bluetooth/patch_bluetooth.py nvm /path/to/htnv20.bin crnv20.bin
```

В проверенной конфигурации прошивка elish `htbtfw20.tlv` загружалась под именем `crbtfw20.tlv`. Параметры драйвера были `BaudRate=3000000`, `SWInBandSleepEnabled=1`, `ExplicitRtsWakeup=0`, `IsRome=1` и `DoNotEnterD3=1`. Для установки нужен подписанный пакет с подходящими INF/CAT. На текущем образе ещё предстоит повторно проверить радио, сон и пробуждение. [Контрольные суммы файлов](bluetooth/README.md)

## Wi-Fi

Патчер принимает оригинальный `qcwlan6800sc81.sys` версии `1.0.1760.20`, проверяет хеши и создаёт отдельный неподписанный файл. Для установки подпишите драйвер в составе соответствующего пакета. На elish подключение работало, иногда требовалась повторная загрузка прошивки.

```sh
python wifi/patch_qcwlan.py --orig /path/to/original/qcwlan6800sc81.sys --out qcwlan6800sc81.unsigned.sys
```

## UEFI

Патч исправляет размер выходных буферов для `GetChipFamily` и `GetModemSupport`. В проверенной сборке Windows прочитала из DSDT значение `SOID = 0x164`. Выполните команды из корня [Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium). Проверка проводилась на коммите `4bd8ae83bf37de41777f19e0777db70f59383a6d`.

```sh
git apply --check /path/to/elish/uefi/0001-fix-chipinfo-output-width.patch
git apply /path/to/elish/uefi/0001-fix-chipinfo-output-width.patch
```

## Дисплей K81 и GPU Adreno 650

Генератор переносит тайминги и команды DSI из исходного XML панели и добавляет идентификатор, поля EDID и физические размеры:

```sh
python display/gen_panel_xml.py /path/to/Panel_K81_42_02_0a_dualdsi_cphy_lcd_vid.xml elish_k81_42_02_0a.xml
```

### Патч GPU Adreno 650 (SM8250-AC / LPDDR5)

Оригинальный драйвер Qualcomm `qcdxkm8250.sys` (WDDM 2.7) собран для Adreno 640 и LPDDR4X. Для запуска Adreno 650 с LPDDR5 на Snapdragon 870 скрипт устраняет две аппаратные проблемы: холодный старт GMU/PDC (TCM retention `+0x1e730` и PDC master bit 31) и хеширование каналов памяти LPDDR5 (`DDRHighestBankBit = 15` вместо 16):

```sh
python display/patch_adreno650.py /path/to/stock/qcdxkm8250.sys qcdxkm8250.patched.sys
```

После применения патча драйвер подписывается Authenticode и устанавливается в DriverStore. В реестре настраиваются параметры `HKLM\SOFTWARE\QCOM\Drivers\DxKMD` (`DDRHighestBankBit=15`, `UBWCEnable=1`, `GmemSize=0x120000`). Подробнее в [display/README.md](display/README.md).


## Тачскрин Novatek NT36523

Драйвер KMDF HID работает через SPI на GPIO 8–11. На планшете ввод проверен с предыдущей подписанной сборкой. Для новой сборки на Windows добавьте в PATH Python 3, clang, lld-link, llvm-ar и llvm-objcopy. Понадобятся ARM64 WDK, Windows SDK с заголовками версии `10.0.28000.0` и KMDF `1.17`. В папку прошивок положите `novatek_nt36523_k81a_fw01.bin` и `novatek_nt36523_k81a_fw02.bin` из Android elish, раздел vendor, каталог firmware.

```powershell
python touch/build.py --wdk "C:\path\to\wdk\c" --sdk "C:\path\to\sdk\c" --firmware-dir "C:\path\to\firmware" --out touch/out
```

Сборка создаёт неподписанный `touch/out/nvtbb.sys` со встроенной прошивкой и копирует INF. Для установки нужны устройство `ROOT\NVTBB` и подписанный пакет. Новая сборка проверена офлайн. [Хеши прошивок, соответствие панелям и происхождение кода](touch/README.md)
