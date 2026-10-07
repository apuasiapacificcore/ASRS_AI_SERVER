#include <Braccio.h>
#include <Servo.h>


// ===== Braccio Servos =====
Servo base, shoulder, elbow, wrist_rot, wrist_ver, gripper;

#define TAKE_PIN 2
#define RETURN_PIN 8
#define DONE_PIN 4    // NEW: signals Mega that sequence is complete

void setup() {
  Serial.begin(9600);
  Braccio.begin();

  // wrist_ver.attach(5);

  pinMode(TAKE_PIN, INPUT);
  pinMode(RETURN_PIN, INPUT);
  pinMode(DONE_PIN, OUTPUT);       // NEW
  digitalWrite(DONE_PIN, LOW);     // NEW: start LOW (not done)

  homeBraccio();
}

void loop() {

    // === Serial test mode ===
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();

    if (cmd == "TAKE") {
      runTakeSequence();
    }
    else if (cmd == "RETURN") {
      runReturnSequence();
    }
    else if (cmd == "HOME") {
      homeBraccio();
    }
    else if (cmd.startsWith("POS")) {
      int vals[7];
      sscanf(cmd.c_str(), "POS %d %d %d %d %d %d %d",
            &vals[0], &vals[1], &vals[2], &vals[3],
            &vals[4], &vals[5], &vals[6]);
      Serial.println("Moving to: " + cmd);
      Braccio.ServoMovement(vals[0], vals[1], vals[2],
                            vals[3], vals[4], vals[5], vals[6]);
    }
    else if (cmd.length() >= 4 && cmd.charAt(0) == 'M'
             && cmd.charAt(1) >= '1' && cmd.charAt(1) <= '6') {
      // e.g. "M5 30" — move only that motor, keep others where they are
      int motor = cmd.charAt(1) - '0';   // 1-6
      int angle = 0;
      sscanf(cmd.c_str() + 2, "%d", &angle);
      angle = constrain(angle, 0, 180);

      // Read current positions from each servo
      int pos[6] = {
        base.read(), shoulder.read(), elbow.read(),
        wrist_rot.read(), wrist_ver.read(), gripper.read()
      };

      // Override the target motor
      pos[motor - 1] = angle;

      Serial.println("M" + String(motor) + " -> " + String(angle)
                     + "  (all: " + String(pos[0]) + " " + String(pos[1])
                     + " " + String(pos[2]) + " " + String(pos[3])
                     + " " + String(pos[4]) + " " + String(pos[5]) + ")");

      Braccio.ServoMovement(20, pos[0], pos[1], pos[2],
                                pos[3], pos[4], pos[5]);
    }
  }

  // Take button
  if (digitalRead(TAKE_PIN) == HIGH) {
    digitalWrite(DONE_PIN, LOW);   // NEW: clear done flag
    homeBraccio2();
    runTakeSequence();
    digitalWrite(DONE_PIN, HIGH);  // NEW: signal Mega we're done
    delay(500);
  }

  // Return button
  if (digitalRead(RETURN_PIN) == HIGH) {
    digitalWrite(DONE_PIN, LOW);   // NEW: clear done flag
    homeBraccio2();
    runReturnSequence();
    digitalWrite(DONE_PIN, HIGH);  // NEW: signal Mega we're done
    delay(500);
  }
  
}

void homeBraccio() {
  Braccio.ServoMovement(20, 90, 0, 90, 0, 160, 0); // safe pose
  delay(2000);
  Serial.println("Done Homing...");
}

void homeBraccio2() {
  Braccio.ServoMovement(20, 90, 0, 90, 0, 160, 0); // safe pose
  delay(500);
  Serial.println("Done Homing...");
}

void runTakeSequence() {
  Serial.println("Start");
  // simple grab sequence
  Braccio.ServoMovement(20, 83, 50, 60, 35, 85, 0); // First In between
  delay(1500);
  Braccio.ServoMovement(20, 83, 60, 60, 35, 85, 0); // Second In between
  delay(1500);
  Braccio.ServoMovement(20, 83, 60, 60, 35, 85, 73); // Grab
  delay(1500);
  Braccio.ServoMovement(20, 83, 0, 90, 0, 150, 73); // safe pos
  delay(1500);
  Braccio.ServoMovement(20, 20, 0, 90, 0, 150, 73); // Twist
  delay(1500);
  Braccio.ServoMovement(20, 20, 40, 30, 50, 150, 73); // Second in-between
  delay(1500);
  Braccio.ServoMovement(20, 20, 40, 30, 50, 150, 0); // relese
  delay(1500);
  Braccio.ServoMovement(20, 20, 0, 90, 0, 150, 0);
  delay(1500);

  homeBraccio2();

  Serial.println("DONE_TAKE"); // notify Mega
}

void runReturnSequence() {
  Serial.println("Start");
  Braccio.ServoMovement(20, 22, 0, 90, 0, 160, 0);
  delay(1500);
  Braccio.ServoMovement(20, 22, 30, 55, 20, 150, 0); // First in-between
  delay(700);
  Braccio.ServoMovement(20, 22, 45, 30, 40, 150, 0); // Second in-between
  delay(1500);
  Braccio.ServoMovement(20, 22, 45, 30, 40, 150, 73); // Close the gripper
  delay(1500);
  Braccio.ServoMovement(20, 22, 0, 90, 0, 150, 73);
  delay(1500);
  Braccio.ServoMovement(20, 90, 0, 90, 0, 150, 73); //Twist the head
  delay(1500);
  Braccio.ServoMovement(20, 80, 50, 60, 40, 85, 73); // First In between
  delay(1500);
  Braccio.ServoMovement(20, 80, 57, 60, 35, 85, 73); // Second In between
  delay(1500);
  Braccio.ServoMovement(20, 80, 57, 60, 35, 85, 0);  //Drop
  delay(1500);
  Braccio.ServoMovement(20, 90, 0, 90, 0, 160, 0);
  delay(1000);

  homeBraccio2();

  Serial.println("DONE_RETURN"); // notify Mega
}
