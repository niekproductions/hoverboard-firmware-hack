/*
* This file is part of the hoverboard-firmware-hack project.
*
* Copyright (C) 2017-2018 Rene Hopf <renehopf@mac.com>
* Copyright (C) 2017-2018 Nico Stute <crinq@crinq.de>
* Copyright (C) 2017-2018 Niklas Fauth <niklas.fauth@kit.fail>
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdbool.h>
#include "stm32f1xx_hal.h"
#include "defines.h"
#include "setup.h"
#include "config.h"
#include "hd44780.h"

void SystemClock_Config(void);

extern TIM_HandleTypeDef htim_left;
extern TIM_HandleTypeDef htim_right;
extern ADC_HandleTypeDef hadc1;
extern ADC_HandleTypeDef hadc2;
extern volatile adc_buf_t adc_buffer;
LCD_PCF8574_HandleTypeDef lcd;
extern I2C_HandleTypeDef hi2c2;
extern UART_HandleTypeDef huart2;

int cmd1;  // normalized input values. -1000 to 1000
int cmd2;
int cmd3;

typedef struct{
   int16_t steer;
   int16_t speed;
   //uint32_t crc;
} Serialcommand;

volatile Serialcommand command;

uint8_t button1, button2;

int steer; // global variable for steering. -1000 to 1000
int speed; // global variable for speed. -1000 to 1000

extern volatile int pwml;  // global variable for pwm left. -1000 to 1000
extern volatile int pwmr;  // global variable for pwm right. -1000 to 1000
extern volatile int weakl; // global variable for field weakening left. -1000 to 1000
extern volatile int weakr; // global variable for field weakening right. -1000 to 1000

extern uint8_t buzzerFreq;    // global variable for the buzzer pitch. can be 1, 2, 3, 4, 5, 6, 7...
extern uint8_t buzzerPattern; // global variable for the buzzer pattern. can be 1, 2, 3, 4, 5, 6, 7...

extern uint8_t enable; // global variable for motor enable

extern volatile uint32_t timeout; // global variable for timeout
extern float batteryVoltage; // global variable for battery voltage

bool displayWarning = false;

uint32_t inactivity_timeout_counter;
uint32_t main_loop_counter;

int32_t motor_test_direction = 1;

extern uint8_t nunchuck_data[6];
uint32_t nunchuck_bias_x;
uint32_t nunchuck_bias_y;

bool doReverse = false;

#ifdef CONTROL_PPM
extern volatile uint16_t ppm_captured_value[PPM_NUM_CHANNELS+1];
#endif

int milli_vel_error_sum = 0;


void poweroff() {
    #ifndef CONTROL_MOTOR_TEST
    if (abs(speed) < 20) {
    #endif
        buzzerPattern = 0;
        enable = 0;
        for (int i = 0; i < 8; i++) {
            buzzerFreq = i;
            HAL_Delay(100);
        }
        HAL_GPIO_WritePin(OFF_PORT, OFF_PIN, 0);
        while(1) {}
    #ifndef CONTROL_MOTOR_TEST
    }
    #endif
}


int main(void) {
  HAL_Init();
  __HAL_RCC_AFIO_CLK_ENABLE();
  HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);
  /* System interrupt init*/
  /* MemoryManagement_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(MemoryManagement_IRQn, 0, 0);
  /* BusFault_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(BusFault_IRQn, 0, 0);
  /* UsageFault_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(UsageFault_IRQn, 0, 0);
  /* SVCall_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(SVCall_IRQn, 0, 0);
  /* DebugMonitor_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DebugMonitor_IRQn, 0, 0);
  /* PendSV_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(PendSV_IRQn, 0, 0);
  /* SysTick_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(SysTick_IRQn, 0, 0);

  SystemClock_Config();

  __HAL_RCC_DMA1_CLK_DISABLE();
  MX_GPIO_Init();
  MX_TIM_Init();
  MX_ADC1_Init();
  MX_ADC2_Init();

  #if defined(DEBUG_SERIAL_USART2) || defined(DEBUG_SERIAL_USART3)
    UART_Init();
  #endif

  HAL_GPIO_WritePin(OFF_PORT, OFF_PIN, 1);

  HAL_ADC_Start(&hadc1);
  HAL_ADC_Start(&hadc2);

  for (int i = 8; i >= 0; i--) {
    buzzerFreq = i;
    HAL_Delay(100);
  }
  buzzerFreq = 0;

  HAL_GPIO_WritePin(LED_PORT, LED_PIN, 1);

  int lastSpeedL = 0, lastSpeedR = 0;
  int speedL = 0, speedR = 0;

  #ifdef CONTROL_PPM
    PPM_Init();
  #endif

  #ifdef CONTROL_NUNCHUCK
    I2C_Init();
    Nunchuck_Init();
    nunchuck_bias_x = (NUNCHUCK_MAX_X - NUNCHUCK_MIN_X)/2 + NUNCHUCK_MIN_X - 1;
    nunchuck_bias_y = (NUNCHUCK_MAX_Y - NUNCHUCK_MIN_Y)/2 + NUNCHUCK_MIN_Y - 1;
  #endif

  #ifdef CONTROL_SERIAL_USART2
    UART_Control_Init();
    HAL_UART_Receive_DMA(&huart2, (uint8_t *)&command, 4);
  #endif

  #ifdef DEBUG_I2C_LCD
    I2C_Init();
    HAL_Delay(50);
    lcd.pcf8574.PCF_I2C_ADDRESS = 0x20;
    lcd.pcf8574.PCF_I2C_TIMEOUT = 5;
    lcd.pcf8574.i2c = hi2c2;
    lcd.NUMBER_OF_LINES = NUMBER_OF_LINES_2;
    lcd.type = TYPE0;

    if(LCD_Init(&lcd)!=LCD_OK){
        // error occured
        //TODO while(1);
    }

    uint8_t fullChar[8] = {
	  0b11111,
	  0b11111,
	  0b11111,
	  0b11111,
	  0b11111,
	  0b11111,
	  0b11111,
      0b11111
    };  

     uint8_t battChar[8] = {
	  0b01110,
	  0b11011,
	  0b10001,
	  0b11111,
	  0b11111,
	  0b11111,
	  0b11111,
	  0b11111
    };  

    uint8_t forwardChar[8] = {
	  0b00100,
	  0b01110,
	  0b10101,
	  0b00100,
	  0b00100,
	  0b00100,
	  0b00100,
	  0b00000
    };  

    uint8_t reverseChar[8] = {
	  0b00100,
	  0b00100,
	  0b00100,
	  0b00100,
	  0b10101,
	  0b01110,
	  0b00100,
	  0b00000
    };

    // Add custom characters to display memory
    LCD_CustomChar(&lcd, fullChar, 1);
    LCD_CustomChar(&lcd, battChar, 2);
    LCD_CustomChar(&lcd, forwardChar, 3);
    LCD_CustomChar(&lcd, reverseChar, 4);

    LCD_ClearDisplay(&lcd);
    HAL_Delay(5);
    LCD_SetLocation(&lcd, 0, 0);
    LCD_WriteString(&lcd, "Kratoffel");
    LCD_SetLocation(&lcd, 0, 1);
    LCD_WriteString(&lcd, "@blankers.eu");
    HAL_Delay(1000);

    LCD_ClearDisplay(&lcd);
    HAL_Delay(5);
    LCD_SetLocation(&lcd, 0, 0);
    LCD_WriteString(&lcd, "Initializing...");
    HAL_Delay(500);
    LCD_SetLocation(&lcd, 0, 1);
    for(uint8_t i = 0; i < 16; i++) {
        // Write blocks
        LCD_WaitForBusyFlag(&lcd);
	    LCD_WriteDATA(&lcd, (uint8_t) 1);
        HAL_Delay(50);
    }
    HAL_Delay(200);
    LCD_ClearDisplay(&lcd);
    HAL_Delay(5);

    #ifdef CAL_NUNCHUCK
    while(1) {
      Nunchuck_Read();
      LCD_SetLocation(&lcd, 0, 0);
      LCD_WriteString(&lcd, "X: ");
      LCD_WriteFloat(&lcd,nunchuck_data[0], 1);
      LCD_SetLocation(&lcd, 0, 1);
      LCD_WriteString(&lcd, "Y: ");
      LCD_WriteFloat(&lcd,nunchuck_data[1], 1);

      button1 = (uint8_t)nunchuck_data[5] & 1;
      button2 = (uint8_t)(nunchuck_data[5] >> 1) & 1;

      LCD_SetLocation(&lcd, 15, 0);
      if (button1)
        LCD_WriteString(&lcd, "B");
      else
        LCD_WriteString(&lcd, " ");
      LCD_SetLocation(&lcd, 15, 1);
      if (button2)
        LCD_WriteString(&lcd, "B");
      else
        LCD_WriteString(&lcd, " ");
      HAL_Delay(50);
    }    
    #endif

    #ifdef CAL_BATT
    while(1) {
      LCD_SetLocation(&lcd, 0, 0);
      LCD_WriteString(&lcd, "Batt ADC: ");
      LCD_WriteFloat(&lcd, (int)adc_buffer.batt1, 0);

      LCD_SetLocation(&lcd, 0, 1);
      LCD_WriteString(&lcd, "Volt: ");
      LCD_WriteFloat(&lcd, (int)(batteryVoltage * 100.0f), 0);
      HAL_Delay(50);
    }
    #endif
  #endif

  float board_temp_adc_filtered = (float)adc_buffer.temp;
  float board_temp_deg_c;

  enable = 1;  // enable motors

  while(1) {
    HAL_Delay(DELAY_IN_MAIN_LOOP); //delay in ms

    #ifdef CONTROL_NUNCHUCK
      Nunchuck_Read();

      cmd1 = CLAMP((nunchuck_data[0] - (uint8_t) nunchuck_bias_x) * 8, -1000, 1000); // x - axis. Set nunchuck range in config.h
      cmd2 = CLAMP((nunchuck_data[1] - (uint8_t) nunchuck_bias_y) * 8, -1000, 1000); // y - axis

      button1 = (uint8_t)nunchuck_data[5] & 1;
      button2 = (uint8_t)(nunchuck_data[5] >> 1) & 1;
      static uint8_t lastButton1 = 1;

      if (!button1) { // Bottom trigger pressed, sound horn
        buzzerPattern = 0;
        buzzerFreq = 7; // Enable buzzer, higher number = lower frequency
      } else if (!lastButton1) {
        buzzerFreq = 0; // Disable buzzer
      }
      lastButton1 = button1;

      if (!button2) { // Top button pressed, reverse direction
        buzzerPattern = 0;
        if (abs(speed) > 20) { // Still driving, warn user
            buzzerFreq = 5;
            HAL_Delay(100);
            buzzerFreq = 0;
        } else {
            doReverse = !doReverse;

            if(!doReverse) {
                #ifdef DEBUG_I2C_LCD
                LCD_SetLocation(&lcd, 15, 1);
                LCD_WriteString(&lcd, "\x03"); // Forward arrow
                #endif
                buzzerFreq = 5;
                HAL_Delay(400);
                buzzerFreq = 0;
            } else {
                #ifdef DEBUG_I2C_LCD
                LCD_SetLocation(&lcd, 15, 1);
                LCD_WriteString(&lcd, "\x04"); // Reverse arrow
                #endif
                buzzerFreq = 5;
                HAL_Delay(400);
                buzzerFreq = 0;
                HAL_Delay(400);
                buzzerFreq = 5;
                HAL_Delay(400);
                buzzerFreq = 0;
            }
        }
      }
    #endif

    #ifdef CONTROL_PPM
      cmd1 = CLAMP((ppm_captured_value[0] - 500) * 2, -1000, 1000);
      cmd2 = CLAMP((ppm_captured_value[1] - 500) * 2, -1000, 1000);
      button1 = ppm_captured_value[5] > 500;
      float scale = ppm_captured_value[2] / 1000.0f;
    #endif

    #ifdef CONTROL_ADC
      // ADC values range: 0-4095, see ADC-calibration in config.h
      cmd1 = CLAMP(adc_buffer.l_tx2 - ADC1_MIN, 0, ADC1_MAX) / (ADC1_MAX / 1000.0f);  // ADC1
      cmd2 = CLAMP(adc_buffer.l_rx2 - ADC2_MIN, 0, ADC2_MAX) / (ADC2_MAX / 1000.0f);  // ADC2

      // use ADCs as button inputs:
      button1 = (uint8_t)(adc_buffer.l_tx2 > 2000);  // ADC1
      button2 = (uint8_t)(adc_buffer.l_rx2 > 2000);  // ADC2

      timeout = 0;
    #endif

    #ifdef CONTROL_SERIAL_USART2
      cmd1 = CLAMP((int16_t)command.steer, -1000, 1000);
      cmd2 = CLAMP((int16_t)command.speed, -1000, 1000);

      timeout = 0;
    #endif

    #ifdef CONTROL_MOTOR_TEST
      if (motor_test_direction == 1) cmd2 += 1;
      else cmd2 -= 1;
      if (abs(cmd2) > CONTROL_MOTOR_TEST_MAX_SPEED) motor_test_direction = -motor_test_direction;

      timeout = 0;
    #endif

    // TODO: reduce steering when speed goes up

    // ####### LOW-PASS FILTER #######
    steer = steer * (1.0 - FILTER) + cmd1 * FILTER;
    speed = speed * (1.0 - FILTER) + cmd2 * FILTER;


    // ####### MIXER #######
    if (!doReverse) {
        speedR = CLAMP(speed * SPEED_COEFFICIENT -  steer * STEER_COEFFICIENT, -1000, 1000);
        speedL = CLAMP(speed * SPEED_COEFFICIENT +  steer * STEER_COEFFICIENT, -1000, 1000);
    } else {
        speedR = CLAMP(-speed * SPEED_COEFFICIENT +  steer * STEER_COEFFICIENT, -1000, 1000);
        speedL = CLAMP(-speed * SPEED_COEFFICIENT -  steer * STEER_COEFFICIENT, -1000, 1000);
    }


    #ifdef ADDITIONAL_CODE
      ADDITIONAL_CODE;
    #endif


    // ####### SET OUTPUTS #######
    if ((speedL < lastSpeedL + 50 && speedL > lastSpeedL - 50) && (speedR < lastSpeedR + 50 && speedR > lastSpeedR - 50) && timeout < TIMEOUT) {
    #ifdef INVERT_R_DIRECTION
      pwmr = speedR;
    #else
      pwmr = -speedR;
    #endif
    #ifdef INVERT_L_DIRECTION
      pwml = -speedL;
    #else
      pwml = speedL;
    #endif
    }

    lastSpeedL = speedL;
    lastSpeedR = speedR;


    if (main_loop_counter % 25 == 0) {
      // ####### CALC BOARD TEMPERATURE #######
      board_temp_adc_filtered = board_temp_adc_filtered * 0.99 + (float)adc_buffer.temp * 0.01;
      board_temp_deg_c = ((float)TEMP_CAL_HIGH_DEG_C - (float)TEMP_CAL_LOW_DEG_C) / ((float)TEMP_CAL_HIGH_ADC - (float)TEMP_CAL_LOW_ADC) * (board_temp_adc_filtered - (float)TEMP_CAL_LOW_ADC) + (float)TEMP_CAL_LOW_DEG_C;
      
      // ####### DEBUG SERIAL OUT #######
      #if (defined DEBUG_SERIAL_USART2 || defined DEBUG_SERIAL_USART3)
          #ifdef CONTROL_ADC
            setScopeChannel(0, (int)adc_buffer.l_tx2);  // 1: ADC1
            setScopeChannel(1, (int)adc_buffer.l_rx2);  // 2: ADC2
          #endif
          setScopeChannel(2, (int)speedR);  // 3: output speed: 0-1000
          setScopeChannel(3, (int)speedL);  // 4: output speed: 0-1000
          setScopeChannel(4, (int)adc_buffer.batt1);  // 5: for battery voltage calibration
          setScopeChannel(5, (int)(batteryVoltage * 100.0f));  // 6: for verifying battery voltage calibration
          setScopeChannel(6, (int)board_temp_adc_filtered);  // 7: for board temperature calibration
          setScopeChannel(7, (int)board_temp_deg_c);  // 8: for verifying board temperature calibration
          consoleScope();
      #endif

      #ifdef DEBUG_I2C_LCD
          /* Layout while driving (full):
           * [L100 R100  b100%]
           * [████████████████]
           *
           */

          if (!displayWarning) {
              // Clear first row
              LCD_SetLocation(&lcd, 0, 0);
              LCD_WriteString(&lcd, "                ");

              // speedR and speedL -1000 to 1000, display as percentage
              LCD_SetLocation(&lcd, 0, 0);
              LCD_WriteString(&lcd, "L");
              LCD_WriteFloat(&lcd, (double) (speedL/10), 0);
              LCD_SetLocation(&lcd, 5, 0);
              LCD_WriteString(&lcd, "R");
              LCD_WriteFloat(&lcd, (double) (speedR/10), 0);

              // Battery percentage
              float battLow = (float)BAT_LOW_LVL2 * (float)BAT_NUMBER_OF_CELLS;
              float battPercent = 100.0 * (batteryVoltage - battLow) / ((float)BAT_FULL - battLow);
              if (battPercent > 100.0)
                battPercent = 100.0;
              if (battPercent < 0.0)
                battPercent = 0.0;
              if (battPercent < 100.0)
                LCD_SetLocation(&lcd, 12, 0);
              else
                LCD_SetLocation(&lcd, 11, 0);

              LCD_WriteString(&lcd, "\x02"); // Battery icon
              LCD_WriteFloat(&lcd, battPercent, 0);
              LCD_WriteString(&lcd, "%");
              

              LCD_SetLocation(&lcd, 0, 1);
              uint32_t numBlocks = abs(speed) / 62 + 1;
              if (abs(speed) <= 20)
                numBlocks = 0;

              char speedString[17] = "                ";
              if (!doReverse)
                speedString[15] = (uint8_t) 3; // Forward arrow
              else
                speedString[15] = (uint8_t) 4; // Reverse arrow

              for (uint8_t i = 0; i < numBlocks; i++) { // Write blocks
                speedString[i] = (uint8_t) 1; // █
              }
              LCD_WriteString(&lcd, speedString);
          } else {
              // Low battery warning
              LCD_SetLocation(&lcd, 0, 0);
              LCD_WriteString(&lcd, "  " "\x02" "BATTERY DEAD" "\x02" "  ");
              LCD_SetLocation(&lcd, 0, 1);
              LCD_WriteString(&lcd, "  CHARGE NOW!!  ");
          }
      #endif
    }


    // ####### POWEROFF BY POWER-BUTTON #######
    if (HAL_GPIO_ReadPin(BUTTON_PORT, BUTTON_PIN) && weakr == 0 && weakl == 0) {
      enable = 0;
      while (HAL_GPIO_ReadPin(BUTTON_PORT, BUTTON_PIN)) {}
      poweroff();
    }


    // ####### BEEP AND EMERGENCY POWEROFF #######
    displayWarning = false;
    if ((TEMP_POWEROFF_ENABLE && board_temp_deg_c >= TEMP_POWEROFF && abs(speed) < 20) || (batteryVoltage < ((float)BAT_LOW_DEAD * (float)BAT_NUMBER_OF_CELLS) && abs(speed) < 20)) {  // poweroff before mainboard burns OR low bat 3
      poweroff();
    } else if (TEMP_WARNING_ENABLE && board_temp_deg_c >= TEMP_WARNING) {  // beep if mainboard gets hot
      buzzerFreq = 4;
      buzzerPattern = 1;
    } else if (batteryVoltage < ((float)BAT_LOW_LVL1 * (float)BAT_NUMBER_OF_CELLS) && batteryVoltage > ((float)BAT_LOW_LVL2 * (float)BAT_NUMBER_OF_CELLS) && BAT_LOW_LVL1_ENABLE) {  // low bat 1: slow beep
      buzzerFreq = 5;
      buzzerPattern = 42;
    } else if (batteryVoltage < ((float)BAT_LOW_LVL2 * (float)BAT_NUMBER_OF_CELLS) && batteryVoltage > ((float)BAT_LOW_DEAD * (float)BAT_NUMBER_OF_CELLS)) {  // low bat 2: fast beep
      displayWarning = true;
      if (BAT_LOW_LVL2_ENABLE) {
        buzzerFreq = 5;
        buzzerPattern = 6;
      }
    } else if (BEEPS_BACKWARD && speed < -50) {  // backward beep
      buzzerFreq = 5;
      buzzerPattern = 1;
    } else {  // do not beep
      buzzerFreq = 0;
      buzzerPattern = 0;
    }


    // ####### INACTIVITY TIMEOUT #######
    if (abs(speedL) > 50 || abs(speedR) > 50) {
      inactivity_timeout_counter = 0;
    } else {
      inactivity_timeout_counter ++;
    }
    if (inactivity_timeout_counter > (INACTIVITY_TIMEOUT * 60 * 1000) / (DELAY_IN_MAIN_LOOP + 1)) {  // rest of main loop needs maybe 1ms
      poweroff();
    }
    
    main_loop_counter += 1;
    timeout++;
  }
}

/** System Clock Configuration
*/
void SystemClock_Config(void) {
  RCC_OscInitTypeDef RCC_OscInitStruct;
  RCC_ClkInitTypeDef RCC_ClkInitStruct;
  RCC_PeriphCLKInitTypeDef PeriphClkInit;

  /**Initializes the CPU, AHB and APB busses clocks
    */
  RCC_OscInitStruct.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState            = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = 16;
  RCC_OscInitStruct.PLL.PLLState        = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource       = RCC_PLLSOURCE_HSI_DIV2;
  RCC_OscInitStruct.PLL.PLLMUL          = RCC_PLL_MUL16;
  HAL_RCC_OscConfig(&RCC_OscInitStruct);

  /**Initializes the CPU, AHB and APB busses clocks
    */
  RCC_ClkInitStruct.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2);

  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_ADC;
  PeriphClkInit.AdcClockSelection    = RCC_ADCPCLK2_DIV8;  // 8 MHz
  HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit);

  /**Configure the Systick interrupt time
    */
  HAL_SYSTICK_Config(HAL_RCC_GetHCLKFreq() / 1000);

  /**Configure the Systick
    */
  HAL_SYSTICK_CLKSourceConfig(SYSTICK_CLKSOURCE_HCLK);

  /* SysTick_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(SysTick_IRQn, 0, 0);
}
