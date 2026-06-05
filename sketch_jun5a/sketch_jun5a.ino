#include <SPI.h>
#include <MFRC522.h>
#include <TFT_eSPI.h>
#include <ThreeWire.h>  
#include <RtcDS1302.h>
#include <SD.h>
#include <sqlite3.h>

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
#define BUTTON_PIN 3 // New physical button pin

// --- Objects ---
MFRC522 mfrc522(SS_PIN, RST_PIN); 
TFT_eSPI tft = TFT_eSPI();        
ThreeWire myWire(RTC_DAT, RTC_CLK, RTC_RST);
RtcDS1302<ThreeWire> Rtc(myWire);

// --- SQLite Setup ---
const char* db_file = "/sd/attendance.db"; 
sqlite3 *db; 

#define FLIPPER_ORANGE tft.color565(255, 130, 0)
int lastSecond = -1;
int log_row = 0; // Tracks which line to print on the screen

// --- SQLite Helper Functions ---

// This callback handles what happens when we ask the database for information
int db_callback(void *data, int argc, char **argv, char **azColName) {
  if (data != NULL) {
    // If data is passed, we want to print it to the TFT screen!
    tft.setCursor(5, 30 + (log_row * 15));
    // argv[0] is time, argv[1] is the UID/Door
    tft.print(argv[0] ? argv[0] : "NULL"); 
    tft.print(" | ");
    tft.print(argv[1] ? argv[1] : "NULL"); 
    log_row++;
  }
  return 0; 
}

// Executes SQL. If printToScreen is true, it triggers the callback to draw on the TFT
void executeSQL(const char* sql, bool printToScreen = false) {
  char *zErrMsg = 0;
  int rc = sqlite3_exec(db, sql, db_callback, printToScreen ? (void*)1 : NULL, &zErrMsg);
  if (rc != SQLITE_OK) {
    Serial.printf("SQL error: %s\n", zErrMsg);
    sqlite3_free(zErrMsg);
  }
}

void setupDatabase() {
  Serial.print("Initializing SD card...");
  if (!SD.begin(SD_SS_PIN, SPI)) {
    Serial.println("SD Card initialization failed!");
    return;
  }

  sqlite3_initialize();
  if (sqlite3_open(db_file, &db)) {
    Serial.printf("Can't open database: %s\n", sqlite3_errmsg(db));
    return;
  }

  // Create Tables
  executeSQL("CREATE TABLE IF NOT EXISTS Member (member_id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT, rfid TEXT UNIQUE);");
  executeSQL("CREATE TABLE IF NOT EXISTS Log (log_id INTEGER PRIMARY KEY AUTOINCREMENT, member_id INTEGER, door_id TEXT, time TEXT, is_double_tap INTEGER, FOREIGN KEY(member_id) REFERENCES Member(member_id));");
}

// --- Main Setup ---
void setup() {
  Serial.begin(115200);

  // Setup Button (Uses internal pull-up resistor so we don't need external resistors)
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // 1. Screen Init
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  
  // 2. Custom SPI Init 
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, SS_PIN);

  // 3. RFID Init
  mfrc522.PCD_Init();
  
  // 4. RTC Init
  Rtc.Begin();
  if (!Rtc.GetIsRunning()) Rtc.SetIsRunning(true);
  if (!Rtc.IsDateTimeValid()) {
      RtcDateTime compiled = RtcDateTime(__DATE__, __TIME__);
      Rtc.SetDateTime(compiled);
  }

  // 5. Database Init
  setupDatabase();

  resetScreen();
}

// --- Main Loop ---
void loop() {
  // Check if the physical button is pressed to view logs
  if (digitalRead(BUTTON_PIN) == LOW) {
    showLogsScreen();
  }

  // Update Live Clock
  RtcDateTime now = Rtc.GetDateTime();
  if (now.Second() != lastSecond) {
    lastSecond = now.Second();
    drawLiveClock(now);
  }

  // Check for RFID
  if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) {
    return;
  }

  // --- Scan Successful ---
  tft.fillRect(0, 50, tft.width(), 100, TFT_BLACK);
  tft.setTextColor(TFT_GREEN, TFT_BLACK); 
  tft.setCursor(10, 60);
  tft.print("Scan Successful!");

  String uidString = "";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    if(mfrc522.uid.uidByte[i] < 0x10) uidString += "0";
    uidString += String(mfrc522.uid.uidByte[i], HEX);
    if(i < mfrc522.uid.size - 1) uidString += " ";
  }
  uidString.toUpperCase();
  
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(10, 90);
  tft.print("UID: ");
  tft.print(uidString);

  // --- SAVE TO SQLITE DATABASE ---
  char sqlQuery[200];
  char timeStr[25];
  // Format the time as YYYY-MM-DD HH:MM:SS
  snprintf(timeStr, sizeof(timeStr), "%04d-%02d-%02d %02d:%02d:%02d", now.Year(), now.Month(), now.Day(), now.Hour(), now.Minute(), now.Second());
  
  // Insert the log. Since we haven't linked Members yet, we will save the UID into the door_id column temporarily
  snprintf(sqlQuery, sizeof(sqlQuery), "INSERT INTO Log (door_id, time) VALUES ('%s', '%s');", uidString.c_str(), timeStr);
  executeSQL(sqlQuery);

  mfrc522.PICC_HaltA();
  delay(2000); 
  resetScreen();
}

// --- UI Helper Functions ---
void resetScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, tft.width(), 20, FLIPPER_ORANGE);
  tft.setTextColor(TFT_BLACK, FLIPPER_ORANGE);
  tft.setTextSize(2);
  tft.setCursor(5, 3);
  tft.print("RFID Scanner");

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 80);
  tft.print("Ready to scan...");
}

void drawLiveClock(const RtcDateTime& dt) {
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(tft.width() - 110, 25); 
  
  if (dt.Hour() < 10) tft.print("0");
  tft.print(dt.Hour());
  tft.print(":");
  if (dt.Minute() < 10) tft.print("0");
  tft.print(dt.Minute());
  tft.print(":");
  if (dt.Second() < 10) tft.print("0");
  tft.print(dt.Second());
}

// --- Log Viewer Screen ---
void showLogsScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, tft.width(), 20, FLIPPER_ORANGE);
  tft.setTextColor(TFT_BLACK, FLIPPER_ORANGE);
  tft.setTextSize(2);
  tft.setCursor(5, 3);
  tft.print("Recent Logs");

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1); // Small text to fit more logs
  
  log_row = 0;
  // Get the 10 most recent logs and display them on screen
  executeSQL("SELECT time, door_id FROM Log ORDER BY log_id DESC LIMIT 10;", true);

  // Wait for the user to release the button
  delay(200); 
  while(digitalRead(BUTTON_PIN) == LOW); 
  
  // Wait for the user to press the button AGAIN to exit
  while(digitalRead(BUTTON_PIN) == HIGH) {
    delay(50);
  }
  
  // Wait for release before going back to the main menu
  delay(200);
  while(digitalRead(BUTTON_PIN) == LOW);
  
  resetScreen();
}