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
#define BUTTON_PIN 3  // Set back to your external button on GPIO 3
#define BUZZER_PIN 4  // NEW: Connect your buzzer's positive leg here!

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
int uiState = 0; // 0 = Dashboard, 1 = Logs, 2 = Members
int lastSecond = -1;
int display_row = 0; 
#define FLIPPER_ORANGE tft.color565(255, 130, 0)

// Temporary variables for checking users
String current_member_id = "";
String current_member_name = "";

// --- SQLite Callbacks ---
int display_db_callback(void *data, int argc, char **argv, char **azColName) {
  if (data != NULL) {
    tft.setCursor(5, 35 + (display_row * 15));
    tft.print(argv[0] ? argv[0] : "NULL"); 
    tft.print(" | ");
    tft.print(argv[1] ? argv[1] : "NULL"); 
    display_row++;
  }
  return 0; 
}

int check_member_callback(void *data, int argc, char **argv, char **azColName) {
  if (argc >= 2) {
    current_member_id = argv[0] ? argv[0] : "";
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

  executeSQL("CREATE TABLE IF NOT EXISTS Member (member_id TEXT PRIMARY KEY, name TEXT, rfid TEXT UNIQUE);");
  executeSQL("CREATE TABLE IF NOT EXISTS Log (member_id TEXT, time TEXT);");
}

// --- Web Server Endpoints ---
void handleResetDB() {
  executeSQL("DROP TABLE IF EXISTS Log;");
  executeSQL("DROP TABLE IF EXISTS Member;");
  setupDatabase(); 
  server.send(200, "text/plain", "DATABASE RESET SUCCESSFUL! You can now add members and logs.");
}

void handleGetLogs() {
  jsonResponse = "[";
  char *zErrMsg = 0;
  sqlite3_exec(db, "SELECT * FROM Log ORDER BY time DESC LIMIT 50;", web_db_callback, NULL, &zErrMsg);
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
  if (server.hasArg("member_id") && server.hasArg("name") && server.hasArg("rfid")) {
    String m_id = server.arg("member_id");
    String m_name = server.arg("name");
    String m_rfid = server.arg("rfid");

    char sql[200];
    snprintf(sql, sizeof(sql), "INSERT INTO Member (member_id, name, rfid) VALUES ('%s', '%s', '%s');", 
             m_id.c_str(), m_name.c_str(), m_rfid.c_str());
    
    char *zErrMsg = 0;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &zErrMsg);
    
    if (rc != SQLITE_OK) {
      server.send(500, "text/plain", String("Error: ") + zErrMsg);
      sqlite3_free(zErrMsg);
    } else {
      server.send(200, "text/plain", "Success");
    }
  } else {
    server.send(400, "text/plain", "Missing parameters");
  }
}

void handleDeleteMember() {
  if (server.hasArg("member_id")) {
    String id = server.arg("member_id");
    char sql[200];
    snprintf(sql, sizeof(sql), "DELETE FROM Log WHERE member_id = '%s';", id.c_str());
    executeSQL(sql); 
    snprintf(sql, sizeof(sql), "DELETE FROM Member WHERE member_id = '%s';", id.c_str());
    executeSQL(sql); 
    server.send(200, "text/plain", "Deleted Successfully");
  } else {
    server.send(400, "text/plain", "Missing parameters");
  }
}

// --- Main Setup ---
void setup() {
  Serial.begin(115200);
  
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW); // Ensure buzzer is off on boot

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
 Rtc.SetDateTime(RtcDateTime(__DATE__, __TIME__));

  setupDatabase();

  tft.fillRect(10, 40, 200, 20, TFT_BLACK);
  tft.setCursor(10, 40); tft.print("Checking Wi-Fi...");
  
  WiFi.setTxPower(WIFI_POWER_8_5dBm); // Helps prevent power brownouts
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 10) {
    delay(500);
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    server.on("/logs", HTTP_GET, handleGetLogs);
    server.on("/members", HTTP_GET, handleGetMembers);
    server.on("/add_member", HTTP_POST, handleAddMember);
    server.on("/delete_member", HTTP_POST, handleDeleteMember);
    server.on("/reset_db", HTTP_GET, handleResetDB); 
    server.begin();
  }

  drawDashboardInit();
}

// --- Main Loop ---
void loop() {
  server.handleClient();
  checkButton();

  RtcDateTime now = Rtc.GetDateTime();
  if (now.Second() != lastSecond) {
    lastSecond = now.Second();
    if (uiState == 0) updateDashboardClock(now); 
  }

  // Only allow scanning when on the main Dashboard
  if (uiState == 0) {
    if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) return;

    String uidString = "";
    for (byte i = 0; i < mfrc522.uid.size; i++) {
      if(mfrc522.uid.uidByte[i] < 0x10) uidString += "0";
      uidString += String(mfrc522.uid.uidByte[i], HEX);
      if(i < mfrc522.uid.size - 1) uidString += " ";
    }
    uidString.toUpperCase();

    char timeStr[25];
    snprintf(timeStr, sizeof(timeStr), "%04d-%02d-%02d %02d:%02d:%02d", now.Year(), now.Month(), now.Day(), now.Hour(), now.Minute(), now.Second());

    current_member_id = "";
    current_member_name = "";
    char checkSql[150];
    snprintf(checkSql, sizeof(checkSql), "SELECT member_id, name FROM Member WHERE rfid = '%s';", uidString.c_str());
    sqlite3_exec(db, checkSql, check_member_callback, NULL, NULL);

    char insertSql[250];

    if (current_member_id != "") {
      // --- KNOWN USER ---
      drawScanResult(true, uidString, current_member_id, current_member_name, timeStr);
      snprintf(insertSql, sizeof(insertSql), "INSERT INTO Log (member_id, time) VALUES ('%s', '%s');", current_member_id.c_str(), timeStr);
      sqlite3_exec(db, insertSql, NULL, NULL, NULL);
      
      // Beep for 200ms, then wait total 3 seconds
      buzzAndDelay(200, 3000); 

    } else {
      // --- UNKNOWN USER ---
      drawScanResult(false, uidString, "", "", timeStr);
      snprintf(insertSql, sizeof(insertSql), "INSERT INTO Log (member_id, time) VALUES ('%s', '%s');", uidString.c_str(), timeStr);
      sqlite3_exec(db, insertSql, NULL, NULL, NULL);
      
      // Beep continuously for 3000ms
      buzzAndDelay(3000, 3000); 
    }

    mfrc522.PICC_HaltA();
    
    // Auto timeout back to dashboard
    uiState = 0;
    drawDashboardInit();
  }
}

// --- Custom Buzzer/Timer Function ---
void buzzAndDelay(int beepTime, int totalWaitTime) {
  unsigned long start = millis();
  digitalWrite(BUZZER_PIN, HIGH); // Turn buzzer ON
  
  while (millis() - start < totalWaitTime) {
    if (millis() - start >= beepTime) {
      digitalWrite(BUZZER_PIN, LOW); // Turn buzzer OFF after 'beepTime'
    }
    server.handleClient(); // Keep the web server alive while waiting
    delay(10);
  }
  digitalWrite(BUZZER_PIN, LOW); // Safety shutoff
}

// --- UI Logic & Drawing ---

void checkButton() {
  if (digitalRead(BUTTON_PIN) == LOW) {
    while(digitalRead(BUTTON_PIN) == LOW) {
      server.handleClient();
      delay(10);
    }
    
    uiState++;
    if (uiState > 2) uiState = 0; // Cycle: 0(Dash) -> 1(Logs) -> 2(Members) -> 0

    if (uiState == 0) drawDashboardInit();
    else if (uiState == 1) showLogsScreen();
    else if (uiState == 2) showMembersScreen();
    
    lastSecond = -1; // Force the clock to update immediately
  }
}

// --- 1. Main Dashboard Design ---
void drawDashboardInit() {
  tft.fillScreen(TFT_BLACK);
  
  // Header
  tft.fillRect(0, 0, tft.width(), 25, FLIPPER_ORANGE);
  tft.setTextColor(TFT_BLACK, FLIPPER_ORANGE);
  tft.setTextSize(2);
  tft.setCursor(10, 5);
  tft.print("System Dashboard");

  // Footer (Wi-Fi Status)
  tft.setTextSize(1);
  tft.setCursor(5, tft.height() - 15);
  if (WiFi.status() == WL_CONNECTED) {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.print("WiFi: OK | IP: ");
    tft.print(WiFi.localIP());
  } else {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.print("WiFi: Offline (Local Mode)");
  }
}

void updateDashboardClock(const RtcDateTime& dt) {
  // Giant Time in the Center
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(4); // Huge font
  tft.setCursor(35, 60); 
  
  char timeBuf[15];
  snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d", dt.Hour(), dt.Minute(), dt.Second());
  tft.print(timeBuf);

  // Date underneath
  tft.setTextColor(FLIPPER_ORANGE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(75, 110); 
  
  char dateBuf[15];
  snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d", dt.Year(), dt.Month(), dt.Day());
  tft.print(dateBuf);
}

// --- 2. Separated Scan Result Screen ---
void drawScanResult(bool success, String uid, String id, String name, String timeStr) {
  tft.fillScreen(TFT_BLACK);
  
  // Dynamic Colored Header
  tft.fillRect(0, 0, tft.width(), 25, success ? TFT_GREEN : TFT_RED);
  tft.setTextColor(TFT_BLACK, success ? TFT_GREEN : TFT_RED);
  tft.setTextSize(2);
  tft.setCursor(10, 5);
  tft.print(success ? "Scan Successful!" : "Scan Failed!");

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  
  if (success) {
    tft.setCursor(10, 40); tft.print("Student ID:");
    tft.setTextColor(FLIPPER_ORANGE, TFT_BLACK);
    tft.setCursor(10, 60); tft.print(id);
    
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(10, 90); tft.print("Name:");
    tft.setTextColor(FLIPPER_ORANGE, TFT_BLACK);
    tft.setCursor(10, 110); tft.print(name);
  } else {
    tft.setCursor(10, 50); tft.print("Unknown Card");
    tft.setTextColor(FLIPPER_ORANGE, TFT_BLACK);
    tft.setCursor(10, 80); tft.print("UID: " + uid);
  }

  // Time at bottom
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(10, 140);
  tft.print("Scanned at: " + timeStr);
}

// --- 3. Logs and Members Screens ---
void showLogsScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, tft.width(), 25, FLIPPER_ORANGE);
  tft.setTextColor(TFT_BLACK, FLIPPER_ORANGE);
  tft.setTextSize(2);
  tft.setCursor(10, 5); tft.print("Recent Logs");

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1); 
  display_row = 0;
  executeSQL("SELECT Log.time, IFNULL(Member.name, Log.member_id) FROM Log LEFT JOIN Member ON Log.member_id = Member.member_id ORDER BY Log.time DESC LIMIT 8;", true);
}

void showMembersScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, tft.width(), 25, FLIPPER_ORANGE);
  tft.setTextColor(TFT_BLACK, FLIPPER_ORANGE);
  tft.setTextSize(2);
  tft.setCursor(10, 5); tft.print("Registered Members");

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1); 
  display_row = 0;
  executeSQL("SELECT member_id, name FROM Member LIMIT 8;", true);
}