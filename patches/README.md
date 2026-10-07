# Kernel patches для Orange Pi 4 Pro (Allwinner A733)

Патчи ядра для повышения скорости eMMC и исправления CQE-бага.

## Патчи

### 0001-cqhci-halt-trick-ret-true.patch

**Файл:** drivers/mmc/host/cqhci-core.c

**Проблема:** при высокой частоте eMMC (HS400) CQE не может остановиться (Failed to halt), драйвер отключает CQE и сбрасывает частоту до 50 МГц.

**Решение:**
1. Polling halt с явным таймаутом (вместо wait_event_timeout).
2. Очистка CQHCI_CTL при неудачном halt.
3. ret = true — обман mmc-core, чтобы не запускался recovery.

**Результат:** CQE остаётся включённым, частота не сбрасывается.

### 0002-sun60iw2p1-mmc-v5p3x-150mhz.patch

**Файл:** arch/arm64/boot/dts/allwinner/sun60iw2p1.dtsi

**Изменения:**
- compatible = "allwinner,sunxi-mmc-v5p3x" (правильный драйвер для A733).
- max-frequency = <150000000> (150 МГц, HS400).
- sunxi-dly-208M = <0xff 0x01 0xff 0xff 0xff 0xff> (delay-настройки).

**Результат:** скорость чтения eMMC ~205 МБ/с (было ~80).

## Как применить

Патчи применяются к исходникам ядра (/root/linux-kernel-src) перед сборкой:

    cd /root/linux-kernel-src
    patch -p1 < /root/ugv_rpi/patches/kernel/0001-cqhci-halt-trick-ret-true.patch
    patch -p1 < /root/ugv_rpi/patches/kernel/0002-sun60iw2p1-mmc-v5p3x-150mhz.patch

Затем — сборка ядра:

    make -j8 Image modules dtbs
