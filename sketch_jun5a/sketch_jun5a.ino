#include <SPI.h>
#include <MFRC522.h>
#include <TFT_eSPI.h>
#include <ThreeWire.h>  
#include <RtcDS1302.h>
#include <SD.h>
#include <sqlite3.h>
#include <WiFi.h>
#include <WebServer.h>

// --- Network Credentials ---
const char* ssid = "SLT-Fiber-2.4G-AAFE";
const char* password = "9cb2fd657$";

// --- Pins ---
#define RST_PIN 40
#define SS_PIN  38
#define SPI_SCK  36
#define SPI_MISO 37
#define SPI_MOSI 35
#define RTC_CLK 8
#define RTC_DAT 14
#define RTC_RST 21
#define SD_SS_PIN 39
#define BUTTON_PIN 0 // BOOT button

// --- Objects ---
MFRC522 mfrc522(SS_PIN, RST_PIN); 
TFT_eSPI tft = TFT_eSPI();        
ThreeWire myWire(RTC_DAT, RTC_CLK, RTC_RST);
RtcDS1302<ThreeWire> Rtc(myWire);
WebServer server(80); 

// --- System State Variables ---
const char* db_file = "/sd/attendance.db"; 
sqlite3 *db; 
String jsonResponse = "";
int uiState = 0; // 0 = Scanner, 1 = Logs, 2 = Members
int lastSecond = -1;
int display_row = 0; 
#define FLIPPER_ORANGE tft.color565(255, 130, 0)

// Temporary variables for checking users
int current_member_id = 0;
String current_member_name = "";

// --- SQLite Callbacks ---
int display_db_callback(void *data, int argc, char **argv, char **azColName) {
  if (data != NULL) {
    tft.setCursor(5, 30 + (display_row * 15));
    tft.print(argv[0] ? argv[0] : "NULL"); 
    tft.print(" | ");
    tft.print(argv[1] ? argv[1] : "NULL"); 
    display_row++;
  }
  return 0; 
}

int check_member_callback(void *data, int argc, char **argv, char **azColName) {
  if (argc >= 2) {
    current_member_id = atoi(argv[0] ? argv[0] : "0");
    current_member_name = argv[1] ? argv[1] : "Unknown";
  }
  return 0;
}

int web_db_callback(void *data, int argc, char **argv, char **azColName) {
  if (jsonResponse.length() > 1) jsonResponse += ",";
  jsonResponse += "{";
  for (int i = 0; i < argc; i++) {
    jsonResponse += "\"" + String(azColName[i]) + "\":\"" + String(argv[i] ? argv[i] : "") + "\"";
    if (i < argc - 1) jsonResponse += ",";
  }
  jsonResponse += "}";
  return 0;
}

void executeSQL(const char* sql, bool printToScreen = false) {
  char *zErrMsg = 0;
  int rc = sqlite3_exec(db, sql, printToScreen ? display_db_callback : NULL, printToScreen ? (void*)1 : NULL, &zErrMsg);
  if (rc != SQLITE_OK) {
    Serial.printf("SQL error: %s\n", zErrMsg);
    sqlite3_free(zErrMsg);
  }
}

void setupDatabase() {
  if (!SD.begin(SD_SS_PIN, SPI)) return;
  sqlite3_initialize();
  if (sqlite3_open(db_file, &db)) return;

  // New Table Structures
  executeSQL("CREATE TABLE IF NOT EXISTS Member (member_id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT, rfid TEXT UNIQUE);");
  executeSQL("CREATE TABLE IF NOT EXISTS Log (log_id INTEGER PRIMARY KEY AUTOINCREMENT, member_id INTEGER, time TEXT, FOREIGN KEY(member_id) REFERENCES Member(member_id));");
}

// --- Web Server Endpoints ---
void handleGetLogs() {
  jsonResponse = "[";
  char *zErrMsg = 0;
  sqlite3_exec(db, "SELECT * FROM Log ORDER BY log_id DESC LIMIT 50;", web_db_callback, NULL, &zErrMsg);
  jsonResponse += "]";
  server.send(200, "application/json", jsonResponse);
}

void handleGetMembers() {
  jsonResponse = "[";
  char *zErrMsg = 0;
  sqlite3_exec(db, "SELECT * FROM Member;", web_db_callback, NULL, &zErrMsg);
  jsonResponse += "]";
  server.send(200, "application/json", jsonResponse);
}

void handleAddMember() {
  if (server.hasArg("name") && server.hasArg("rfid")) {
    char sql[150];
    snprintf(sql, sizeof(sql), "INSERT INTO Member (name, rfid) VALUES ('%s', '%s');", server.arg("name").c_str(), server.arg("rfid").c_str());
    executeSQL(sql); 
    server.send(200, "text/plain", "Success");
  } else {
    server.send(400, "text/plain", "Missing parameters");
  }
}

// --- Main Setup ---
void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 40); tft.print("Booting System...");

  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, SS_PIN);
  
  mfrc522.PCD_Init();
  Rtc.Begin();
  if (!Rtc.GetIsRunning()) Rtc.SetIsRunning(true);
  if (!Rtc.IsDateTimeValid()) Rtc.SetDateTime(RtcDateTime(__DATE__, __TIME__));

  setupDatabase();

  // Non-blocking Wi-Fi setup (Tries for 5 seconds, then gives up and boots offline)
  tft.fillRect(10, 40, 200, 20, TFT_BLACK);
  tft.setCursor(10, 40); tft.print("Checking Wi-Fi...");
  
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 10) {
    delay(500);
    attempts++;
  }

  // Only start Web Server if Wi-Fi succeeded
  if (WiFi.status() == WL_CONNECTED) {
    server.on("/logs", HTTP_GET, handleGetLogs);
    server.on("/members", HTTP_GET, handleGetMembers);
    server.on("/add_member", HTTP_POST, handleAddMember);
    server.begin();
  }

  resetScreen();
}

// --- Main Loop ---
void loop() {
  server.handleClient();
  checkButton();

  RtcDateTime now = Rtc.GetDateTime();
  if (now.Second() != lastSecond) {
    lastSecond = now.Second();
    if (uiState == 0) drawLiveClock(now); // Only draw clock on the scanner screen
  }

  // Only scan cards if we are on the main screen
  if (uiState == 0) {
    if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) return;

    // Convert UID to String
    String uidString = "";
    for (byte i = 0; i < mfrc522.uid.size; i++) {
      if(mfrc522.uid.uidByte[i] < 0x10) uidString += "0";
      uidString += String(mfrc522.uid.uidByte[i], HEX);
      if(i < mfrc522.uid.size - 1) uidString += " ";
    }
    uidString.toUpperCase();

    // Format current time
    char timeStr[25];
    snprintf(timeStr, sizeof(timeStr), "%04d-%02d-%02d %02d:%02d:%02d", now.Year(), now.Month(), now.Day(), now.Hour(), now.Minute(), now.Second());

    // Check if user exists in database
    current_member_id = 0;
    current_member_name = "";
    char checkSql[150];
    snprintf(checkSql, sizeof(checkSql), "SELECT member_id, name FROM Member WHERE rfid = '%s';", uidString.c_str());
    sqlite3_exec(db, checkSql, check_member_callback, NULL, NULL);

    tft.fillRect(0, 50, tft.width(), 120, TFT_BLACK); // Clear middle screen
    char insertSql[150];

    if (current_member_id > 0) {
      // --- KNOWN USER ---
      tft.setTextColor(TFT_GREEN, TFT_BLACK); 
      tft.setCursor(10, 60); tft.print("Scan Successful!");

      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.setCursor(10, 90); tft.printf("ID: %d", current_member_id);
      tft.setCursor(10, 110); tft.printf("Name: %s", current_member_name.c_str());
      
      tft.setTextColor(FLIPPER_ORANGE, TFT_BLACK);
      tft.setCursor(10, 140); tft.print(timeStr);

      // Log the scan with the member's ID
      snprintf(insertSql, sizeof(insertSql), "INSERT INTO Log (member_id, time) VALUES (%d, '%s');", current_member_id, timeStr);

    } else {
      // --- UNKNOWN USER ---
      tft.setTextColor(TFT_RED, TFT_BLACK); 
      tft.setCursor(10, 60); tft.print("Scan Failed");
      tft.setCursor(10, 80); tft.print("No User Found");

      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.setCursor(10, 110); tft.print("UID: " + uidString);

      tft.setTextColor(FLIPPER_ORANGE, TFT_BLACK);
      tft.setCursor(10, 140); tft.print(timeStr);

      // Log the scan, but leave member_id NULL
      snprintf(insertSql, sizeof(insertSql), "INSERT INTO Log (member_id, time) VALUES (NULL, '%s');", timeStr);
    }

    sqlite3_exec(db, insertSql, NULL, NULL, NULL);

    mfrc522.PICC_HaltA();
    
    // Keep server alive while pausing to show scan result
    unsigned long waitStart = millis();
    while (millis() - waitStart < 2500) {
      server.handleClient();
      delay(10);
    }
    resetScreen();
  }
}

// --- UI Logic & Drawing ---

void checkButton() {
  if (digitalRead(BUTTON_PIN) == LOW) {
    // Wait for the user to lift their finger off the button
    while(digitalRead(BUTTON_PIN) == LOW) {
      server.handleClient();
      delay(10);
    }
    
    // Cycle to the next screen (0 -> 1 -> 2 -> 0)
    uiState++;
    if (uiState > 2) uiState = 0;

    if (uiState == 0) resetScreen();
    else if (uiState == 1) showLogsScreen();
    else if (uiState == 2) showMembersScreen();
  }
}

void resetScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, tft.width(), 20, FLIPPER_ORANGE);
  tft.setTextColor(TFT_BLACK, FLIPPER_ORANGE);
  tft.setTextSize(2);
  tft.setCursor(5, 3);
  tft.print("RFID Scanner");

  // Draw Wi-Fi Status Bar
  tft.setTextSize(1);
  tft.setCursor(5, 25);
  if (WiFi.status() == WL_CONNECTED) {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.print("WiFi: Connected | IP: ");
    tft.print(WiFi.localIP());
  } else {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.print("WiFi: Offline (Local Storage)");
  }

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 80);
  tft.print("Ready to scan...");
}

void showLogsScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, tft.width(), 20, FLIPPER_ORANGE);
  tft.setTextColor(TFT_BLACK, FLIPPER_ORANGE);
  tft.setTextSize(2);
  tft.setCursor(5, 3); tft.print("Recent Logs");

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1); 
  display_row = 0;
  
  // Join the tables so we can show the Time and the User's Name
  executeSQL("SELECT Log.time, IFNULL(Member.name, 'UNKNOWN') FROM Log LEFT JOIN Member ON Log.member_id = Member.member_id ORDER BY log_id DESC LIMIT 10;", true);
}

void showMembersScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, tft.width(), 20, FLIPPER_ORANGE);
  tft.setTextColor(TFT_BLACK, FLIPPER_ORANGE);
  tft.setTextSize(2);
  tft.setCursor(5, 3); tft.print("Registered Members");

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1); 
  display_row = 0;
  
  executeSQL("SELECT member_id, name FROM Member LIMIT 10;", true);
}

void drawLiveClock(const RtcDateTime& dt) {
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(tft.width() - 55, 6); 
  
  if (dt.Hour() < 10) tft.print("0");
  tft.print(dt.Hour()); tft.print(":");
  if (dt.Minute() < 10) tft.print("0");
  tft.print(dt.Minute()); tft.print(":");
  if (dt.Second() < 10) tft.print("0");
  tft.print(dt.Second());
}