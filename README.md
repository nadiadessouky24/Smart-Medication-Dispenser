# Smart Medication Dispenser

STM32L4 firmware for an automated pill dispenser. Uses FreeRTOS (CMSIS-V2) with all application logic in `StartDefaultTask`.

## Hardware

- STM32L4 board (Nucleo)
- HC-05 Bluetooth module on USART1
- I2C LCD (addr `0x27`) on I2C1
- SG90 servo on TIM2 CH1 (PWM)
- ST-Link / USART2 for debug output

## Build & Flash

1. Open the project in **STM32CubeIDE** (or Keil MDK).
2. Build (`Ctrl+B`) → Flash (`F11` / Load).
3. Open a serial terminal on the ST-Link COM port at **115200 8N1** to see boot logs.

## Running

On boot the firmware:

1. Initializes I2C, UART, TIM2 PWM, RTC.
2. Starts the FreeRTOS scheduler — `defaultTask` runs the main loop.
3. Listens on Bluetooth for dose schedule commands.
4. Checks RTC every second; rotates the servo to the next compartment when an alarm time matches.

## Bluetooth Commands

Send via Arduino Blue in the format "SET HH:MM"
