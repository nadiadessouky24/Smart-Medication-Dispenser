/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Smart Medication Dispenser - Main program body
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <string.h>
#include <stdio.h>
/* USER CODE END Includes */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
// DS3231 RTC
#define RTC_ADDR        (0x68 << 1)
#define RTC_TIME_REG    0x00

// LCD I2C (address confirmed 0x27)
#define LCD_ADDR        (0x27 << 1)
#define LCD_BACKLIGHT   0x08
#define LCD_ENABLE      0x04
#define LCD_RS          0x01

#define LCD_CMD_CLEAR       0x01
#define LCD_CMD_HOME        0x02
#define LCD_CMD_ENTRY_MODE  0x06
#define LCD_CMD_DISPLAY_ON  0x0C
#define LCD_CMD_FUNCTION    0x28
#define LCD_CMD_SET_DDRAM   0x80
#define LCD_ROW0_OFFSET     0x00
#define LCD_ROW1_OFFSET     0x40

// Servo positions (pulse width in microseconds)
// NARROW-RANGE CALIBRATED VALUES for this specific SG90 (range 500-1050us)
#define SERVO_COMP_0    500
#define SERVO_COMP_1    683
#define SERVO_COMP_2    866
#define SERVO_COMP_3    1050

// IR sensor threshold — LOW = pill present (TCRT5000 analog output)
// Calibrate by checking "IR on boot" value in TeraTerm
#define IR_THRESHOLD    2000

// Timeouts
#define ACK_TIMEOUT_MS        30000
#define ACK_AFTER_SWITCH_MS   30000

#define MAX_DOSES       4

// Flash storage — last page of STM32L432KC flash (page 127)
#define FLASH_SCHEDULE_ADDR  0x0803F800
#define FLASH_MAGIC          0xDEADBEEF
/* USER CODE END PD */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;
I2C_HandleTypeDef hi2c1;
TIM_HandleTypeDef htim2;
UART_HandleTypeDef huart1;
UART_HandleTypeDef huart2;

osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};

/* USER CODE BEGIN PV */
typedef struct {
    uint8_t hour;
    uint8_t minute;
    uint8_t active;
} DoseTime_t;

volatile DoseTime_t doseSchedule[MAX_DOSES] = {0};
volatile uint8_t numDoses = 0;

char btCmdBuf[64];
volatile uint8_t btCmdIdx = 0;

uint8_t doseTriggeredThisMinute = 0;

// Tracks which compartment the servo will rotate to next
volatile uint8_t currentCompartment = 0;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_I2C1_Init(void);
static void MX_ADC1_Init(void);
static void MX_TIM2_Init(void);
static void MX_USART1_UART_Init(void);
void StartDefaultTask(void *argument);

/* USER CODE BEGIN PFP */
void UART2_Print(const char *msg);
void I2C_Scan(void);
void LCD_SendNibble(uint8_t nibble, uint8_t mode);
void LCD_SendByte(uint8_t byte, uint8_t mode);
void LCD_SendCommand(uint8_t cmd);
void LCD_SendData(uint8_t data);
void LCD_Init(void);
void LCD_Clear(void);
void LCD_SetCursor(uint8_t row, uint8_t col);
void LCD_Print(const char *str);
void LCD_PrintLine(uint8_t row, const char *str);
void Servo_SetCompartment(uint8_t compartment);
void Servo_RotateToNext(void);
uint16_t IR_Read(void);
uint8_t Pill_Present(void);
void Buzzer_On(void);
void Buzzer_Off(void);
uint8_t Switch_IsLow(void);
void RTC_SetTime(uint8_t hours, uint8_t minutes, uint8_t seconds);
void RTC_GetTime(uint8_t *hours, uint8_t *minutes, uint8_t *seconds);
void Flash_SaveSchedule(void);
void Flash_LoadSchedule(void);
void AdvanceToNextDose(void);
void HandleDoseCycle(void);
void BT_ProcessCommand(char *cmd);
/* USER CODE END PFP */

/* USER CODE BEGIN 0 */

// ============================================================
// UART2 Helper
// ============================================================
void UART2_Print(const char *msg) {
    HAL_UART_Transmit(&huart2, (uint8_t *)msg, strlen(msg), HAL_MAX_DELAY);
}

// ============================================================
// I2C Scanner
// ============================================================
void I2C_Scan(void) {
    char buf[48];
    uint8_t found = 0;
    UART2_Print("Scanning I2C bus...\r\n");
    for (uint8_t addr = 1; addr < 128; addr++) {
        if (HAL_I2C_IsDeviceReady(&hi2c1, addr << 1, 1, 10) == HAL_OK) {
            snprintf(buf, sizeof(buf), "  Found: 0x%02X", addr);
            UART2_Print(buf);
            if (addr == 0x27 || addr == 0x3F) UART2_Print(" (LCD)");
            else if (addr == 0x68)             UART2_Print(" (DS3231 RTC)");
            UART2_Print("\r\n");
            found++;
        }
    }
    if (!found) UART2_Print("  No devices found!\r\n");
    else {
        snprintf(buf, sizeof(buf), "  Total: %d device(s)\r\n", found);
        UART2_Print(buf);
    }
}

// ============================================================
// LCD Driver (PCF8574 I2C backpack, 4-bit mode)
// ============================================================
void LCD_SendNibble(uint8_t nibble, uint8_t mode) {
    uint8_t data = nibble | mode | LCD_BACKLIGHT;
    uint8_t hi = data | LCD_ENABLE;
    uint8_t lo = data & ~LCD_ENABLE;
    HAL_I2C_Master_Transmit(&hi2c1, LCD_ADDR, &hi, 1, HAL_MAX_DELAY);
    HAL_Delay(1);
    HAL_I2C_Master_Transmit(&hi2c1, LCD_ADDR, &lo, 1, HAL_MAX_DELAY);
    HAL_Delay(1);
}

void LCD_SendByte(uint8_t byte, uint8_t mode) {
    LCD_SendNibble(byte & 0xF0, mode);
    LCD_SendNibble((byte << 4) & 0xF0, mode);
}

void LCD_SendCommand(uint8_t cmd) { LCD_SendByte(cmd, 0); }
void LCD_SendData(uint8_t data)   { LCD_SendByte(data, LCD_RS); }

void LCD_Init(void) {
    HAL_Delay(50);
    LCD_SendNibble(0x30, 0); HAL_Delay(5);
    LCD_SendNibble(0x30, 0); HAL_Delay(5);
    LCD_SendNibble(0x30, 0); HAL_Delay(1);
    LCD_SendNibble(0x20, 0); HAL_Delay(1);
    LCD_SendCommand(LCD_CMD_FUNCTION);   HAL_Delay(1);
    LCD_SendCommand(LCD_CMD_DISPLAY_ON); HAL_Delay(1);
    LCD_SendCommand(LCD_CMD_CLEAR);      HAL_Delay(2);
    LCD_SendCommand(LCD_CMD_ENTRY_MODE); HAL_Delay(1);
    UART2_Print("LCD initialized.\r\n");
}

void LCD_Clear(void) { LCD_SendCommand(LCD_CMD_CLEAR); HAL_Delay(2); }

void LCD_SetCursor(uint8_t row, uint8_t col) {
    LCD_SendCommand(LCD_CMD_SET_DDRAM | ((row ? LCD_ROW1_OFFSET : LCD_ROW0_OFFSET) + col));
}

void LCD_Print(const char *str) {
    while (*str) LCD_SendData((uint8_t)*str++);
}

void LCD_PrintLine(uint8_t row, const char *str) {
    LCD_SetCursor(row, 0);
    char line[17];
    snprintf(line, sizeof(line), "%-16s", str);
    LCD_Print(line);
}

// ============================================================
// Buzzer (PA4 — active buzzer, GPIO HIGH = on)
// ============================================================
void Buzzer_On(void)  { HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET); }
void Buzzer_Off(void) { HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET); }

// ============================================================
// Switch (PA8 — SPDT, LOW = switched toward GND)
// ============================================================
uint8_t Switch_IsLow(void) {
    return (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_8) == GPIO_PIN_RESET) ? 1 : 0;
}

// ============================================================
// Servo (TIM2 CH1 on PA5)
// ============================================================
void Servo_SetCompartment(uint8_t comp) {
    uint16_t pulse[] = {SERVO_COMP_0, SERVO_COMP_1, SERVO_COMP_2, SERVO_COMP_3};
    if (comp > 3) comp = 0;
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, pulse[comp]);
    HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_1);
}

// ============================================================
// Servo rotate to next compartment — uses stop/restart pattern
// to force PWM output reload inside FreeRTOS task context.
// Uses narrow-range calibrated pulses (500-1050us) for this servo.
// ============================================================
void Servo_RotateToNext(void) {
    uint16_t pulses[4] = {SERVO_COMP_0, SERVO_COMP_1, SERVO_COMP_2, SERVO_COMP_3};

    char buf[48];
    snprintf(buf, sizeof(buf), "Servo -> compartment %d (pulse=%u)\r\n",
             currentCompartment, pulses[currentCompartment]);
    UART2_Print(buf);

    HAL_TIM_PWM_Stop(&htim2, TIM_CHANNEL_1);
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, pulses[currentCompartment]);
    HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_1);
    osDelay(1500);

    currentCompartment = (currentCompartment + 1) % 4;
}

// ============================================================
// IR Sensor (ADC1 CH8 on PA3)
// TCRT5000: LOW analog output = pill present (green LED on)
// ============================================================
uint16_t IR_Read(void) {
    HAL_ADC_Start(&hadc1);
    HAL_ADC_PollForConversion(&hadc1, HAL_MAX_DELAY);
    uint16_t val = HAL_ADC_GetValue(&hadc1);
    HAL_ADC_Stop(&hadc1);
    return val;
}

uint8_t Pill_Present(void) {
    return (IR_Read() < IR_THRESHOLD) ? 1 : 0;  // LOW = pill present
}

// ============================================================
// RTC (DS3231 over I2C)
// ============================================================
void RTC_SetTime(uint8_t hours, uint8_t minutes, uint8_t seconds) {
    uint8_t data[3];
    data[0] = ((seconds / 10) << 4) | (seconds % 10);
    data[1] = ((minutes / 10) << 4) | (minutes % 10);
    data[2] = ((hours   / 10) << 4) | (hours   % 10);
    HAL_I2C_Mem_Write(&hi2c1, RTC_ADDR, RTC_TIME_REG,
                      I2C_MEMADD_SIZE_8BIT, data, 3, HAL_MAX_DELAY);
    UART2_Print("RTC time set.\r\n");
}

void RTC_GetTime(uint8_t *hours, uint8_t *minutes, uint8_t *seconds) {
    uint8_t d[3] = {0};
    HAL_I2C_Mem_Read(&hi2c1, RTC_ADDR, RTC_TIME_REG,
                     I2C_MEMADD_SIZE_8BIT, d, 3, HAL_MAX_DELAY);
    *seconds = (d[0] & 0x0F) + ((d[0] >> 4) * 10);
    *minutes = (d[1] & 0x0F) + ((d[1] >> 4) * 10);
    *hours   = (d[2] & 0x0F) + (((d[2] >> 4) & 0x03) * 10);
}

// ============================================================
// Flash storage — save/load dose schedule to last flash page
// STM32L432KC: page 127 at 0x0803F800
// ============================================================
void Flash_SaveSchedule(void) {
    HAL_FLASH_Unlock();

    // Erase page 127
    FLASH_EraseInitTypeDef eraseInit;
    uint32_t pageError;
    eraseInit.TypeErase = FLASH_TYPEERASE_PAGES;
    eraseInit.Page = 127;
    eraseInit.NbPages = 1;
    HAL_FLASHEx_Erase(&eraseInit, &pageError);

    // Write magic number (validates data on next boot)
    uint64_t magic = FLASH_MAGIC;
    HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD,
                      FLASH_SCHEDULE_ADDR, magic);

    // Write number of doses
    uint64_t nDoses = numDoses;
    HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD,
                      FLASH_SCHEDULE_ADDR + 8, nDoses);

    // Write each dose (hour and minute packed into 64 bits)
    for (int i = 0; i < numDoses; i++) {
        uint64_t dose = ((uint64_t)doseSchedule[i].hour << 8) |
                         (uint64_t)doseSchedule[i].minute;
        HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD,
                          FLASH_SCHEDULE_ADDR + 16 + (i * 8), dose);
    }

    HAL_FLASH_Lock();
    UART2_Print("Schedule saved to flash.\r\n");
}

void Flash_LoadSchedule(void) {
    // Check magic number
    uint32_t magic = *(uint32_t *)FLASH_SCHEDULE_ADDR;

    if (magic != FLASH_MAGIC) {
        UART2_Print("No saved schedule in flash.\r\n");
        return;
    }

    // Read number of doses
    uint32_t n = *(uint32_t *)(FLASH_SCHEDULE_ADDR + 8);
    if (n > MAX_DOSES) {
        UART2_Print("Invalid flash data — skipping.\r\n");
        return;
    }
    numDoses = n;

    // Read each dose
    for (int i = 0; i < numDoses; i++) {
        uint64_t dose = *(uint64_t *)(FLASH_SCHEDULE_ADDR + 16 + (i * 8));
        doseSchedule[i].hour   = (dose >> 8) & 0xFF;
        doseSchedule[i].minute = dose & 0xFF;
        doseSchedule[i].active = 1;
    }

    char buf[48];
    snprintf(buf, sizeof(buf), "Loaded %d dose(s) from flash:\r\n", numDoses);
    UART2_Print(buf);
    for (int i = 0; i < numDoses; i++) {
        snprintf(buf, sizeof(buf), "  Dose %d: %02d:%02d\r\n",
                 i+1, doseSchedule[i].hour, doseSchedule[i].minute);
        UART2_Print(buf);
    }
}

// ============================================================
// Advance to next dose
// ============================================================
void AdvanceToNextDose(void) {
    if (numDoses > 0) {
        for (int i = 0; i < numDoses - 1; i++) {
            doseSchedule[i] = doseSchedule[i + 1];
        }
        numDoses--;

        // Save updated schedule to flash
        Flash_SaveSchedule();

        if (numDoses > 0) {
            char buf[40];
            snprintf(buf, sizeof(buf), "Next dose: %02d:%02d\r\n",
                     doseSchedule[0].hour, doseSchedule[0].minute);
            UART2_Print(buf);
            char lcdBuf[17];
            snprintf(lcdBuf, sizeof(lcdBuf), "Next: %02d:%02d",
                     doseSchedule[0].hour, doseSchedule[0].minute);
            LCD_PrintLine(1, lcdBuf);
        } else {
            UART2_Print("No more doses today.\r\n");
            LCD_PrintLine(1, "No more doses");
        }
    }
}

// ============================================================
// Handle dose cycle
// ============================================================
void HandleDoseCycle(void) {
    char buf[64];

    snprintf(buf, sizeof(buf), "ALARM: Dose due at %02d:%02d\r\n",
             doseSchedule[0].hour, doseSchedule[0].minute);
    UART2_Print(buf);

    // ---- Rotate servo to next compartment ----
    Servo_RotateToNext();

    // ---- PHASE 1: Buzzer on, wait for pill removal or switch ----
    LCD_PrintLine(0, "** DOSE DUE **");
    LCD_PrintLine(1, "Pill present");
    Buzzer_On();

    uint32_t startTime = HAL_GetTick();
    uint8_t pillTaken = 0;
    uint8_t switchAcknowledged = 0;

    while (HAL_GetTick() - startTime < ACK_TIMEOUT_MS) {
        if (!Pill_Present()) {
            pillTaken = 1;
            break;
        }
        if (Switch_IsLow()) {
            switchAcknowledged = 1;
            break;
        }
        osDelay(200);
    }

    if (pillTaken) {
        // Pill removed directly
        Buzzer_Off();
        snprintf(buf, sizeof(buf), "Dose TAKEN at %02d:%02d\r\n",
                 doseSchedule[0].hour, doseSchedule[0].minute);
        UART2_Print(buf);
        LCD_PrintLine(0, "Dose taken!");
        LCD_PrintLine(1, "");
        osDelay(3000);
        AdvanceToNextDose();
        doseTriggeredThisMinute = 1;
        return;
    }

    if (!switchAcknowledged) {
        // 30s timeout — missed
        Buzzer_Off();
        snprintf(buf, sizeof(buf), "Dose MISSED at %02d:%02d\r\n",
                 doseSchedule[0].hour, doseSchedule[0].minute);
        UART2_Print(buf);
        LCD_PrintLine(0, "Dose MISSED!");
        LCD_PrintLine(1, "");
        osDelay(3000);
        AdvanceToNextDose();
        doseTriggeredThisMinute = 1;
        return;
    }

    // ---- PHASE 2: Switch acknowledged, pill still present ----
    Buzzer_Off();
    UART2_Print("Acknowledged - pill still present. Waiting 30s...\r\n");
    LCD_PrintLine(0, "Acknowledged");
    LCD_PrintLine(1, "Take pill now!");

    // Wait for switch to go back HIGH
    while (Switch_IsLow()) { osDelay(50); }

    // Wait up to 30s for pill removal
    startTime = HAL_GetTick();
    pillTaken = 0;

    while (HAL_GetTick() - startTime < ACK_AFTER_SWITCH_MS) {
        if (!Pill_Present()) {
            pillTaken = 1;
            break;
        }
        osDelay(200);
    }

    if (pillTaken) {
        snprintf(buf, sizeof(buf), "Dose TAKEN at %02d:%02d (after ack)\r\n",
                 doseSchedule[0].hour, doseSchedule[0].minute);
        UART2_Print(buf);
        LCD_PrintLine(0, "Dose taken!");
        LCD_PrintLine(1, "");
    } else {
        snprintf(buf, sizeof(buf), "Dose MISSED at %02d:%02d (after ack)\r\n",
                 doseSchedule[0].hour, doseSchedule[0].minute);
        UART2_Print(buf);
        LCD_PrintLine(0, "Dose MISSED!");
        LCD_PrintLine(1, "");
    }

    osDelay(3000);
    AdvanceToNextDose();
    doseTriggeredThisMinute = 1;
}

// ============================================================
// Bluetooth command parser — "SET 08:00 13:00 18:00 22:00"
// ============================================================
void BT_ProcessCommand(char *cmd) {
    char buf[64];
    char lcdBuf[17];

    UART2_Print("Processing: ");
    UART2_Print(cmd);
    UART2_Print("\r\n");

    if (strncmp(cmd, "SET", 3) == 0) {
        numDoses = 0;
        char *ptr = cmd + 4;
        while (*ptr && numDoses < MAX_DOSES) {
            uint8_t h = 0, m = 0;
            if (sscanf(ptr, "%hhu:%hhu", &h, &m) == 2) {
                doseSchedule[numDoses].hour   = h;
                doseSchedule[numDoses].minute = m;
                doseSchedule[numDoses].active = 1;
                numDoses++;
            }
            ptr = strchr(ptr, ' ');
            if (ptr) ptr++; else break;
        }

        snprintf(buf, sizeof(buf), "Received %d dose times:\r\n", numDoses);
        UART2_Print(buf);
        for (int i = 0; i < numDoses; i++) {
            snprintf(buf, sizeof(buf), "  Dose %d: %02d:%02d\r\n",
                     i+1, doseSchedule[i].hour, doseSchedule[i].minute);
            UART2_Print(buf);
        }

        // Save to flash so schedule survives power cycles
        Flash_SaveSchedule();

        if (numDoses > 0) {
            snprintf(lcdBuf, sizeof(lcdBuf), "Next: %02d:%02d",
                     doseSchedule[0].hour, doseSchedule[0].minute);
        } else {
            snprintf(lcdBuf, sizeof(lcdBuf), "No doses set");
        }
        LCD_PrintLine(1, lcdBuf);
        doseTriggeredThisMinute = 0;

    } else {
        UART2_Print("Unknown command. Use: SET HH:MM HH:MM\r\n");
    }
}

/* USER CODE END 0 */

int main(void)
{
  HAL_Init();
  SystemClock_Config();

  MX_GPIO_Init();
  MX_USART2_UART_Init();
  MX_I2C1_Init();
  MX_ADC1_Init();
  MX_TIM2_Init();
  MX_USART1_UART_Init();

  /* USER CODE BEGIN 2 */
  UART2_Print("\r\n===================================\r\n");
  UART2_Print("  Smart Medication Dispenser\r\n");
  UART2_Print("===================================\r\n\r\n");

  I2C_Scan();
  UART2_Print("\r\n");

  LCD_Init();
  LCD_PrintLine(0, "Med Dispenser");
  LCD_PrintLine(1, "Loading...");

  // Uncomment once to set time, then comment out again
 // RTC_SetTime(17, 19, 0);

  // Load saved schedule from flash
  Flash_LoadSchedule();

  // Update LCD with loaded schedule
  if (numDoses > 0) {
      char lcdBuf[17];
      snprintf(lcdBuf, sizeof(lcdBuf), "Next: %02d:%02d",
               doseSchedule[0].hour, doseSchedule[0].minute);
      LCD_PrintLine(1, lcdBuf);
  } else {
      LCD_PrintLine(1, "No doses set");
  }

  // Switch and IR boot check
  if (Switch_IsLow()) {
      UART2_Print("WARNING: Switch is LOW on boot!\r\n");
  } else {
      UART2_Print("Switch is HIGH. Ready.\r\n");
  }

  char irBuf[32];
  snprintf(irBuf, sizeof(irBuf), "IR on boot: %d\r\n", IR_Read());
  UART2_Print(irBuf);

  UART2_Print("Send 'SET HH:MM HH:MM ...' from Arduino Blue.\r\n\r\n");

  // ---- Home servo to compartment 0 at boot ----
  UART2_Print("Homing servo to compartment 0...\r\n");
  HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_1);
  __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, SERVO_COMP_0);
  HAL_Delay(1500);
  currentCompartment = 1;  // next rotation will go to compartment 1
  UART2_Print("Servo homed.\r\n");
  /* USER CODE END 2 */

  osKernelInitialize();
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);
  osKernelStart();

  while (1) {}
}

// ============================================================
// Main Task
// ============================================================
void StartDefaultTask(void *argument)
{
  /* USER CODE BEGIN 5 */
  uint8_t testByte;
  char lcdLine1[17];
  char lcdLine2[17];
  uint32_t lastUpdate = 0;
  uint8_t lastMinute = 99;

  for(;;)
  {
      // ---- Bluetooth receive ----
      if (HAL_UART_Receive(&huart1, &testByte, 1, 10) == HAL_OK) {
          if (testByte == '\n' || testByte == '\r' || testByte == 0xFA) {
              if (btCmdIdx > 0) {
                  btCmdBuf[btCmdIdx] = '\0';
                  btCmdIdx = 0;
                  BT_ProcessCommand(btCmdBuf);
              }
          } else if (testByte >= 32 && testByte < 127) {
              if (btCmdIdx < sizeof(btCmdBuf) - 1) {
                  btCmdBuf[btCmdIdx++] = testByte;
              }
          }
      }

      // ---- Check time every 5 seconds ----
      uint32_t now = HAL_GetTick();
      if (now - lastUpdate >= 5000) {
          lastUpdate = now;

          uint8_t h, m, s;
          RTC_GetTime(&h, &m, &s);

          if (m != lastMinute) {
              lastMinute = m;
              doseTriggeredThisMinute = 0;

              snprintf(lcdLine1, sizeof(lcdLine1), "Time:  %02d:%02d", h, m);
              LCD_PrintLine(0, lcdLine1);

              if (numDoses > 0) {
                  snprintf(lcdLine2, sizeof(lcdLine2), "Next: %02d:%02d",
                           doseSchedule[0].hour, doseSchedule[0].minute);
              } else {
                  snprintf(lcdLine2, sizeof(lcdLine2), "No doses set");
              }
              LCD_PrintLine(1, lcdLine2);
          }

          // ---- Dose time check ----
          if (numDoses > 0 && !doseTriggeredThisMinute) {
              if (h == doseSchedule[0].hour && m == doseSchedule[0].minute) {
                  HandleDoseCycle();
              }
          }
      }

      osDelay(10);
  }
  /* USER CODE END 5 */
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  if (htim->Instance == TIM1) HAL_IncTick();
}

void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1);
  HAL_PWR_EnableBkUpAccess();
  __HAL_RCC_LSEDRIVE_CONFIG(RCC_LSEDRIVE_LOW);

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_LSE|RCC_OSCILLATORTYPE_MSI;
  RCC_OscInitStruct.LSEState = RCC_LSE_ON;
  RCC_OscInitStruct.MSIState = RCC_MSI_ON;
  RCC_OscInitStruct.MSICalibrationValue = 0;
  RCC_OscInitStruct.MSIClockRange = RCC_MSIRANGE_6;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_MSI;
  RCC_OscInitStruct.PLL.PLLM = 1;
  RCC_OscInitStruct.PLL.PLLN = 16;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV7;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  HAL_RCC_OscConfig(&RCC_OscInitStruct);

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
  HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_1);
  HAL_RCCEx_EnableMSIPLLMode();
}

static void MX_ADC1_Init(void)
{
  ADC_ChannelConfTypeDef sConfig = {0};
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_ASYNC_DIV1;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadc1.Init.LowPowerAutoWait = DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.DMAContinuousRequests = DISABLE;
  hadc1.Init.Overrun = ADC_OVR_DATA_PRESERVED;
  hadc1.Init.OversamplingMode = DISABLE;
  HAL_ADC_Init(&hadc1);
  sConfig.Channel = ADC_CHANNEL_8;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_2CYCLES_5;
  sConfig.SingleDiff = ADC_SINGLE_ENDED;
  sConfig.OffsetNumber = ADC_OFFSET_NONE;
  sConfig.Offset = 0;
  HAL_ADC_ConfigChannel(&hadc1, &sConfig);
}

static void MX_I2C1_Init(void)
{
  hi2c1.Instance = I2C1;
  hi2c1.Init.Timing = 0x00707CBB;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  HAL_I2C_Init(&hi2c1);
  HAL_I2CEx_ConfigAnalogFilter(&hi2c1, I2C_ANALOGFILTER_ENABLE);
  HAL_I2CEx_ConfigDigitalFilter(&hi2c1, 0);
}

static void MX_TIM2_Init(void)
{
  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 79;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 19999;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
  HAL_TIM_PWM_Init(&htim2);
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig);
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 0;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  HAL_TIM_PWM_ConfigChannel(&htim2, &sConfigOC, TIM_CHANNEL_1);
  HAL_TIM_PWM_ConfigChannel(&htim2, &sConfigOC, TIM_CHANNEL_2);
  HAL_TIM_MspPostInit(&htim2);
}

static void MX_USART1_UART_Init(void)
{
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 9600;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  huart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  HAL_UART_Init(&huart1);
}

static void MX_USART2_UART_Init(void)
{
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  huart2.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart2.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  HAL_UART_Init(&huart2);
}

static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(LD3_GPIO_Port, LD3_Pin, GPIO_PIN_RESET);

  // PA4 — Active buzzer (GPIO HIGH = on)
  GPIO_InitStruct.Pin = GPIO_PIN_4;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  // PA8 — SPDT switch (input, pull-up, active LOW)
  GPIO_InitStruct.Pin = GPIO_PIN_8;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  // PB0 — unused input
  GPIO_InitStruct.Pin = RTC_INT_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(RTC_INT_GPIO_Port, &GPIO_InitStruct);

  // PB3 — Onboard LED
  GPIO_InitStruct.Pin = LD3_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LD3_GPIO_Port, &GPIO_InitStruct);
}

void Error_Handler(void)
{
  __disable_irq();
  while (1) {}
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line) {}
#endif