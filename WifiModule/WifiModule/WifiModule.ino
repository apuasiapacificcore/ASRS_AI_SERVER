#include <WiFi.h>
#include <Firebase_ESP_Client.h>

//--- 1. NETWORK DETAILS ---
#define WIFI_SSID "Guest@APU"
#define WIFI_PASSWORD "Welcome2APU"

// #define WIFI_SSID "Student@APU"
// #define WIFI_PASSWORD "ThaAnkY0u"

// --- 2. FIREBASE DETAILS ---
#define FIREBASE_HOST "asrs-2020a-default-rtdb.asia-southeast1.firebasedatabase.app" 
#define FIREBASE_AUTH "Jfsjc5Q8XS0MIlbXdDv6HW1sACUwN3RThtHj8ruU"        

FirebaseData firebaseData;
FirebaseAuth auth;
FirebaseConfig config;

// Define Serial2 on pins 16 (RX) and 17 (TX)
HardwareSerial MegaSerial(2);

void setup() {
  // Start the serial port to talk to the Arduino Mega
  Serial.begin(115200);

  // Initialize communication with Arduino Mega
  // Syntax: begin(baud, config, rxPin, txPin)
  MegaSerial.begin(9600, SERIAL_8N1, 16, 17);

  // Connect to the local Wi-Fi router
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected!");

  // Assign Database URL and Database Secret (legacy token)
  config.database_url = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;

  // Connect to Firebase using the memory addresses of config & auth
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
}

void loop() {
  // --- PATH 1: Arduino Mega ---> ESP32 ---> Firebase (Status) ---
  if (MegaSerial.available()) {
    String megaMessage = MegaSerial.readStringUntil('\n');
    megaMessage.trim();
    if (megaMessage.length() > 0) {
      Firebase.RTDB.setString(&firebaseData, "/ASRS/Status", megaMessage);
      Serial.println("Pushed status to Firebase: " + megaMessage);
    }
  }

  // --- PATH 2: Firebase ---> ESP32 ---> Arduino Mega ---
  if (Firebase.RTDB.getString(&firebaseData, "/ASRS/AICommand")) {
    if (firebaseData.dataType() == "string") {
      String command = firebaseData.stringData();
      
      if (command.length() > 0 && command != "IDLE") {
        Serial.print("Successfully read command from Firebase: ");
        Serial.println(command);

        // FORWARD COMMAND TO MEGA: Send it over HardwareSerial (pins 16 & 17)
        MegaSerial.println(command);

        // Reset Firebase command back to IDLE so it doesn't loop forever
        Firebase.RTDB.setString(&firebaseData, "/ASRS/AICommand", "IDLE");
        Firebase.RTDB.setString(&firebaseData, "/ASRS/UnityCommand", "IDLE");
      }
    }
  }
   
  else {
    // Print error if it fails to read from Firebase
    // Serial.println("FAILED: " + firebaseData.errorReason());
  }
  
  delay(1000); // Small delay to prevent flooding Firebase requests
}















// #include <WiFi.h>

// void setup() {
//   Serial.begin(115200);
//   delay(1000);
  
//   // Turn on the WiFi radio in Station Mode
//   WiFi.mode(WIFI_STA);
//   // Give the radio a moment to fully initialize
//   delay(500); 
  
//   Serial.println();
//   Serial.print("My ESP32 MAC Address is: ");
//   Serial.println(WiFi.macAddress());
// }

// void loop() {}