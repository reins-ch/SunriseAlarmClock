// -----
// SunriseAlarmClock.ino - Prototye software for a sunrise alarm clock/ dimmable bedside lamp.
// This class is implemented for use with the Arduino environment.
// Copyright (c) by Christoph Reinsch
// -----
// 04.10.2018 created by Christoph Reinsch
// -----

// What this software does:
// - get the current time via DCF77
// - use a rotary encoder with button to set the alarm time and duration
//   - single click to toggle light on/off, rotate to set brightness
//   - double click to switch alarm on/off
//   - long press to toggle alarm edit mode, single click to switch the unit to edit, rotate to set value
// - use an AC pwm dimmer module to control any dimmable 230V bulb

// Rotary encoder setup:
// Attach a pins CLK and DT to A2 and A3.
// Attach SW pin to digital pin 13 (SW_PIN)
// GND to GND, + to 3.3V

// DCF board setup:
// P1 to pin 7 (DCF_POWER)
// T to pin 2 (DCF_PIN)
// G to GND, V to 3,3V ***MUST BE 3,3V***
#include "PinChangeInterrupt.h"
#include "DCF77.h"
#include <RotaryEncoder.h>
#include <TimerOne.h>
#include <Time.h>
#include <TimeLib.h>
#include <OneButton.h>
#include "ssd1306.h"
#include "nano_gfx.h"

#define DCF_PIN 2 // Connection pin to DCF 77 device
#define DCF_INTERRUPT 0 // Interrupt number associated with pin
#define DCF_POWER 7 // LOW - DCF on, HIGH - DCF off, do not leave dangling

#define SW_PIN 13 // Pin of Encoder Button
#define ENC_PIN_A 5 // Pin CLK of Encoder
#define ENC_PIN_B 6 // Pin DT of Encoder

#define AC_LOAD 11   // Output to Opto Triac pin, has to be 11 (Timer2)

#define ENC_CW 1 // encoder rotating clockwise
#define ENC_CCW -1 // encoder rotating counter-clockwise

#define HOUR 1 // enum for setting time
#define MINUTE 2 // enum for setting time
#define DURATION 3 // enum for setting time

//////////// Internal (functionality) states
boolean alarmEnabled = true; // if the alarm is primed
boolean lightOn = false; // if the light is on
boolean syncTimer1ToMains = false; // if the zeroCrossInterrupt should sync the Timer1

//////////// UI states
enum uiStates {
  mainView,
  setAlarmView,
  setTimeView
};
uiStates uiState = mainView;

//////////// Variables being changed by the user
byte timeHour = 0; // 0 - 23
byte timeMinute = 0; // 0 - 59
byte alarmHour = 6; // 0 - 23
byte alarmMinute = 30; // 0 - 59
byte alarmDuration = 15; // 0 - 30
uint8_t lightIntensity = 128; // Dimming level (0-128)  0 = ON, 128 = OFF

//////////// Internal Variables
uint16_t alarmIntensity = 0;
uint8_t secondsPerStep;
byte varToEdit = HOUR; // HOUR | MINUTE | DURATION

//////////// Library variables
time_t time;
DCF77 DCF = DCF77(DCF_PIN, DCF_INTERRUPT);
OneButton encoderBtn(SW_PIN, true);
RotaryEncoder encoder(ENC_PIN_A, ENC_PIN_B);

//////////// Loop Variables
uint32_t encLastMillis; // last time read
uint16_t encCount;      // running count of encoder clicks
uint16_t encSpeed;      // last calculated speed in clicks/second
uint8_t lastSecond;     // the last second any "set_View" was opened

const PROGMEM uint8_t bell [] = {
  0x00, 0x00, 0x00, 0xE0, 0xF8, 0xFC, 0xFC, 0xFF, 0xFF, 0xFF, 0xFC, 0xFC, 0xF8, 0xE0, 0x00, 0x00,
  0x00, 0x70, 0x70, 0x7F, 0x7F, 0x7F, 0x7F, 0xFF, 0xFF, 0xFF, 0x7F, 0x7F, 0x7F, 0x7F, 0x70, 0x70
};
const PROGMEM uint8_t empty [] = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

void setup() {
  Serial.begin(57600);
  Serial.println("Prototype software for a sunrise alarm clock/ dimmable bedside lamp");
  pinMode(SW_PIN, INPUT_PULLUP);

  // Initialize DCF module
  // pinMode(DCF_POWER, OUTPUT);
  // digitalWrite(DCF_POWER, LOW);
  // DCF.Start();

  // Initialize the dimming module
  pinMode(AC_LOAD, OUTPUT);                    // Set AC Load pin as output
  attachInterrupt(0, zeroCrossInt, RISING); // Choose the zero cross interrupt # from the table above

  // set frequency for phase cutting to frequency of mains voltage
  Timer1.initialize(10000);
  Timer1.attachInterrupt(turnOnTriac);
  Timer1.stop();

  // Initialize OLED screen
  ssd1306_128x64_i2c_init();
  ssd1306_fillScreen( 0x00 );
  ssd1306_setFixedFont(ssd1306xled_font6x8);

  // Initialize pwm output
  // pinMode(PWM_PIN, OUTPUT);
  // digitalWrite(PWM_PIN, LOW);

  // Initialize OneButton.h functionality for encoder button
  encoderBtn.attachClick(toggleLightOn);
  encoderBtn.attachDoubleClick(toggleAlarmEnabled);
  encoderBtn.attachLongPressStop(switchToSetAlarmView);

  // Initialize encoder wheel
  pinMode(ENC_PIN_A, INPUT);
  pinMode(ENC_PIN_B, INPUT);
  attachPCINT(digitalPinToPCINT(5), encoderInterrupt, CHANGE);
  attachPCINT(digitalPinToPCINT(6), encoderInterrupt, CHANGE);

  secondsPerStep = alarmDuration * 60 / 255;
}

void zeroCrossInt() { // function to be fired at the zero crossing to dim the light
  // Firing angle calculation : 1 full 50Hz wave =1/50=20ms
  // Every zerocrossing thus: (50Hz)-> 10ms (1/2 Cycle) For 60Hz => 8.33ms (10.000/120)
  // 10ms=10000us
  // (10000us - 10us) / 128 = 75 (Approx) For 60Hz =>65
  if (syncTimer1ToMains) {
    syncTimer1ToMains = false;
    delayMicroseconds(75 * lightIntensity); // Off cycle
    digitalWrite(AC_LOAD, HIGH);         // triac firing
    Timer1.start();
    delayMicroseconds(10);               // triac On propogation delay (for 60Hz use 8.33)
    // digitalWrite(AC_LOAD, LOW);          // triac Off
  }
  digitalWrite(AC_LOAD, LOW); // triac Off 
}

/**
 * Interrupt Function to be fired for Timer1
 */
void turnOnTriac() {
  if (lightOn) {
    digitalWrite(AC_LOAD, HIGH);
  }
}

/**
   A single click function of the encoder button
*/
void toggleLightOn () {
  // if (lightOn) {
  //   analogWrite(PWM_PIN, 0);
  // } else {
  //   analogWrite(PWM_PIN, lightIntensity);
  // }
  lightOn = !lightOn;
  syncTimer1ToMains = true;
}

/**
   A double click function of the encoder button
*/
void toggleAlarmEnabled () {
  alarmEnabled = !alarmEnabled;
}

/**
   A single click function of the encoder button
*/
void cycleSetAlarm() {
  varToEdit = (varToEdit == DURATION ? HOUR : varToEdit + 1);
  lastSecond = second();
}

/**
   A single click function of the encoder button
*/

void cycleSetTime() {
  varToEdit = (varToEdit == MINUTE ? HOUR : varToEdit + 1);
  lastSecond = second();
}

/**
   A double click function of the encoder button
*/
void switchToSetTimeView() {
  ssd1306_clearScreen();
  timeHour = hour();
  timeMinute = minute();
  uiState = setTimeView;
  // setAlarm = false;
  // timeState = true;
  encoderBtn.attachClick(cycleSetTime);
  encoderBtn.attachDoubleClick(switchToSetAlarmView);
  encoderBtn.attachLongPressStop(switchToMainView);
  lastSecond = second();
}

/**
   A double click function of the encoder button
   A LongPressStop function of the encoder button
*/
void switchToSetAlarmView () {
  ssd1306_clearScreen();
  uiState = setAlarmView;
  // timeState = false;
  // setAlarm = true;
  encoderBtn.attachClick(cycleSetAlarm);
  encoderBtn.attachDoubleClick(switchToSetTimeView);
  encoderBtn.attachLongPressStop(switchToMainView);
  lastSecond = second();
}

/**
   A LongPressStop function of the encoder button
*/
void switchToMainView () {
  ssd1306_clearScreen();
  if (uiState == setTimeView) {
    setTime(timeHour, timeMinute, 0, 14, 3, 15);
  }
  uiState = mainView;
  // timeState = false;
  // setAlarm = false;
  varToEdit = HOUR;
  encoderBtn.attachClick(toggleLightOn);
  encoderBtn.attachDoubleClick(toggleAlarmEnabled);
  encoderBtn.attachLongPressStop(switchToSetAlarmView);
}

/**
   The interrupt function for the encoder wheel
*/
void encoderInterrupt () {
  encoder.tick(); // just call tick() to check the state.
  encCount++;
  if (millis() - encLastMillis > 500) {
    encSpeed = encCount * (0.25);
    encLastMillis = millis();
    encCount = 0;
  }
}

void loop() {
  uint32_t lastMillis;     // last time read
  uint8_t persistRotation; // count the number off loops to persist the last turned direction for the UI
  int8_t encDir = encoder.getDirection();
  encoderBtn.tick(); // check state of encoder Button
  
  // Persist the last direction so the UI doesn't flash rapidly when the encoder is turned,
  // but the variable to set isn't displayed because it's an odd second
  if (encDir) {
    persistRotation = 50;
  } else {
    persistRotation = persistRotation > 0 ? persistRotation-- : 0;
  }

  // time_t DCFtime = DCF.getTime(); // is there an new DCF77 time
  // if (DCFtime != 0)
  // {
  //   setTime(DCFtime);
  // }

  if (lightOn && encDir && uiState == mainView)
  {
    // change brightness of light when on main view
    if (encDir == ENC_CW) {
      lightIntensity = (lightIntensity > 120 - encSpeed ? 
                        120 : lightIntensity + encSpeed);
    } else if (encDir == ENC_CCW) {
      lightIntensity = (lightIntensity < encSpeed ? 
                        5 : lightIntensity - encSpeed);
    }
    syncTimer1ToMains = true;
    // analogWrite(PWM_PIN, lightIntensity);
  }

  if (uiState == setAlarmView) { // handle changing of the alarm time
    switch (varToEdit) {
      case HOUR:
        handleAlarmTime(encDir, alarmHour, 23);
        break;
      case MINUTE:
        handleAlarmTime(encDir, alarmMinute, 59);
        break;
      case DURATION:
        handleAlarmTime(encDir, alarmDuration, 30);
        secondsPerStep = alarmDuration * 60 / 255;
        break;
    }
  }

  if (uiState == setTimeView) { // handle changing of the time
    switch (varToEdit) {
      case HOUR:
        handleAlarmTime(encDir, timeHour, 23);
        break;
      case MINUTE:
        handleAlarmTime(encDir, timeMinute, 59);
        break;
    }
  }

  if (alarmEnabled && isAlarmOn()) {
    // do alarm handling here (incrementing alarmIntensity)
    if (alarmIntensity == 255) { //Alarm ist zu Ende
      lightOn = true;
      lightIntensity = 255;
      alarmIntensity = 0;
    }
  }

  // UI stuff
  if (millis() - lastMillis > 250) { //draw 4 frames per second
    if (uiState != mainView && ((second() - lastSecond) + 60) % 60 > 15) {
      // switch back to main view after 15 seconds of idling in timeState or setAlarm
      switchToMainView(); 
    }
    if (uiState == setAlarmView) {
      drawSetAlarmView(persistRotation > 0);
    } else if (uiState == setTimeView) {
      drawSetTimeView(persistRotation > 0);
    } else {
      drawMainView();
    }
    drawHeader();
    lastMillis = millis();
    if (lightOn) {
      syncTimer1ToMains = true;
    }
  }
}

/**
 * Draw description and other stuff onto the first 16px in y direction
 * This part is yellow on most of the cheap OLEDs
 */
void drawHeader() {
  char digits[8];

  if (uiState == setAlarmView) {
    switch (varToEdit) {
      case HOUR:
      case MINUTE:
        ssd1306_printFixed(0, 8, "Weckzeit einstellen:", STYLE_NORMAL);
        break;
      case DURATION:
        ssd1306_printFixed(0, 8, "Sonnenaufgangsdauer:", STYLE_NORMAL);
        break;
    }
  } else if (uiState == setTimeView) {
    ssd1306_printFixed(0, 8, "Uhrzeit einstellen:", STYLE_NORMAL);
  } else {
    sprintf(digits, "%02d:%02d %02d'", alarmHour, alarmMinute, alarmDuration);
    ssd1306_printFixedN(0, 0, digits, STYLE_NORMAL, FONT_SIZE_2X);
    if (alarmEnabled) {
      ssd1306_drawBitmap(112, 0, 16, 16, bell);
    } else {
      ssd1306_drawBitmap(112, 0, 16, 16, empty);
    }
  }
}

/**
 * Draw what should be displayed when you're not interacting with the device
 */
void drawMainView() {
  char digits[5];

  // Draw the current time
  if (second() % 2) {
    sprintf(digits, "%02d:%02d", hour(), minute());
  } else {
    sprintf(digits, "%02d %02d", hour(), minute());
  }
  ssd1306_printFixedN(0, 36, digits, STYLE_NORMAL, FONT_SIZE_4X);

  // // Draw the lightIntensity
  // uint8_t rectWidth = 4;
  // uint8_t rectHeight = 48;
  // uint8_t intensityRectHeight = rectHeight * (lightIntensity / 255);
  // uint8_t intensityRectOffsetY = rectHeight - intensityRectHeight;
  // uint8_t buffer[rectWidth * rectHeight / 8];
  // NanoCanvas canvas(rectWidth, rectHeight, buffer);
  // canvas.clear();
  // // canvas.setOffset(intensityRectOffsetY, 0);
  // canvas.fillRect(124, 16, 127, 16 + intensityRectHeight, 0xFF);
  // canvas.blt((ssd1306_displayWidth() - 80), 16);
}
/**
 * Draw the changing of the alarm time
 */
void drawSetAlarmView(boolean rotated) {
  char digits[12];
  sprintf(digits, "%02d:%02d%02d'    ", alarmHour, alarmMinute, alarmDuration);
  if (second() % 2 && !rotated) {
    switch (varToEdit) {
      case HOUR:
        digits[0] = ' ';
        digits[1] = ' ';
        break;
      case MINUTE:
        digits[3] = ' ';
        digits[4] = ' ';
        break;
      case DURATION:
        digits[5] = ' ';
        digits[6] = ' ';
        break;
    }
  }
  if (varToEdit == DURATION) {
    ssd1306_printFixedN(0, 36, digits + 5, STYLE_NORMAL, FONT_SIZE_4X);
  } else {
    sprintf(digits, "%.5s", digits);
    ssd1306_printFixedN(0, 36, digits, STYLE_NORMAL, FONT_SIZE_4X);
  }
}

/**
 * Draw the changing of the clock time
 */
void drawSetTimeView(boolean rotated) {
  char digits[5];
  sprintf(digits, "%02d:%02d", timeHour, timeMinute);
  if (second() % 2 && !rotated) {
    switch (varToEdit) {
      case HOUR:
        digits[0] = ' ';
        digits[1] = ' ';
        break;
      case MINUTE:
        digits[3] = ' ';
        digits[4] = ' ';
        break;
    }
  }
  ssd1306_printFixedN(0, 36, digits, STYLE_NORMAL, FONT_SIZE_4X);
}

/**
   Handles the functionality of changing a unit of the alarm
   @param encDir uses constants ENC_CW and ENC_CCW
   @param &var the variable to change (alarmHour, alarmMinute, alarmDuration)
   @param maxNumber 23 for hour, 59 for minute, maybe 30 for duration
*/
void handleAlarmTime(int8_t encDir, byte &var, byte maxNumber) {
  if (encDir == ENC_CW) {
    // code can be optimised better than modulo
    var = (var == maxNumber ? 0 : var + 1);
  } else if (encDir == ENC_CCW) {
    var = (var == 0 ? maxNumber : var - 1);
  }
}

/**
   Checks whether the rising alarm should be on at the moment
*/
boolean isAlarmOn() {
  if (alarmMinute >= alarmDuration) { 
    // minute handle doesn't pass 0 in the duration
    return (hour() == alarmHour
            && minute() >= (alarmMinute - alarmDuration)
            && minute() < alarmMinute);
  } else {
    byte startHour = alarmHour == 0 ? 23 : (alarmHour - 1);
    byte startMinute = 60 - (alarmDuration - alarmMinute);
    return ((hour() == startHour && minute() >= startMinute)
            || (hour() == alarmHour && minute() < alarmMinute));
  }
}