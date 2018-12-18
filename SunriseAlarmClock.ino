// -----
// SunriseAlarmClock.ino - Prototye software for a sunrise alarm clock / dimmable bedside lamp.
// This sketch is implemented for use with the Arduino environment.
// Copyright (c) by Christoph Reinsch
// -----
// 18.12.2018 created by Christoph Reinsch
// -----

// What this software does:
// - use a rotary encoder with button to set the alarm time and duration
//   - single click to toggle light on/off, rotate to set brightness
//   - double click to switch alarm on/off
//   - long press to toggle alarm edit mode, single click to switch the unit to edit, rotate to set value
//   - double click in alarm edit mode to enter time edit mode
//   - long press to exit edit modes
// - use an AC pwm dimmer module to control any dimmable 230V bulb
// - slowly dim the lamp for the set duration until full brightness before the set alarm time (following an exponential curve)

// Rotary encoder setup:
// Attach a pins CLK and DT to A2 and A3.
// Attach SW pin to digital pin 7 (SW_PIN)
// GND to GND, + to 3.3V

// OLED setup (I2C):
// SCL, SDA to SCL, SDA
// GND to GND, VCC to 3.3V

// AC Dimmer Setup
// PWM to Pin 4 (AC_LOAD)
// ZeroCrossing to Pin 2 (hardcoded in RBDDimmer.h due to hardware timer)
// GND to GND, VCC to 3.3V

#include <RotaryEncoder.h> // https://github.com/mathertel/RotaryEncoder
#include <RBDdimmer.h> // https://github.com/RobotDynOfficial/Lib-RBD-Dimmer-for-Mega-UNO-Leonardo
#include <Time.h>      // https://github.com/PaulStoffregen/Time/blob/master/Time.cpp
#include <TimeLib.h>   // https://github.com/PaulStoffregen/Time/blob/master/TimeLib.h
#include <OneButton.h> // https://github.com/mathertel/OneButton
#include "ssd1306.h"   // https://github.com/adafruit/Adafruit_SSD1306
#include "nano_gfx.h"  // https://github.com/adafruit/Adafruit_SSD1306

#define SW_PIN 7 // Pin of Encoder Button
#define ENC_PIN_A 5 // Pin CLK of Encoder
#define ENC_PIN_B 6 // Pin DT of Encoder

#define AC_LOAD 4   // Output to Opto Triac pin

#define HOUR 1 // enum for setting time
#define MINUTE 2 // enum for setting time
#define DURATION 3 // enum for setting time

#define MINUTESPERHOUR 60
#define HOURSPERDAY 24
// (MAXDURATION-1) * CHANGESPERMINUTE bytes are needed during runtime, so be cautious 
#define MAXDURATION 11 // maximum duration of the alarm + 1
#define CHANGESPERMINUTE 12 // how many steps the light is making in rising, please only use whole divisors of 60

// 150 is just my prefence for dimming speed control
#define ENCODERSPEED 150

//////////// Internal (functionality) states
boolean alarmEnabled = true; // if the alarm is primed
boolean lightOn = false; // if the light is on

//////////// UI states
enum uiStates {
  mainView,
  setAlarmView,
  setTimeView
};
uiStates uiState = mainView;

//////////// Variables being changed by the user
int8_t timeHour = 0; // 0 - 23
int8_t timeMinute = 0; // 0 - 59
int8_t alarmHour = 0; // 0 - 23
int8_t alarmMinute = 2; // 0 - 59
int8_t alarmDuration = 2; // 0 - 10
int8_t lightIntensity = 95; // Dimming level (5-95)  5 = OFF, 95 = ON

//////////// Internal Variables
// store the dimming values for every CHANGESPERMINUTE
uint8_t alarmValues[(MAXDURATION-1)*CHANGESPERMINUTE];
uint8_t alarmValueIter = 0;
byte varToEdit = HOUR; // HOUR | MINUTE | DURATION

//////////// Library variables
time_t time;
OneButton encoderBtn(SW_PIN, true);
RotaryEncoder encoder(ENC_PIN_A, ENC_PIN_B);
dimmerLamp dimmer(AC_LOAD); //initialise port for dimmer

//////////// Loop Variables
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
  Serial.begin(9600);
  Serial.println("Prototype software for a sunrise alarm clock / dimmable bedside lamp");
  pinMode(SW_PIN, INPUT_PULLUP);

  // Initialize the dimming module
  dimmer.begin(NORMAL_MODE, OFF);
  dimmer.setPower(lightIntensity);


  // Initialize OLED screen
  ssd1306_128x64_i2c_init();
  ssd1306_fillScreen( 0x00 );
  ssd1306_setFixedFont(ssd1306xled_font6x8);

  // Initialize OneButton.h functionality for encoder button
  encoderBtn.attachClick(toggleLightOn);
  encoderBtn.attachDoubleClick(toggleAlarmEnabled);
  encoderBtn.attachLongPressStop(switchToSetAlarmView);

  // calculate the dimming value every 10 seconds
  for (int i = 0; i < alarmDuration*CHANGESPERMINUTE; i++) {
    alarmValues[i] = dimValue(i);
  }
}

/**
   A single click function of the encoder button
*/
void toggleLightOn () {
  lightOn = !lightOn;
  dimmer.changeState();
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
  if (uiState == setAlarmView) {
    // recalculate alarmValues array
    for (int i = 0; i < alarmDuration * CHANGESPERMINUTE; i++) {
      alarmValues[i] = dimValue(i);
    }
  }
  uiState = mainView;
  varToEdit = HOUR;
  encoderBtn.attachClick(toggleLightOn);
  encoderBtn.attachDoubleClick(toggleAlarmEnabled);
  encoderBtn.attachLongPressStop(switchToSetAlarmView);
}

void loop() {
  static uint32_t encLastMillis;         // last time read
  static uint16_t encSpeed;              // last calculated speed in clicks/second
  static uint32_t lastMillis;     // last time read
  static int8_t lastAlarmCheckSecond;
  int8_t loopSecond = second();
  encoderBtn.tick(); // check state of encoder Button
  encoder.tick();
  int8_t encDir = encoder.getDirection();

  // Persist the last direction so the UI doesn't flash rapidly when the encoder is turned,
  // but the variable to set isn't displayed because it's an odd second
  if (encDir) {
    // also calculate the speed of the encoder
    encSpeed = (1.0 / (millis() - encLastMillis)) * ENCODERSPEED;
    encLastMillis = millis();
  }

  if (lightOn && encDir && uiState == mainView) {
    // change brightness of light when on main view
    lightIntensity += encDir * encSpeed;
    if (lightIntensity > 95) lightIntensity = 95; else if (lightIntensity < 5) lightIntensity = 5;
    dimmer.setPower(lightIntensity);
  }

  if (uiState == setAlarmView) { // handle changing of the alarm time
    switch (varToEdit) {
      case HOUR:
        handleAlarmTime(encDir, alarmHour, HOURSPERDAY);
        break;
      case MINUTE:
        handleAlarmTime(encDir, alarmMinute, MINUTESPERHOUR);
        break;
      case DURATION:
        handleAlarmTime(encDir, alarmDuration, MAXDURATION);
        break;
    }
  }

  if (uiState == setTimeView) { // handle changing of the time
    switch (varToEdit) {
      case HOUR:
        handleAlarmTime(encDir, timeHour, HOURSPERDAY);
        break;
      case MINUTE:
        handleAlarmTime(encDir, timeMinute, MINUTESPERHOUR);
        break;
    }
  }

  if (((loopSecond - lastAlarmCheckSecond + SECS_PER_MIN) % SECS_PER_MIN > (SECS_PER_MIN/CHANGESPERMINUTE)) && alarmEnabled) {
    // handle light increasing if alarm is on
    static bool alarmOn;
    lastAlarmCheckSecond = loopSecond;
    if (alarmOn) {
      lightIntensity = alarmValues[alarmValueIter];
      dimmer.setPower(lightIntensity);
      alarmValueIter++;
      if (alarmValueIter > alarmDuration * CHANGESPERMINUTE) { // alarm is finished
        alarmOn = false;
        alarmValueIter = 0;
        dimmer.setPower(95);
      }
    } else if (isAlarmOn()) { // alarm just went on
      alarmOn = true;
      lightOn = true;
      lightIntensity = alarmValues[alarmValueIter];
      alarmValueIter++;
      dimmer.setPower(lightIntensity);
      dimmer.setState(ON);
    }
  }

  // UI stuff
  if (millis() - lastMillis > 250) { //draw 4 frames per second
    bool rotated = (millis() - encLastMillis) < 500;
    if (rotated && uiState != mainView) {
      // delay automatic switching to main view if rotated
      lastSecond = loopSecond;
    }
    if (uiState != mainView && ((loopSecond - lastSecond) + SECS_PER_MIN) % SECS_PER_MIN > 15) {
      // switch back to main view after 15 seconds of idling in timeState or setAlarm
      switchToMainView(); 
    }
    if (uiState == setAlarmView) {
      drawSetAlarmView(loopSecond, rotated);
    } else if (uiState == setTimeView) {
      drawSetTimeView(loopSecond, rotated);
    } else {
      drawMainView(loopSecond);
    }
    drawHeader();
    lastMillis = millis();
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
void drawMainView(int8_t loopSecond) {
  char digits[5];

  // Draw the current time
  if (loopSecond % 2) {
    sprintf(digits, "%02d:%02d", hour(), minute());
  } else {
    sprintf(digits, "%02d %02d", hour(), minute());
  }
  ssd1306_printFixedN(0, 36, digits, STYLE_NORMAL, FONT_SIZE_4X);

  // Draw the lightIntensity
  // uint8_t rectWidth = 4;
  // uint8_t rectHeight = 48;
  // uint8_t intensityRectHeight = rectHeight * (lightIntensity / 100);
  // uint8_t intensityRectOffsetY = rectHeight - intensityRectHeight;
  // uint8_t buffer[rectWidth * rectHeight / 8];
  // NanoCanvas canvas(rectWidth, rectHeight, buffer);
  // canvas.clear();
  // // canvas.setOffset(intensityRectOffsetY, 0);
  // canvas.fillRect(124, 16, 4, 16 + intensityRectHeight, 0xFF);
  // canvas.blt((ssd1306_displayWidth() - 80), 16);
}

/**
 * Draw the changing of the alarm time
 */
void drawSetAlarmView(int8_t loopSecond, boolean rotated) {
  char digits[12];
  sprintf(digits, "%02d:%02d%02d'    ", alarmHour, alarmMinute, alarmDuration);
  if (loopSecond % 2 && !rotated) {
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
void drawSetTimeView(int8_t loopSecond, boolean rotated) {
  char digits[5];
  sprintf(digits, "%02d:%02d", timeHour, timeMinute);
  if (loopSecond % 2 && !rotated) {
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
   @param encDir -1 || 0 || +1
   @param &var the variable to change (alarmHour, alarmMinute, alarmDuration)
   @param maxNumber 24 for hour, 60 for minute, maybe 10 for duration
*/
void handleAlarmTime(int8_t encDir, int8_t &var, byte maxNumber) {
  var += encDir;
  if (var == maxNumber)
    var = 0;
  else if (var == -1)
    var = maxNumber;
}

/**
   Checks whether the rising alarm should be on at the moment
*/
boolean isAlarmOn() {
  return ((alarmHour - hour() + HOURSPERDAY) % HOURSPERDAY <= 1) && 
    ((alarmMinute - minute() + MINUTESPERHOUR) % MINUTESPERHOUR < alarmDuration);
}

/**
 * Calculate the dimming value for a time step
 * @param i the time step
 */
int8_t dimValue(int i) {
  return (int) 5 * pow(pow(19.0, 1.0/(alarmDuration*CHANGESPERMINUTE)), i);
}