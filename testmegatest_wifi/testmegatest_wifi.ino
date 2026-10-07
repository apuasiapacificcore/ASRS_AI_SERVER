  #include <SoftwareSerial.h>

  // ===== HC-06 Bluetooth =====
  SoftwareSerial BTSerial(10, 11); // RX = 10, TX = 11

  // ===== Motor 1 (X) =====
  #define ENA1 24
  #define DIR1 22
  #define PUL1 2
  #define LIMIT1 31

  // ===== Motor 2 (Y) =====
  #define ENA2 25
  #define DIR2 23
  #define PUL2 3
  #define LIMIT2 39

  // ===== Motor 3 (Z) =====
  #define ENA3 44
  #define DIR3 47
  #define PUL3 4
  #define LIMIT3 35

  // ===== SIGNAL TO UNO =====
  #define UNO_TAKE_TRIGGER   40
  #define UNO_RETURN_TRIGGER 43
  #define UNO_DONE_PIN       38    // NEW: reads done-signal from UNO pin 4

  // ===== Conveyor =====
  #define CONVEYOR_EN 46
  #define CONVEYOR_DIR 48

  // Timing
  unsigned int stepInterval = 200; // us between pulses
  unsigned int pulseWidth   = 10;  // us HIGH time

  // Nudge amounts after reaching section
  const long Y_LIFT = 2500; // TAKE up
  const long Y_DROP = 5200; // RETURN down

  // Gantry exchange position — Z extends to meet the arm
  const long TAKE_POS_Z = 8500;
  const long GIVE_POS_Z = 9500;

  // ===== Section step positions =====
  long sectionSteps1[9] = {49200,36450,23600, 49200,36450,23600, 49200,36450,23600};  // X
  long sectionSteps2_take[9]   = {31500,31500,31500, 17700,17700,17700, 2500,2500,2500}; // Y TAKE
  long sectionSteps2_return[9] = {34000,34000,34000, 21000,21000,21000, 5700,5700,5700}; // Y RETURN
  long sectionSteps3[9] = {10400,10400,10500,10200,10200,10300,10050,10100,10200}; // Z

  // Current positions
  long currentPos1 = 0;
  long currentPos2 = 0;
  long currentPos3 = 0;

  // Timeout for waiting on UNO (safety fallback)
  #define UNO_TIMEOUT_MS 30000    // NEW: max 30s wait — if UNO hasn't signalled by then, something is wrong

  // Input buffer for serial/Bluetooth
  String inputBuffer = "";

  // ===== SEND TRIGGER TO UNO =====
  void sendTriggerToUNO(bool takeMode) {
    if (takeMode) {
      digitalWrite(UNO_TAKE_TRIGGER, HIGH);
      delay(200);                             // CHANGED: was 1000ms, 200ms is enough for UNO to detect
      digitalWrite(UNO_TAKE_TRIGGER, LOW);
    } else {
      digitalWrite(UNO_RETURN_TRIGGER, HIGH);
      delay(200);                             // CHANGED: was 1000ms
      digitalWrite(UNO_RETURN_TRIGGER, LOW);
    }
  }

  // ===== NEW: Wait for UNO to signal done via pin 38, with timeout =====
  bool waitForUNODone() {
    unsigned long startTime = millis();
    Serial.println("Waiting for UNO arm sequence...");

    while (digitalRead(UNO_DONE_PIN) == LOW) {
      if (millis() - startTime > UNO_TIMEOUT_MS) {
        Serial.println("WARNING: UNO timeout! Proceeding anyway.");
        return false;   // timed out
      }
    }

    unsigned long elapsed = millis() - startTime;
    Serial.println(String("UNO done in ") + elapsed + "ms");
    return true;        // success
  }

  void conveyorMove(int durationMs, bool forward = true) {
    digitalWrite(CONVEYOR_DIR, forward ? HIGH : LOW);
    digitalWrite(CONVEYOR_EN, HIGH);
    delay(durationMs);
    digitalWrite(CONVEYOR_EN, LOW);
    Serial.println(forward ? "Conveyor moved forward." : "Conveyor moved backward.");
  }

  void setup() {
    Serial.begin(9600);
    Serial1.begin(9600);
    BTSerial.begin(9600);

    // Motor pins
    pinMode(ENA1, OUTPUT); digitalWrite(ENA1, LOW);
    pinMode(DIR1, OUTPUT); pinMode(PUL1, OUTPUT); pinMode(LIMIT1, INPUT_PULLUP);

    pinMode(ENA2, OUTPUT); digitalWrite(ENA2, LOW);
    pinMode(DIR2, OUTPUT); pinMode(PUL2, OUTPUT); pinMode(LIMIT2, INPUT_PULLUP);

    pinMode(ENA3, OUTPUT); digitalWrite(ENA3, LOW);
    pinMode(DIR3, OUTPUT); pinMode(PUL3, OUTPUT); pinMode(LIMIT3, INPUT_PULLUP);

    // UNO signal pins
    pinMode(UNO_TAKE_TRIGGER, OUTPUT);
    pinMode(UNO_RETURN_TRIGGER, OUTPUT);
    pinMode(UNO_DONE_PIN, INPUT);             // NEW: read done-signal from UNO
    digitalWrite(UNO_TAKE_TRIGGER, LOW);
    digitalWrite(UNO_RETURN_TRIGGER, LOW);

    pinMode(CONVEYOR_EN, OUTPUT);
    pinMode(CONVEYOR_DIR, OUTPUT);
    digitalWrite(CONVEYOR_EN, LOW); // conveyor OFF

    Serial.println("Homing all motors...");
    homeMotor(3);  //Hand
    homeMotor(1);  //horizontal movement
    homeMotor(2);  //vertical hand
    Serial.println("Homing complete.");
  }

  void loop() {
    Serial.print("DEBUG: Entering Void loop ");
    String cmd = readCommand();
    if (cmd.length() > 0) {
      handleCommand(cmd);
    }
  }

  // ===== Read Serial + BT and trim \r\n automatically =====
  String readCommand() {
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') {
        String temp = inputBuffer;
        inputBuffer = "";
        temp.trim();
        return temp;
      } else inputBuffer += c;
    }

    while (BTSerial.available()) {
      char c = BTSerial.read();
      if (c == '\n' || c == '\r') {
        String temp = inputBuffer;
        inputBuffer = "";
        temp.trim();
        return temp;
      } else inputBuffer += c;
    }

    // Listen to the ESP32 via Serial1
    while (Serial1.available()) {
      char c = Serial1.read();
      if (c == '\n' || c == '\r') {
        String temp = inputBuffer;
        inputBuffer = "";
        temp.trim();
        return temp;
      } else inputBuffer += c;
    }
    return "";
  }

  // ===== Command handler =====
  void handleCommand(String cmd) {
    cmd.trim();
    if (cmd.length() == 0) return;

    Serial.println("DEBUG: The Arduino just heard -> ");
    Serial.println(cmd);

    bool isTake   = cmd.endsWith("_TAKE");
    bool isReturn = cmd.endsWith("_RETURN");

    if (isTake || isReturn) {
      String label = cmd;
      label.replace("_TAKE", "");
      label.replace("_RETURN", "");

      int idx = getSectionIndex(label);
      if (idx != -1) {

        Serial.println(String("Section ") + label + (isTake ? " (TAKE)" : " (RETURN)"));
        BTSerial.println("ACK " + cmd);
        Serial1.println("ACK " + cmd);

        if (isReturn) {
          // 1. SIMULTANEOUS: Conveyor backward + Gantry Z to take position
          digitalWrite(CONVEYOR_DIR, LOW);          // backward
          digitalWrite(CONVEYOR_EN, HIGH);           // start conveyor
          unsigned long convStart = millis();

          moveMotorTo(3, TAKE_POS_Z);           // extend hand (runs while conveyor also runs)

          // Wait for remaining conveyor time
          unsigned long elapsed = millis() - convStart;
          if (elapsed < 7500) delay(7500 - elapsed);
          digitalWrite(CONVEYOR_EN, LOW);            // stop conveyor
          Serial.println("Conveyor moved backward.");

          // 2. Arm picks from conveyor and places on gantry
          sendTriggerToUNO(false);
          waitForUNODone();

          // 3. Gantry back to normal position
          homeMotor(3);                              // retract hand

          // 4. Gantry to slot position
          moveMotorTo(1, sectionSteps1[idx]);
          moveMotorTo(2, sectionSteps2_return[idx]);
          moveMotorTo(3, sectionSteps3[idx]);

          moveMotorTo(2, currentPos2 - Y_DROP);

          // Return home
          homeMotor(3);
          homeMotor(1);
          homeMotor(2);
        }
        else {
          // ===== TAKE: keep old order =====
          moveMotorTo(1, sectionSteps1[idx]);
          moveMotorTo(2, sectionSteps2_take[idx]);
          moveMotorTo(3, sectionSteps3[idx]);

          moveMotorTo(2, currentPos2 + Y_LIFT);

          // Return home
          homeMotor(3);
          homeMotor(1);
          homeMotor(2);

          moveMotorTo(3, GIVE_POS_Z); // Extend Z to the exchange position

          // 3. Trigger UNO arm and wait for it to finish
          sendTriggerToUNO(true);
          waitForUNODone();

          // 4. SIMULTANEOUS: Run conveyor forward + return gantry to normal position
          digitalWrite(CONVEYOR_DIR, HIGH);          // forward direction
          digitalWrite(CONVEYOR_EN, HIGH);           // start conveyor
          unsigned long convStart = millis();

          homeMotor(3);                              // Retract hand to normal position while conveyor runs

          // Wait for the remainder of the 7450ms conveyor duration
          unsigned long elapsed = millis() - convStart;
          if (elapsed < 7450) delay(7450 - elapsed);
          digitalWrite(CONVEYOR_EN, LOW);            // stop conveyor
        }

        Serial.println("Done. Back home.");
        BTSerial.println("DONE " + cmd);
        Serial1.println("DONE " + cmd);
      }
    }
  }


  // ===== Motor functions =====
  void homeMotor(int motor) {

    Serial.println(String("entered Home motor, with motor: ")+motor);

    int dirPin, pulPin, limitPin;
    long *currentPos;
    bool homeDir;

    if (motor == 1) { dirPin = DIR1; pulPin = PUL1; limitPin = LIMIT1; currentPos = &currentPos1; homeDir = HIGH; }
    if (motor == 2) { dirPin = DIR2; pulPin = PUL2; limitPin = LIMIT2; currentPos = &currentPos2; homeDir = HIGH; }
    if (motor == 3) { dirPin = DIR3; pulPin = PUL3; limitPin = LIMIT3; currentPos = &currentPos3; homeDir = LOW; }

    digitalWrite(dirPin, homeDir);

    Serial.println(String("Inside Loop 1"));

    while (digitalRead(limitPin) == HIGH) stepSingle(pulPin);

    Serial.println(String("Inside Loop_2"));

    *currentPos = 0;
  }

  void moveMotorTo(int motor, long target) {
    int dirPin, pulPin;
    long *currentPos;

    if (motor == 1) { dirPin = DIR1; pulPin = PUL1; currentPos = &currentPos1; }
    if (motor == 2) { dirPin = DIR2; pulPin = PUL2; currentPos = &currentPos2; }
    if (motor == 3) { dirPin = DIR3; pulPin = PUL3; currentPos = &currentPos3; }

    long stepsToMove = target - *currentPos;

    if (motor == 3) {
      if (stepsToMove >= 0) digitalWrite(dirPin, HIGH);
      else { digitalWrite(dirPin, LOW); stepsToMove = -stepsToMove; }
    } else {
      if (stepsToMove >= 0) digitalWrite(dirPin, LOW);
      else { digitalWrite(dirPin, HIGH); stepsToMove = -stepsToMove; }
    }

    for (long i = 0; i < stepsToMove; i++) stepSingle(pulPin);
    *currentPos = target;
  }

  void stepSingle(int pulPin) {
    digitalWrite(pulPin, HIGH);
    delayMicroseconds(pulseWidth);
    digitalWrite(pulPin, LOW);
    delayMicroseconds(stepInterval);
  }

  // ===== Section lookup =====
  int getSectionIndex(String label) {
    String sections[] = {"A1","A2","A3","B1","B2","B3","C1","C2","C3"};
    for (int i = 0; i < 9; i++) if (label.equalsIgnoreCase(sections[i])) return i;
    return -1;
  }
