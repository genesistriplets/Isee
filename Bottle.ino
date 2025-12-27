/*
 * Arduino Bottle Recycler Controller
 * Controls Servos, Ultrasonics, LCD, Relay, and Serial Communication.
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Servo.h>

// --- PIN DEFINITIONS ---
#define SERVO1_PIN 2
#define ECHO1_PIN 3
#define TRIG1_PIN 4
#define ECHO2_PIN 5
#define TRIG2_PIN 6
#define SERVO2_PIN 7
#define RELAY_PIN 8
#define BUZZER_PIN 10
// Voltage sensor on D9 (Ignored for now)

// --- CONFIGURATION ---
const int SERVO_CLOSED_ANGLE = 0;   // Idle state
const int SERVO_OPEN_ANGLE = 90;    // Open state
const int SENSOR_THRESHOLD_CM = 15; // Detection distance
const unsigned long TRIGGER_CONFIRM_TIME = 1000; // Time object must persist to wake up
const unsigned long ACTIVE_TIMEOUT = 30000;      // 30 seconds timeout
const unsigned long REMOVE_ERR_TIMEOUT = 30000;  // Time before "Remove Material" error

// --- OBJECTS ---
LiquidCrystal_I2C lcd(0x27, 16, 2); // Adjust 0x27 if your address differs
Servo servo1;
Servo servo2;

// --- STATE MACHINE ---
enum SystemState {
  STATE_WAIT_CONN,
  STATE_IDLE,
  STATE_SENSING_VERIFY,
  STATE_ACTIVE_RUNNING,
  STATE_BOTTLE_PROCESS,
  STATE_ERROR_REMOVE
};

SystemState currentState = STATE_WAIT_CONN;

// --- VARIABLES ---
unsigned long stateTimer = 0;
unsigned long lastActivityTime = 0;
unsigned long lastToneTime = 0;
bool isConnected = false;
bool consecutiveBottle = false;
int consecutiveCount = 0;

// Music Notes
#define NOTE_E5 659
#define NOTE_E6 1319
#define NOTE_A6 1760

void setup() {
  Serial.begin(9600); // Communication with Python
  
  // Pin Modes
  pinMode(TRIG1_PIN, OUTPUT);
  pinMode(ECHO1_PIN, INPUT);
  pinMode(TRIG2_PIN, OUTPUT);
  pinMode(ECHO2_PIN, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  
  // Init Actuators
  digitalWrite(RELAY_PIN, LOW); // Lights OFF
  servo1.attach(SERVO1_PIN);
  servo2.attach(SERVO2_PIN);
  closeServos();

  // Init LCD
  lcd.init();
  lcd.backlight();
  
  updateLCD("Waiting for", "connection");
}

void loop() {
  // Check Serial Connection Heartbeat/Handshake
  checkSerialConnection();

  switch (currentState) {
    case STATE_WAIT_CONN:
      // Managed by checkSerialConnection
      break;

    case STATE_IDLE:
      // Monitor sensors to wake up system
      if (checkSensors()) {
        stateTimer = millis();
        playSenseTone();
        currentState = STATE_SENSING_VERIFY;
      }
      break;

    case STATE_SENSING_VERIFY:
      // Verify object persists for 1 second
      if (!checkSensors()) {
        currentState = STATE_IDLE; // False alarm
      } else if (millis() - stateTimer > TRIGGER_CONFIRM_TIME) {
        // Confirmed! Wake up Python
        wakeUpSystem();
      }
      break;

    case STATE_ACTIVE_RUNNING:
      // 1. Check for timeout (30s inactive)
      if (millis() - lastActivityTime > ACTIVE_TIMEOUT) {
        // If sensors are blocked but no bottle found for 30s -> ERROR
        if (checkSensors()) {
           currentState = STATE_ERROR_REMOVE;
           updateLCD("Remove", "Material");
           lastToneTime = millis();
        } else {
           goToSleep();
        }
      }
      
      // 2. Read Serial for "BOTTLE" command
      if (Serial.available() > 0) {
        String data = Serial.readStringUntil('\n');
        data.trim();
        if (data == "BOTTLE") {
          processBottle();
        }
      }
      
      // 3. Reset timer if sensors are still triggered (activity)
      if (checkSensors()) {
        // Note: We don't reset lastActivityTime constantly to prevent infinite loop 
        // if a non-bottle is stuck, but we check this in transition to ERROR.
      }
      break;

    case STATE_BOTTLE_PROCESS:
      // Blocking process handled in processBottle() function
      // Returns to ACTIVE_RUNNING automatically
      break;

    case STATE_ERROR_REMOVE:
      if (!checkSensors()) {
        // Material removed
        goToSleep();
      } else {
        // Play tone every 5 seconds
        if (millis() - lastToneTime > 5000) {
           playRemoveTone();
           lastToneTime = millis();
        }
      }
      break;
  }
}

// --- HELPER FUNCTIONS ---

void checkSerialConnection() {
  // Simple check: if we are in WAIT state and receive handshake
  if (currentState == STATE_WAIT_CONN) {
    if (Serial.available() > 0) {
      String msg = Serial.readStringUntil('\n');
      msg.trim();
      if (msg == "HANDSHAKE") {
        Serial.println("READY"); // Ack
        isConnected = true;
        currentState = STATE_IDLE;
        updateLCD("PySerial", "Connected");
        playOnTone();
        delay(1500); // Show message for a bit
        updateLCD("Insert", "Bottle"); // Changed from "System" "Idle"
      }
    }
  } else {
    // Optional: Logic to detect disconnection could go here
    // For now, we assume connection persists once made
  }
}

void wakeUpSystem() {
  digitalWrite(RELAY_PIN, HIGH); // Lights ON
  Serial.println("WAKE");        // Tell Python to load Camera
  lastActivityTime = millis();
  currentState = STATE_ACTIVE_RUNNING;
  updateLCD("Scanning...", "Insert Bottle");
}

void goToSleep() {
  digitalWrite(RELAY_PIN, LOW); // Lights OFF
  Serial.println("SLEEP");      // Tell Python to drop Camera
  consecutiveBottle = false;
  consecutiveCount = 0;
  currentState = STATE_IDLE;
  closeServos();
  updateLCD("Insert", "Bottle"); // Changed from "System" "Idle"
}

void processBottle() {
  lastActivityTime = millis(); // Reset timeout
  currentState = STATE_BOTTLE_PROCESS;

  // Servo Open
  openServos();
  
  // Audio & Visual
  if (consecutiveCount > 0) {
    updateLCD("Thank", "You"); // Same text, different tone
    playSuperThankYouTone();
  } else {
    updateLCD("Thank", "You");
    playThankYouTone();
  }
  
  delay(1500); // Wait for bottle to drop
  
  // Servo Close
  closeServos();
  
  // Post-process logic
  consecutiveCount++;
  
  // Wait for 2 seconds (Blind time where we ignore sensors/serial)
  delay(2000); 
  
  updateLCD("Insert", "more");

  // Flush Serial Buffer
  // This ensures that if the Python script sent a "BOTTLE" signal *during* // the 3.5 seconds (1.5s drop + 2s wait) above, it is DELETED here.
  // This prevents the system from triggering immediately again for the same bottle.
  while(Serial.available() > 0) {
    Serial.read();
  }
  
  currentState = STATE_ACTIVE_RUNNING;
}

bool checkSensors() {
  long duration1, distance1, duration2, distance2;

  // Sensor 1
  digitalWrite(TRIG1_PIN, LOW); delayMicroseconds(2);
  digitalWrite(TRIG1_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG1_PIN, LOW);
  duration1 = pulseIn(ECHO1_PIN, HIGH, 10000); // 10ms timeout
  distance1 = duration1 * 0.034 / 2;

  // Sensor 2
  digitalWrite(TRIG2_PIN, LOW); delayMicroseconds(2);
  digitalWrite(TRIG2_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG2_PIN, LOW);
  duration2 = pulseIn(ECHO2_PIN, HIGH, 10000); // 10ms timeout
  distance2 = duration2 * 0.034 / 2;

  // Valid detection if distance > 0 (not timeout) and < threshold
  bool s1 = (distance1 > 0 && distance1 < SENSOR_THRESHOLD_CM);
  bool s2 = (distance2 > 0 && distance2 < SENSOR_THRESHOLD_CM);

  return (s1 || s2);
}

// --- SERVO CONTROL ---
void openServos() {
  servo1.write(SERVO_OPEN_ANGLE);
  servo2.write(SERVO_OPEN_ANGLE);
}

void closeServos() {
  servo1.write(SERVO_CLOSED_ANGLE);
  servo2.write(SERVO_CLOSED_ANGLE);
}

// --- LCD HELPER ---
void updateLCD(String top, String bottom) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(top);
  lcd.setCursor(0, 1);
  lcd.print(bottom);
}

// --- TONE GENERATION ---
// Custom implementation of bendTones to simulate frequency sweep
// Note: "vol" parameter is ignored as digital pins are fixed voltage
void bendTones(float initFreq, float endFreq, float bending, float duration, int vol) {
  float currentFreq = initFreq;
  unsigned long startTime = millis();
  
  // Loop until duration is met
  while (millis() - startTime < (duration * 20)) { // Factor adjusted for loop overhead roughly
     tone(BUZZER_PIN, (int)currentFreq);
     
     if (initFreq < endFreq) {
       currentFreq *= bending;
     } else {
       currentFreq /= bending;
     }
     
     // Safety limits for buzzer
     if (currentFreq < 50) currentFreq = 50;
     if (currentFreq > 10000) currentFreq = 10000;
     
     delay(10); // Small step delay
  }
  noTone(BUZZER_PIN);
}

// Helper wrapper for single notes
void _tone(int note, int duration, int pause) {
  tone(BUZZER_PIN, note, duration);
  delay(duration + pause);
}

void playOnTone() {
  _tone(NOTE_E5, 50, 30);
  _tone(NOTE_E6, 55, 25);
  _tone(NOTE_A6, 60, 10);
}

void playSenseTone() {
    bendTones(800, 2150, 1.02, 10, 1);
    bendTones(2149, 800, 1.03, 7, 1);
}

void playThankYouTone() {
    bendTones(1500, 2500, 1.05, 20, 8);
    bendTones(2499, 1500, 1.05, 25, 8);
}

void playSuperThankYouTone() {
    bendTones(2000, 6000, 1.05, 8, 3);
    delay(50);
    bendTones(5999, 2000, 1.05, 13, 2);
}

void playRemoveTone() {
    bendTones(1000, 1700, 1.03, 8, 2);
    bendTones(1699, 500, 1.04, 8, 3);
    bendTones(1000, 1700, 1.05, 9, 10);
}
