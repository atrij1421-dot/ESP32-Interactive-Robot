#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <XPT2046_Touchscreen.h>
#include <ESP32Servo.h>
#include <math.h>

// =====================================================
// TFT: VSPI
// =====================================================

#define TFT_CS    5
#define TFT_DC    2
#define TFT_RST   4

#define TFT_SCLK  18
#define TFT_MOSI  23
#define TFT_MISO  19

// =====================================================
// TOUCH: HSPI
// =====================================================

#define TOUCH_SCLK  25
#define TOUCH_MOSI  26
#define TOUCH_MISO  27

#define TOUCH_CS    14
// Touch is polled continuously over SPI; T_IRQ is not required by this sketch.

// =====================================================
// NECK SERVO AND MICROPHONE
// =====================================================

#define NECK_SERVO_PIN 13
#define LEFT_ARM_SERVO_PIN 21
#define RIGHT_ARM_SERVO_PIN 22

// Ground GPIO33 to force all three servos to their centre positions.
#define SERVO_CENTER_MODE_PIN 33

// HW-484 sound sensor
#define MIC_AO_PIN 34       // Analog sound level output
#define MIC_DO_PIN 35       // Digital threshold output (comparator)

// =====================================================
// SCREEN
// =====================================================

#define SCREEN_W 320
#define SCREEN_H 240

// =====================================================
// TOUCH CALIBRATION
// YOUR ORIGINAL VALUES
// =====================================================

#define TS_MINX 200
#define TS_MAXX 3800

#define TS_MINY 200
#define TS_MAXY 3800

#define SWAP_XY   false
#define INVERT_X  false
#define INVERT_Y  false

// =====================================================
// SPI OBJECTS
// =====================================================

SPIClass tftSPI(VSPI);
SPIClass touchSPI(HSPI);

// =====================================================
// TFT OBJECT
// =====================================================

Adafruit_ILI9341 tft(
  &tftSPI,
  TFT_DC,
  TFT_CS,
  TFT_RST
);

// =====================================================
// TOUCH OBJECT
// =====================================================

XPT2046_Touchscreen ts(TOUCH_CS);  // Polling mode: no IRQ wire required

// =====================================================
// NECK SERVO OBJECT AND MOVEMENT
// =====================================================

Servo neckServo;
Servo leftArmServo;
Servo rightArmServo;

float neckCurrentAngle = 90.0;
float neckTargetAngle = 90.0;
int neckLastWrittenAngle = 90;

// ARM SERVO STATE
float leftArmCurrent = 90.0;
float rightArmCurrent = 90.0;
float leftArmTarget = 90.0;
float rightArmTarget = 90.0;
int leftArmLastWritten = 90;
int rightArmLastWritten = 90;

unsigned long lastArmIdleMove = 0;
unsigned long nextArmIdleInterval = 3500;
unsigned long lastArmInvestigationMove = 0;
int armInvestigationStep = 0;


// =====================================================
// FACE SETTINGS
// =====================================================

#define BG_COLOR ILI9341_BLACK

#define EYE_W 72
#define EYE_H 90

#define LEFT_X  40
#define RIGHT_X 160
#define EYE_Y   25

#define CANVAS_W 280
#define CANVAS_H 140

#define CANVAS_X 20
#define CANVAS_Y 50

const uint16_t EYE_COLOR = 0x1E9F;

GFXcanvas16 canvas(CANVAS_W, CANVAS_H);

// =====================================================
// GAZE VARIABLES
// =====================================================

float gazeX = 0;
float gazeY = 0;

float targetX = 0;
float targetY = 0;

float smoothFactor = 0.15;

unsigned long lastGazeChange = 0;
unsigned long nextGazeInterval = 1800;

// =====================================================
// HW-484 SOUND SENSOR SETTINGS
// =====================================================

// Most LM393 sound modules pull DO LOW when sound crosses
// the potentiometer-set threshold. Change to HIGH only if
// your particular module behaves in the opposite way.
const int MIC_TRIGGER_LEVEL = LOW;

const unsigned long SOUND_COOLDOWN_MS = 1500;
const unsigned long SOUND_INVESTIGATION_MS = 10000;
const unsigned long MIC_DEBOUNCE_MS = 35;

bool investigatingSound = false;
unsigned long soundInvestigationStart = 0;
unsigned long lastSoundTrigger = 0;
unsigned long micLowSince = 0;
int lastMicAnalog = 0;
int investigationStep = 0;
unsigned long lastInvestigationMove = 0;

// =====================================================
// BLINK VARIABLES
// =====================================================

bool blinking = false;
bool doubleBlink = false;
bool secondBlink = false;

unsigned long blinkStart = 0;
unsigned long lastBlink = 0;

unsigned long nextBlinkInterval = 3500;

// =====================================================
// EXPRESSIONS
// =====================================================

enum Expression {
  NORMAL,
  SURPRISED,
  ANNOYED,
  ANGRY,
  HAPPY
};

Expression expression = NORMAL;

unsigned long expressionStart = 0;
unsigned long lastTouchTime = 0;

int touchCount = 0;

const unsigned long EXPRESSION_DURATION = 5000;

// =====================================================
// TOUCH VARIABLES
// =====================================================

bool wasTouched = false;

// Gesture tracking is independent of Y position because the reported
// touch Y coordinates may be differently oriented from the TFT coordinates.
bool gestureActive = false;
int swipeStartX = 0;
int swipeStartY = 0;
int swipeMinX = 0;
int swipeMaxX = 0;
unsigned long swipeStartTime = 0;
unsigned long lastTouchSampleTime = 0;

const int HAPPY_SWIPE_DISTANCE = 85;       // screen pixels
const unsigned long HAPPY_SWIPE_MAX_MS = 2500;
const unsigned long TOUCH_RELEASE_GRACE_MS = 140;

unsigned long lastRegisteredTap = 0;
bool hasRegisteredTap = false;
const unsigned long TOUCH_DEBOUNCE = 350;

// Eye-tap reaction: the tapped eye closes and the other looks annoyed.
bool eyeInjuryActive = false;
int injuredEye = -1; // 0 = left eye, 1 = right eye
unsigned long eyeInjuryStart = 0;
const unsigned long EYE_INJURY_DURATION_MS = 4500;

// Angry head-shake sequence: 45 -> 135 -> 45, repeated four times.
unsigned long angryMotionStart = 0;
int angryMotionStep = -1;       // -1 = not started, 0 = left, 1 = right, 2 = finished
int angryMotionCycles = 0;
bool angryWaitingAtEnd = false;
unsigned long angryPauseStart = 0;
const int ANGRY_LEFT_ANGLE = 45;
const int ANGRY_RIGHT_ANGLE = 135;
const int ANGRY_SHAKE_CYCLES = 4;
const unsigned long ANGRY_END_PAUSE_MS = 350;

// Centre-mode state. The input is active LOW (GPIO33 connected to GND).
bool servoCenterMode = false;

// =====================================================
// NECK SERVO MOVEMENT
// Defined after gaze and expression variables so all referenced
// variables are declared before this function is compiled.
// =====================================================

void resetAngryHeadShake() {
  angryMotionStep = -1;
  angryMotionCycles = 0;
  angryWaitingAtEnd = false;
  angryPauseStart = 0;
}

void updateNeckServo() {
  const unsigned long now = millis();

  if (investigatingSound) {
    neckTargetAngle = 90.0;
    resetAngryHeadShake();
  } else if (expression == ANGRY) {
    // Start a deliberate, visible NO gesture: 45 -> 135 -> 45, four cycles.
    if (angryMotionStep == -1) {
      angryMotionStep = 0;
      angryMotionCycles = 0;
      angryWaitingAtEnd = false;
      neckTargetAngle = ANGRY_LEFT_ANGLE;
      Serial.println("ANGRY: starting head shake (45 <-> 135 degrees)");
    }

    // Pause briefly only after the neck has actually reached an endpoint.
    if (angryMotionStep < 2 && !angryWaitingAtEnd &&
        fabs(neckCurrentAngle - neckTargetAngle) <= 2.5) {
      angryWaitingAtEnd = true;
      angryPauseStart = now;
    }

    if (angryWaitingAtEnd && now - angryPauseStart >= ANGRY_END_PAUSE_MS) {
      angryWaitingAtEnd = false;

      if (angryMotionStep == 0) {
        angryMotionStep = 1;
        neckTargetAngle = ANGRY_RIGHT_ANGLE;
      } else {
        angryMotionCycles++;
        if (angryMotionCycles >= ANGRY_SHAKE_CYCLES) {
          angryMotionStep = 2;
          neckTargetAngle = 90.0;
          Serial.println("ANGRY: head shake complete; returning to centre");
        } else {
          angryMotionStep = 0;
          neckTargetAngle = ANGRY_LEFT_ANGLE;
        }
      }
    }
  } else {
    resetAngryHeadShake();
    if (expression != NORMAL) {
      neckTargetAngle = 90.0;
    } else {
      // Idle head follows the eye gaze with a noticeable but smooth movement.
      neckTargetAngle = 90.0 + (gazeX * 0.85);
    }
  }

  neckTargetAngle = constrain(neckTargetAngle, 45.0, 135.0);
  const float movementFactor = (expression == ANGRY) ? 0.14 : 0.16;
  neckCurrentAngle += (neckTargetAngle - neckCurrentAngle) * movementFactor;

  int angleToWrite = constrain((int)(neckCurrentAngle + 0.5), 45, 135);
  if (angleToWrite != neckLastWrittenAngle) {
    neckServo.write(angleToWrite);
    neckLastWrittenAngle = angleToWrite;
  }
}

// =====================================================
// ARM SERVO MOVEMENT
// GPIO21 = left arm, GPIO22 = right arm
// =====================================================

void chooseIdleArmPose() {
  // Wider, clearly separated poses for the MG995 servos.
  // The 35-145 degree range is a conservative starting range;
  // check the mechanical linkage before increasing it further.
  int pose = random(0, 7);

  switch (pose) {
    case 0: leftArmTarget = 90;  rightArmTarget = 90;  break;
    case 1: leftArmTarget = 35;  rightArmTarget = 90;  break;
    case 2: leftArmTarget = 90;  rightArmTarget = 145; break;
    case 3: leftArmTarget = 145; rightArmTarget = 90;  break;
    case 4: leftArmTarget = 90;  rightArmTarget = 35;  break;
    case 5: leftArmTarget = 45;  rightArmTarget = 135; break;
    default:leftArmTarget = 135; rightArmTarget = 45;  break;
  }

  // Longer pause before the next idle arm gesture.
  nextArmIdleInterval = random(2800, 4800);
}

void updateArmServos() {
  unsigned long now = millis();

  if (investigatingSound) {
    if (now - lastArmInvestigationMove >= 2800) {
      lastArmInvestigationMove = now;
      armInvestigationStep = (armInvestigationStep + 1) % 5;
      switch (armInvestigationStep) {
        case 0: leftArmTarget = 40;  rightArmTarget = 140; break;
        case 1: leftArmTarget = 140; rightArmTarget = 40;  break;
        case 2: leftArmTarget = 90;  rightArmTarget = 90;  break;
        case 3: leftArmTarget = 35;  rightArmTarget = 90;  break;
        default:leftArmTarget = 90;  rightArmTarget = 145; break;
      }
    }
  } else if (expression == ANGRY) {
    // Stagger the three servo movements: neck first, then left arm,
    // then right arm. Small 6-degree motions reduce mechanical load.
    unsigned long phase = (now - angryMotionStart) % 3600;
    if (phase < 600) {              // neck moves; arms stay centred
      leftArmTarget = 90;
      rightArmTarget = 90;
    } else if (phase < 1200) {      // left arm moves first
      leftArmTarget = 96;
      rightArmTarget = 90;
    } else if (phase < 1800) {      // then right arm moves
      leftArmTarget = 96;
      rightArmTarget = 84;
    } else if (phase < 2400) {      // return left arm
      leftArmTarget = 90;
      rightArmTarget = 84;
    } else if (phase < 3000) {      // return right arm
      leftArmTarget = 90;
      rightArmTarget = 96;
    } else {                        // settle both arms
      leftArmTarget = 90;
      rightArmTarget = 90;
    }
  } else if (expression != NORMAL) {
    leftArmTarget = 90;
    rightArmTarget = 90;
  } else if (now - lastArmIdleMove >= nextArmIdleInterval) {
    lastArmIdleMove = now;
    chooseIdleArmPose();
  }

  // Slower interpolation during angry gestures; normal idle motion unchanged.
  float armSmoothing = (expression == ANGRY) ? 0.025 : 0.055;
  leftArmCurrent += (leftArmTarget - leftArmCurrent) * armSmoothing;
  rightArmCurrent += (rightArmTarget - rightArmCurrent) * armSmoothing;

  int leftAngle = constrain((int)(leftArmCurrent + 0.5), 35, 145);
  int rightAngle = constrain((int)(rightArmCurrent + 0.5), 35, 145);

  if (leftAngle != leftArmLastWritten) {
    leftArmServo.write(leftAngle);
    leftArmLastWritten = leftAngle;
  }
  if (rightAngle != rightArmLastWritten) {
    rightArmServo.write(rightAngle);
    rightArmLastWritten = rightAngle;
  }
}

// =====================================================
// HW-484 DIGITAL SOUND DETECTION
// =====================================================

void detectSound() {

  unsigned long now = millis();

  // AO is optional for detection, but useful for observing the
  // microphone signal in Serial Monitor.
  lastMicAnalog = analogRead(MIC_AO_PIN);

  bool soundActive = (digitalRead(MIC_DO_PIN) == MIC_TRIGGER_LEVEL);

  if (soundActive) {
    if (micLowSince == 0) {
      micLowSince = now;
    }
  } else {
    micLowSince = 0;
  }

  // Require the comparator output to remain active briefly to
  // reject tiny glitches.
  bool confirmedSound =
    (micLowSince != 0 && now - micLowSince >= MIC_DEBOUNCE_MS);

  static unsigned long lastMicPrint = 0;
  if (now - lastMicPrint >= 300) {
    lastMicPrint = now;
    Serial.print("HW-484 AO: ");
    Serial.print(lastMicAnalog);
    Serial.print(" | DO: ");
    Serial.print(digitalRead(MIC_DO_PIN));
    Serial.print(" | detecting: ");
    Serial.println(soundActive ? "YES" : "NO");
  }

  if (!investigatingSound &&
      confirmedSound &&
      now - lastSoundTrigger >= SOUND_COOLDOWN_MS) {

    investigatingSound = true;
    soundInvestigationStart = now;
    lastSoundTrigger = now;
    investigationStep = 0;
    lastInvestigationMove = 0;

    // Centre the eyes and command the neck servo straight ahead.
    targetX = 0;
    targetY = 0;
    neckTargetAngle = 90.0;

    Serial.println();
    Serial.println();
    Serial.println("ENTERING SHERLOCK HOLMES MODE");
    Serial.println("***** LOUD SOUND DETECTED! *****");
    Serial.println("Eyes narrowed; watery highlights activated.");
    Serial.println("Neck turning straight to 90 degrees.");
    Serial.println("Investigating for 10 seconds.");
  }
}

void updateSoundInvestigation() {

  if (!investigatingSound) return;

  unsigned long now = millis();

  if (now - soundInvestigationStart >= SOUND_INVESTIGATION_MS) {
    investigatingSound = false;
    targetX = 0;
    targetY = 0;
    lastGazeChange = now;
    Serial.println("Sound investigation finished. Returning to idle.");
    return;
  }

  // Gentle eye-search pattern while the head remains facing forward.
  if (now - lastInvestigationMove >= 1100) {
    lastInvestigationMove = now;
    investigationStep = (investigationStep + 1) % 5;

    switch (investigationStep) {
      case 0: targetX = -22; targetY = 0; break;
      case 1: targetX = -12; targetY = -12; break;
      case 2: targetX = 22;  targetY = 0; break;
      case 3: targetX = 12;  targetY = 12; break;
      case 4: targetX = 0;   targetY = 0; break;
    }
  }

  gazeX += (targetX - gazeX) * 0.12;
  gazeY += (targetY - gazeY) * 0.12;
}

// =====================================================
// TOUCH READING
// YOUR ORIGINAL FUNCTION
// =====================================================

bool readTouch(int &x, int &y) {

  if (!ts.touched()) {
    return false;
  }

  TS_Point p = ts.getPoint();

  int rawX = p.x;
  int rawY = p.y;

  if (SWAP_XY) {
    int temp = rawX;
    rawX = rawY;
    rawY = temp;
  }

  x = map(
    rawX,
    TS_MINX,
    TS_MAXX,
    0,
    SCREEN_W - 1
  );

  y = map(
    rawY,
    TS_MINY,
    TS_MAXY,
    0,
    SCREEN_H - 1
  );

  if (INVERT_X) {
    x = SCREEN_W - 1 - x;
  }

  if (INVERT_Y) {
    y = SCREEN_H - 1 - y;
  }

  x = constrain(x, 0, SCREEN_W - 1);
  y = constrain(y, 0, SCREEN_H - 1);

  return true;
}

// =====================================================
// DRAW EYE
// =====================================================

void drawEye(
  int x,
  int y,
  int width,
  int height,
  uint16_t color
) {

  canvas.fillRoundRect(
    x,
    y,
    width,
    height,
    23,
    color
  );
}

// =====================================================
// NORMAL FACE
// =====================================================

void drawNormalFace() {

  canvas.fillScreen(BG_COLOR);

  int eyeX1 = LEFT_X + (int)gazeX;
  int eyeX2 = RIGHT_X + (int)gazeX;

  int eyeY = EYE_Y + (int)gazeY;

  int eyeHeight = EYE_H;

  if (blinking) {

    float progress =
      (millis() - blinkStart) / 120.0;

    if (progress > 1.0) {
      progress = 1.0;
    }

    eyeHeight = max(
      5,
      (int)(EYE_H * fabs(1.0 - 2.0 * progress))
    );
  }

  drawEye(
    eyeX1,
    eyeY,
    EYE_W,
    eyeHeight,
    EYE_COLOR
  );

  drawEye(
    eyeX2,
    eyeY,
    EYE_W,
    eyeHeight,
    EYE_COLOR
  );
}

// =====================================================
// SURPRISED FACE
// =====================================================

void drawSurprisedFace() {

  canvas.fillScreen(BG_COLOR);

  drawEye(
    LEFT_X,
    EYE_Y - 8,
    EYE_W,
    EYE_H + 8,
    EYE_COLOR
  );

  drawEye(
    RIGHT_X,
    EYE_Y - 8,
    EYE_W,
    EYE_H + 8,
    EYE_COLOR
  );

  canvas.fillCircle(
    140,
    115,
    12,
    ILI9341_WHITE
  );
}

// =====================================================
// ANNOYED FACE
// =====================================================

void drawAnnoyedFace() {

  canvas.fillScreen(BG_COLOR);

  drawEye(
    LEFT_X,
    EYE_Y + 8,
    EYE_W,
    EYE_H - 8,
    EYE_COLOR
  );

  drawEye(
    RIGHT_X,
    EYE_Y + 8,
    EYE_W,
    EYE_H - 8,
    EYE_COLOR
  );

  canvas.drawLine(
    LEFT_X, 15,
    LEFT_X + 65, 30,
    ILI9341_WHITE
  );

  canvas.drawLine(
    RIGHT_X + 5, 30,
    RIGHT_X + 72, 15,
    ILI9341_WHITE
  );

  canvas.drawLine(
    120, 130,
    160, 130,
    ILI9341_WHITE
  );
}

// =====================================================
// HAPPY FACE - same rounded-square eye design, green colour
// =====================================================

void drawHappyFace() {
  canvas.fillScreen(BG_COLOR);
  const uint16_t happyGreen = ILI9341_GREEN;

  int eyeY = EYE_Y + (int)gazeY;
  int eyeX1 = LEFT_X + (int)gazeX;
  int eyeX2 = RIGHT_X + (int)gazeX;

  drawEye(eyeX1, eyeY, EYE_W, EYE_H, happyGreen);
  drawEye(eyeX2, eyeY, EYE_W, EYE_H, happyGreen);

  // Simple smiling mouth beneath the eyes.
  canvas.drawLine(112, 119, 124, 130, ILI9341_WHITE);
  canvas.drawLine(124, 130, 140, 135, ILI9341_WHITE);
  canvas.drawLine(140, 135, 156, 130, ILI9341_WHITE);
  canvas.drawLine(156, 130, 168, 119, ILI9341_WHITE);
}

// =====================================================
// SHERLOCK HOLMES MODE - triggered by a loud sound
// Narrow eyes with white watery highlights along their top edges.
// =====================================================

void drawSherlockFace() {
  canvas.fillScreen(BG_COLOR);

  const int narrowHeight = 40;
  const int eyeY = EYE_Y + 24;
  const int eyeX1 = LEFT_X + (int)gazeX;
  const int eyeX2 = RIGHT_X + (int)gazeX;

  drawEye(eyeX1, eyeY, EYE_W, narrowHeight, EYE_COLOR);
  drawEye(eyeX2, eyeY, EYE_W, narrowHeight, EYE_COLOR);

  // White watery patches/highlights sitting on the upper edge of each eye.
  canvas.fillRoundRect(eyeX1 + 15, eyeY + 2, 34, 7, 3, ILI9341_WHITE);
  canvas.fillRoundRect(eyeX2 + 15, eyeY + 2, 34, 7, 3, ILI9341_WHITE);
}

// =====================================================
// EYE INJURY FACE - one eye shut, the other eye annoyed
// injuredEye: 0 = left eye was tapped, 1 = right eye was tapped.
// =====================================================

void drawEyeInjuryFace() {
  canvas.fillScreen(BG_COLOR);
  const int eyeY = EYE_Y + 15;

  if (injuredEye == 0) {
    // Left eye closed.
    canvas.drawLine(LEFT_X + 8, eyeY + 38, LEFT_X + 64, eyeY + 38, EYE_COLOR);
    canvas.drawLine(LEFT_X + 13, eyeY + 42, LEFT_X + 59, eyeY + 42, EYE_COLOR);

    // Right eye open. Keep the annoyed eyebrow completely ABOVE the eye.
    drawEye(RIGHT_X, eyeY + 8, EYE_W, 58, EYE_COLOR);
    canvas.drawLine(RIGHT_X + 3, eyeY + 6, RIGHT_X + 68, eyeY + 1, ILI9341_WHITE);
  } else {
    // Right eye closed.
    canvas.drawLine(RIGHT_X + 8, eyeY + 38, RIGHT_X + 64, eyeY + 38, EYE_COLOR);
    canvas.drawLine(RIGHT_X + 13, eyeY + 42, RIGHT_X + 59, eyeY + 42, EYE_COLOR);

    // Left eye open. Keep the annoyed eyebrow completely ABOVE the eye.
    drawEye(LEFT_X, eyeY + 8, EYE_W, 58, EYE_COLOR);
    canvas.drawLine(LEFT_X + 3, eyeY + 1, LEFT_X + 68, eyeY + 6, ILI9341_WHITE);
  }
}

// =====================================================
// ANGRY FACE
// =====================================================

void drawAngryFace() {

  canvas.fillScreen(BG_COLOR);

  drawEye(
    LEFT_X,
    EYE_Y + 12,
    EYE_W,
    EYE_H - 15,
    ILI9341_RED
  );

  drawEye(
    RIGHT_X,
    EYE_Y + 12,
    EYE_W,
    EYE_H - 15,
    ILI9341_RED
  );

  canvas.drawLine(
    LEFT_X, 20,
    LEFT_X + 65, 42,
    ILI9341_RED
  );

  canvas.drawLine(
    RIGHT_X + 5, 42,
    RIGHT_X + 72, 20,
    ILI9341_RED
  );

  canvas.drawLine(
    120, 125,
    160, 125,
    ILI9341_RED
  );

  canvas.drawLine(
    120, 125,
    130, 135,
    ILI9341_RED
  );

  canvas.drawLine(
    160, 125,
    150, 135,
    ILI9341_RED
  );
}

// =====================================================
// UPDATE FACE
// =====================================================

void updateFace() {
  // Special reactions take priority over the normal expression system.
  if (eyeInjuryActive) {
    drawEyeInjuryFace();
  } else if (investigatingSound) {
    drawSherlockFace();
  } else {
    switch (expression) {
      case NORMAL:
        drawNormalFace();
        break;
      case SURPRISED:
        drawSurprisedFace();
        break;
      case ANNOYED:
        drawAnnoyedFace();
        break;
      case ANGRY:
        drawAngryFace();
        break;
      case HAPPY:
        drawHappyFace();
        break;
    }
  }

  tft.drawRGBBitmap(
    CANVAS_X,
    CANVAS_Y,
    canvas.getBuffer(),
    CANVAS_W,
    CANVAS_H
  );

  // Show the complaint at the bottom of the TFT while the eye is hurt.
  if (eyeInjuryActive) {
    tft.fillRect(0, 202, SCREEN_W, 38, BG_COLOR);
    tft.setTextSize(2);
    tft.setTextColor(ILI9341_WHITE, BG_COLOR);
    tft.setTextWrap(false);
    const char *message = "oww my eye!!!!";
    int16_t x1, y1;
    uint16_t textW, textH;
    tft.getTextBounds(message, 0, 0, &x1, &y1, &textW, &textH);
    tft.setCursor((SCREEN_W - textW) / 2, 212);
    tft.print(message);
  } else {
    tft.fillRect(0, 202, SCREEN_W, 38, BG_COLOR);
  }
}

// =====================================================
// GAZE MOVEMENT
// =====================================================

void updateGaze() {

  unsigned long now = millis();

  if (expression != NORMAL || investigatingSound) {
    return;
  }

  if (now - lastGazeChange >= nextGazeInterval) {

    int direction = random(0, 11);

    switch (direction) {

      case 0:
        targetX = 0;
        targetY = 0;
        break;

      case 1:
        targetX = -45;
        targetY = 0;
        break;

      case 2:
        targetX = 45;
        targetY = 0;
        break;

      case 3:
        targetX = 0;
        targetY = -25;
        break;

      case 4:
        targetX = 0;
        targetY = 25;
        break;

      case 5:
        targetX = -30;
        targetY = -15;
        break;

      case 6:
        targetX = 30;
        targetY = -15;
        break;

      case 7:
        targetX = -30;
        targetY = 15;
        break;

      case 8:
        targetX = 30;
        targetY = 15;
        break;

      case 9:
        targetX = -45;
        targetY = 10;
        break;

      case 10:
        targetX = 45;
        targetY = -10;
        break;
    }

    smoothFactor =
      random(0, 4) == 0 ? 0.24 : 0.15;

    lastGazeChange = now;

    nextGazeInterval = random(900, 2200);
  }

  gazeX += (targetX - gazeX) * smoothFactor;
  gazeY += (targetY - gazeY) * smoothFactor;
}

// =====================================================
// BLINKING
// =====================================================

void updateBlink() {

  unsigned long now = millis();

  if (expression != NORMAL) {
    return;
  }

  if (!blinking &&
      now - lastBlink >= nextBlinkInterval) {

    blinking = true;
    blinkStart = now;

    secondBlink = false;

    doubleBlink = random(0, 4) == 0;
  }

  if (blinking &&
      now - blinkStart >= 240) {

    if (doubleBlink && !secondBlink) {

      secondBlink = true;
      blinkStart = now;

    } else {

      blinking = false;

      lastBlink = now;

      nextBlinkInterval = random(2500, 5000);
    }
  }
}

// =====================================================
// HANDLE TOUCH
// =====================================================

void handleTouch() {
  int x = 0, y = 0;
  const unsigned long now = millis();
  const bool currentlyTouched = readTouch(x, y);

  // Start a gesture on the first valid sample.
  if (currentlyTouched) {
    if (!gestureActive) {
      gestureActive = true;
      swipeStartX = x;
      swipeStartY = y;
      swipeMinX = x;
      swipeMaxX = x;
      swipeStartTime = now;

      Serial.print("Touch start X: ");
      Serial.print(x);
      Serial.print(" Y: ");
      Serial.println(y);

    }

    // Keep tracking through the entire finger movement. Do not wait for
    // IRQ to go HIGH; XPT2046 coordinates are read continuously over SPI.
    if (x < swipeMinX) swipeMinX = x;
    if (x > swipeMaxX) swipeMaxX = x;
    lastTouchSampleTime = now;
    return;
  }

  // Some XPT2046 readings briefly disappear during a drag. Keep the
  // gesture alive for a short grace period instead of ending it immediately.
  if (!gestureActive || (now - lastTouchSampleTime < TOUCH_RELEASE_GRACE_MS)) {
    return;
  }

  // The finger has been released (or absent) long enough: classify once.
  gestureActive = false;
  const unsigned long duration = lastTouchSampleTime - swipeStartTime;
  const int horizontalDistance = swipeMaxX - swipeMinX;

  Serial.print("Touch end; horizontal travel: ");
  Serial.print(horizontalDistance);
  Serial.print(" px; duration: ");
  Serial.print(duration);
  Serial.println(" ms");

  // Swipe detection has priority and does not depend on the Y coordinate.
  if (horizontalDistance >= HAPPY_SWIPE_DISTANCE &&
      duration <= HAPPY_SWIPE_MAX_MS) {
    expression = HAPPY;
    expressionStart = now;
    touchCount = 0;
    targetX = 0;
    targetY = 0;
    neckTargetAngle = 90;
    leftArmTarget = 90;
    rightArmTarget = 90;
    Serial.println("HAPPY! Horizontal swipe detected. Green eyes!");
    return;
  }

  // A short, low-movement gesture is treated as a tap. Debounce taps only;
  // never debounce the beginning of a swipe.
  if (hasRegisteredTap && now - lastRegisteredTap < TOUCH_DEBOUNCE) {
    return;
  }
  lastRegisteredTap = now;
  hasRegisteredTap = true;

  // A short tap inside either eye closes that eye and annoys the other.
  // Approximate screen-coordinate eye areas: left x=60..132, right x=180..252.
  const bool shortTap = (duration <= 800 && horizontalDistance < 30);
  const bool eyeRow = (swipeStartY >= 70 && swipeStartY <= 175);

  // Touch X is mirrored relative to the displayed TFT on this setup,
  // so the physical left side produces the higher screen-coordinate range.
  // Keep the on-screen eye identity matched to the eye the user actually taps.
  if (shortTap && eyeRow &&
      swipeStartX >= 170 && swipeStartX <= 265) {
    eyeInjuryActive = true;
    injuredEye = 0;  // Physical LEFT eye was tapped: close the LEFT eye.
    eyeInjuryStart = now;
    expression = NORMAL;
    touchCount = 0;
    targetX = 0;
    targetY = 0;
    blinking = false;
    Serial.println("LEFT EYE TAPPED: closing left eye; right eye is annoyed!");
    return;
  }

  if (shortTap && eyeRow &&
      swipeStartX >= 50 && swipeStartX <= 145) {
    eyeInjuryActive = true;
    injuredEye = 1;  // Physical RIGHT eye was tapped: close the RIGHT eye.
    eyeInjuryStart = now;
    expression = NORMAL;
    touchCount = 0;
    targetX = 0;
    targetY = 0;
    blinking = false;
    Serial.println("RIGHT EYE TAPPED: closing right eye; left eye is annoyed!");
    return;
  }

  // Mouth / lower-area tap: preserve the original expression sequence.
  if (now - lastTouchTime > 3000) touchCount = 0;
  touchCount++;
  lastTouchTime = now;
  expressionStart = now;

  if (touchCount == 1) {
    expression = SURPRISED;
    Serial.println("SURPRISED!");
  } else if (touchCount <= 3) {
    expression = ANNOYED;
    Serial.println("ANNOYED!");
  } else {
    expression = ANGRY;
    angryMotionStart = now;
    Serial.println("ANGRY! Starting gentle head and arm shake.");
  }
}

// =====================================================
// EYE INJURY TIMER
// =====================================================

void updateEyeInjury() {
  if (eyeInjuryActive &&
      millis() - eyeInjuryStart >= EYE_INJURY_DURATION_MS) {
    eyeInjuryActive = false;
    injuredEye = -1;
    Serial.println("Eye recovered. Back to normal!");
  }
}

// =====================================================
// EXPRESSION TIMER
// =====================================================

void updateExpression() {

  if (expression == NORMAL) {
    return;
  }

  if (millis() - expressionStart >= EXPRESSION_DURATION) {

    expression = NORMAL;

    touchCount = 0;
    targetX = 0;
    targetY = 0;
    neckTargetAngle = 90;
    leftArmTarget = 90;
    rightArmTarget = 90;

    Serial.println("BACK TO NORMAL!");
  }
}

// =====================================================
// SERVO CENTRE MODE
// Ground GPIO33 to centre neck, left arm and right arm at 90 degrees.
// This overrides all automatic servo movements while the pin is LOW.
// =====================================================

bool updateServoCenterMode() {
  const bool requested = (digitalRead(SERVO_CENTER_MODE_PIN) == LOW);

  if (requested && !servoCenterMode) {
    servoCenterMode = true;
    Serial.println("SERVO CENTRE MODE ON: all servos -> 90 degrees");

    neckCurrentAngle = neckTargetAngle = 90.0;
    leftArmCurrent = leftArmTarget = 90.0;
    rightArmCurrent = rightArmTarget = 90.0;

    neckServo.write(90);
    leftArmServo.write(90);
    rightArmServo.write(90);

    neckLastWrittenAngle = 90;
    leftArmLastWritten = 90;
    rightArmLastWritten = 90;
    resetAngryHeadShake();
  }

  if (!requested && servoCenterMode) {
    servoCenterMode = false;
    Serial.println("SERVO CENTRE MODE OFF: automatic movement resumed");
    lastArmIdleMove = millis();
    nextArmIdleInterval = random(2800, 4800);
    lastGazeChange = millis();
  }

  return servoCenterMode;
}

// =====================================================
// SETUP
// =====================================================

void setup() {

  Serial.begin(115200);

  randomSeed(esp_random());

  Serial.println();
  Serial.println("ROBOT FACE STARTING...");
  Serial.println("TFT: VSPI");
  Serial.println("TOUCH: HSPI polling mode (IRQ not connected/used)");
  Serial.println("NECK SERVO: GPIO13");
  Serial.println("LEFT ARM SERVO: GPIO21");
  Serial.println("RIGHT ARM SERVO: GPIO22");
  Serial.println("SERVO CENTRE MODE: GPIO33 to GND");
  Serial.println("HW-484 MICROPHONE: AO=GPIO34, DO=GPIO35");

  pinMode(MIC_AO_PIN, INPUT);
  pinMode(MIC_DO_PIN, INPUT);
  pinMode(SERVO_CENTER_MODE_PIN, INPUT_PULLUP);
  analogReadResolution(12);
  analogSetPinAttenuation(MIC_AO_PIN, ADC_11db);

  // Initialize neck servo.
  // Power the servo from a separate regulated 5V supply.
  // Connect the external supply GND to ESP32 GND.
  neckServo.setPeriodHertz(50);
  neckServo.attach(NECK_SERVO_PIN, 500, 2400);
  neckServo.write(90);
  neckCurrentAngle = 90.0;
  neckTargetAngle = 90.0;
  neckLastWrittenAngle = 90;

  // Initialize left and right arm servos.
  leftArmServo.setPeriodHertz(50);
  leftArmServo.attach(LEFT_ARM_SERVO_PIN, 500, 2400);
  leftArmServo.write(90);

  rightArmServo.setPeriodHertz(50);
  rightArmServo.attach(RIGHT_ARM_SERVO_PIN, 500, 2400);
  rightArmServo.write(90);

  leftArmCurrent = leftArmTarget = 90.0;
  rightArmCurrent = rightArmTarget = 90.0;
  leftArmLastWritten = 90;
  rightArmLastWritten = 90;

  // Initialize TFT SPI
  tftSPI.begin(
    TFT_SCLK,
    TFT_MISO,
    TFT_MOSI,
    TFT_CS
  );

  // Initialize touch SPI
  touchSPI.begin(
    TOUCH_SCLK,
    TOUCH_MISO,
    TOUCH_MOSI,
    TOUCH_CS
  );

  // Initialize TFT
  tft.begin();

  tft.setRotation(1);

  tft.fillScreen(BG_COLOR);

  Serial.println("TFT initialized.");

  // IMPORTANT:
  // Explicitly assign your dedicated HSPI bus.
  ts.begin(touchSPI);

  ts.setRotation(1);

  Serial.println("Touch initialized on HSPI polling mode. Swipe detection checks horizontal travel at any Y position; IRQ is not required.");
  Serial.println("HW-484 digital sound threshold is adjusted with its blue potentiometer.");

  canvas.fillScreen(BG_COLOR);

  lastGazeChange = millis();
  lastBlink = millis();
  lastSoundTrigger = millis() - SOUND_COOLDOWN_MS;

  lastArmIdleMove = millis();
  nextArmIdleInterval = random(2500, 5500);

  nextGazeInterval = random(900, 2200);
  nextBlinkInterval = random(2500, 5000);

  updateFace();

  Serial.println("ROBOT FACE READY!");
}

// =====================================================
// MAIN LOOP
// =====================================================

void loop() {
  // Check the hardware override first so no other routine can move the servos.
  const bool centreModeActive = updateServoCenterMode();

  handleTouch();
  updateEyeInjury();
  updateExpression();

  detectSound();
  updateSoundInvestigation();
  updateGaze();
  updateBlink();

  if (!centreModeActive) {
    updateNeckServo();
    updateArmServos();
  }

  static unsigned long lastFrame = 0;
  if (millis() - lastFrame >= 20) {
    lastFrame = millis();
    updateFace();
  }
}
