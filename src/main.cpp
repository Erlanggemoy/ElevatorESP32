#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>

LiquidCrystal_I2C lcd(0x27, 16, 2);
Servo doorServo;

// Pin untuk motor stepper 
const int stepPin = 5;
const int dirPin = 4;
const int enablePin = 6;

// Tombol lantai
const int button1 = 10;
const int button2 = 3;
const int button3 = 46;

//buzzer
const int buzzer = 21;

// Tombol lantai 2 (naik dan turun)
const int button2Up = 12;    // Tombol naik di lantai 2
const int button2Down = 11;  // Tombol turun di lantai 2

// Tombol manual buka/tutup pintu
const int openDoorButton = 15;
const int closeDoorButton = 7;

// LED indikator lantai
const int led1 = 16;
const int led2 = 17;
const int led3 = 18;

int currentFloor = 1;
bool doorOpen = false;

// Arah pergerakan lift
enum Direction {
  IDLE,
  MOVING_UP,
  MOVING_DOWN
};

Direction currentDirection = IDLE;

// Struktur request lantai dengan arah
struct FloorRequest {
  int floor;
  Direction direction;  // Arah yang diinginkan dari lantai tersebut
  bool isInside;        // True jika request dari dalam lift
};

// RTOS Components
QueueHandle_t floorQueue;           // Queue untuk request lantai
SemaphoreHandle_t lcdSemaphore;     // Semaphore untuk akses LCD
SemaphoreHandle_t doorSemaphore;    // Binary semaphore untuk kontrol pintu
SemaphoreHandle_t motorMutex;       // Mutex untuk akses motor stepper

// Array untuk tracking request per lantai
bool upRequests[4] = {false, false, false, false};    // Index 1-3 untuk lantai 1-3
bool downRequests[4] = {false, false, false, false};  // Index 1-3 untuk lantai 1-3
bool insideRequests[4] = {false, false, false, false}; // Request dari dalam lift

// ISR untuk tombol lantai (dari dalam lift)
void IRAM_ATTR button1ISR() {
  FloorRequest req = {1, IDLE, true};
  xQueueSendFromISR(floorQueue, &req, NULL);
}

void IRAM_ATTR button2ISR() {
  FloorRequest req = {2, IDLE, true};
  xQueueSendFromISR(floorQueue, &req, NULL);
}

void IRAM_ATTR button3ISR() {
  FloorRequest req = {3, IDLE, true};
  xQueueSendFromISR(floorQueue, &req, NULL);
}

// ISR untuk tombol lantai 2 dengan arah
void IRAM_ATTR button2UpISR() {
  FloorRequest req = {2, MOVING_UP, false};
  xQueueSendFromISR(floorQueue, &req, NULL);
}

void IRAM_ATTR button2DownISR() {
  FloorRequest req = {2, MOVING_DOWN, false};
  xQueueSendFromISR(floorQueue, &req, NULL);
}

void IRAM_ATTR openDoorISR() {
  xSemaphoreGiveFromISR(doorSemaphore, NULL);
}

void IRAM_ATTR closeDoorISR() {
  FloorRequest req = {-1, IDLE, false};
  xQueueSendFromISR(floorQueue, &req, NULL);
}

void setup() {
  Serial.begin(115200);
  
  pinMode(stepPin, OUTPUT);
  pinMode(dirPin, OUTPUT);
  pinMode(enablePin, OUTPUT);

  pinMode(button1, INPUT_PULLUP);
  pinMode(button2, INPUT_PULLUP);
  pinMode(button3, INPUT_PULLUP);
  pinMode(button2Up, INPUT_PULLUP);
  pinMode(button2Down, INPUT_PULLUP);
  pinMode(openDoorButton, INPUT_PULLUP);
  pinMode(closeDoorButton, INPUT_PULLUP);

  pinMode(led1, OUTPUT);
  pinMode(led2, OUTPUT);
  pinMode(led3, OUTPUT);
  pinMode(buzzer, OUTPUT);

  lcd.init();
  lcd.backlight();

  doorServo.attach(19);

  digitalWrite(enablePin, LOW);

  // Inisialisasi RTOS components
  floorQueue = xQueueCreate(20, sizeof(FloorRequest));
  lcdSemaphore = xSemaphoreCreateBinary();
  doorSemaphore = xSemaphoreCreateBinary();
  motorMutex = xSemaphoreCreateMutex();
  
  xSemaphoreGive(lcdSemaphore);

  // Attach interrupts
  attachInterrupt(digitalPinToInterrupt(button1), button1ISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(button2), button2ISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(button3), button3ISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(button2Up), button2UpISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(button2Down), button2DownISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(openDoorButton), openDoorISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(closeDoorButton), closeDoorISR, FALLING);

  // Buat tasks

  // Core 1
  xTaskCreatePinnedToCore(liftControlTask, "LiftControl", 4096, NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(requestManagerTask, "RequestManager", 2048, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(doorControlTask, "DoorControl", 2048, NULL, 2, NULL, 1);
  
  //Core 0
  xTaskCreatePinnedToCore(lcdUpdateTask, "LCDUpdate", 2048, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(ledUpdateTask, "LEDUpdate", 1024, NULL, 1, NULL, 0);

  closeDoor();
  updateLCD();
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}

// Task untuk mengelola request yang masuk
void requestManagerTask(void *parameter) {
  FloorRequest req;
  
  while (1) {
    if (xQueueReceive(floorQueue, &req, portMAX_DELAY)) {
      if (req.floor < 0) {
        continue; // Ignore close door signal
      }
      
      // Catat request
      if (req.isInside) {
        insideRequests[req.floor] = true;
        Serial.printf("Inside request: Floor %d\n", req.floor);
      } else {
        if (req.direction == MOVING_UP) {
          upRequests[req.floor] = true;
          Serial.printf("Outside UP request: Floor %d\n", req.floor);
        } else if (req.direction == MOVING_DOWN) {
          downRequests[req.floor] = true;
          Serial.printf("Outside DOWN request: Floor %d\n", req.floor);
        }
      }
    }
  }
}

// Task untuk kontrol lift dengan algoritma SCAN
void liftControlTask(void *parameter) {
  while (1) {
    // Cek apakah ada request yang perlu dilayani
    if (hasAnyRequest()) {
      if (currentDirection == IDLE) {
        // Tentukan arah awal
        determineInitialDirection();
      }
      
      // Gerakkan lift sesuai algoritma SCAN
      if (currentDirection == MOVING_UP) {
        moveUpAndServe();
      } else if (currentDirection == MOVING_DOWN) {
        moveDownAndServe();
      }
    } else {
      currentDirection = IDLE;
    }
    
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// Task untuk kontrol pintu
void doorControlTask(void *parameter) {
  while (1) {
    if (xSemaphoreTake(doorSemaphore, pdMS_TO_TICKS(100)) == pdTRUE) {
      openDoor();
      vTaskDelay(pdMS_TO_TICKS(3000));
      closeDoor();
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// Task untuk update LCD
void lcdUpdateTask(void *parameter) {
  while (1) {
    updateLCD();
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// Task untuk update LED
void ledUpdateTask(void *parameter) {
  while (1) {
    updateLEDs();
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// Cek apakah ada request
bool hasAnyRequest() {
  for (int i = 1; i <= 3; i++) {
    if (upRequests[i] || downRequests[i] || insideRequests[i]) {
      return true;
    }
  }
  return false;
}

// Tentukan arah awal berdasarkan request terdekat
void determineInitialDirection() {
  // Cari request terdekat
  for (int i = 1; i <= 3; i++) {
    if (i > currentFloor && (upRequests[i] || downRequests[i] || insideRequests[i])) {
      currentDirection = MOVING_UP;
      Serial.println("Direction: UP");
      return;
    }
  }
  
  for (int i = 3; i >= 1; i--) {
    if (i < currentFloor && (upRequests[i] || downRequests[i] || insideRequests[i])) {
      currentDirection = MOVING_DOWN;
      Serial.println("Direction: DOWN");
      return;
    }
  }
}

// Gerak naik dan layani request yang searah
void moveUpAndServe() {
  for (int floor = currentFloor; floor <= 3; floor++) {
    if (shouldStopAtFloor(floor, MOVING_UP)) {
      moveToFloor(floor);
      serveFloor(floor, MOVING_UP);
    }
  }
  
  // Setelah sampai puncak, balik arah jika ada request turun
  if (hasDownRequests() || hasRequestsBelow()) {
    currentDirection = MOVING_DOWN;
    Serial.println("Direction changed: DOWN");
  } else {
    currentDirection = IDLE;
  }
}

// Gerak turun dan layani request yang searah
void moveDownAndServe() {
  for (int floor = currentFloor; floor >= 1; floor--) {
    if (shouldStopAtFloor(floor, MOVING_DOWN)) {
      moveToFloor(floor);
      serveFloor(floor, MOVING_DOWN);
    }
  }
  
  // Setelah sampai dasar, balik arah jika ada request naik
  if (hasUpRequests() || hasRequestsAbove()) {
    currentDirection = MOVING_UP;
    Serial.println("Direction changed: UP");
  } else {
    currentDirection = IDLE;
  }
}

// Cek apakah harus berhenti di lantai ini
bool shouldStopAtFloor(int floor, Direction dir) {
  // Selalu berhenti jika ada request dari dalam lift
  if (insideRequests[floor]) {
    return true;
  }
  
  // Berhenti jika request dari luar searah dengan pergerakan
  if (dir == MOVING_UP && upRequests[floor]) {
    return true;
  }
  
  if (dir == MOVING_DOWN && downRequests[floor]) {
    return true;
  }
  
  return false;
}

// Layani lantai (buka pintu dan hapus request)
void serveFloor(int floor, Direction dir) {
  Serial.printf("Serving floor %d\n", floor);
  
  // Bunyikan buzzer saat sampai di lantai
  playArrivalBuzzer();
  
  openDoor();
  vTaskDelay(pdMS_TO_TICKS(3000));
  closeDoor();
  
  // Hapus request yang dilayani
  insideRequests[floor] = false;
  
  if (dir == MOVING_UP) {
    upRequests[floor] = false;
  } else if (dir == MOVING_DOWN) {
    downRequests[floor] = false;
  }
}

// Helper functions untuk cek request
bool hasUpRequests() {
  for (int i = 1; i <= 3; i++) {
    if (upRequests[i]) return true;
  }
  return false;
}

bool hasDownRequests() {
  for (int i = 1; i <= 3; i++) {
    if (downRequests[i]) return true;
  }
  return false;
}

bool hasRequestsAbove() {
  for (int i = currentFloor + 1; i <= 3; i++) {
    if (upRequests[i] || downRequests[i] || insideRequests[i]) return true;
  }
  return false;
}

bool hasRequestsBelow() {
  for (int i = 1; i < currentFloor; i++) {
    if (upRequests[i] || downRequests[i] || insideRequests[i]) return true;
  }
  return false;
}

// Fungsi pindah lift ke tujuan (dengan mutex protection)
void moveToFloor(int targetFloor) {
  if (targetFloor == currentFloor) return;
  
  if (xSemaphoreTake(motorMutex, portMAX_DELAY) == pdTRUE) {
    Serial.printf("Moving from floor %d to %d\n", currentFloor, targetFloor);
    
    int stepCount = abs(targetFloor - currentFloor) * 2000;
    digitalWrite(dirPin, targetFloor > currentFloor ? HIGH : LOW);

    for (int i = 0; i < stepCount; i++) {
      digitalWrite(stepPin, HIGH);
      delayMicroseconds(800);
      digitalWrite(stepPin, LOW);
      delayMicroseconds(800);
    }

    currentFloor = targetFloor;
    xSemaphoreGive(motorMutex);
  }
}

// Fungsi update LED lantai
void updateLEDs() {
  digitalWrite(led1, currentFloor == 1 ? HIGH : LOW);
  digitalWrite(led2, currentFloor == 2 ? HIGH : LOW);
  digitalWrite(led3, currentFloor == 3 ? HIGH : LOW);
}

void updateLCD() {
  if (xSemaphoreTake(lcdSemaphore, pdMS_TO_TICKS(100)) == pdTRUE) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Lantai: ");
    lcd.print(currentFloor);
    
    // Tampilkan arah
    lcd.print(" ");
    if (currentDirection == MOVING_UP) {
      lcd.print("^");
    } else if (currentDirection == MOVING_DOWN) {
      lcd.print("v");
    }

    lcd.setCursor(0, 1);
    lcd.print(doorOpen ? "Pintu: Terbuka" : "Pintu: Tertutup");
    
    xSemaphoreGive(lcdSemaphore);
  }
}

// Fungsi membuka pintu lift
void openDoor() {
  doorServo.write(90);
  doorOpen = true;
  Serial.println("Door opened");
}

// Fungsi menutup pintu
void closeDoor() {
  doorServo.write(0);
  doorOpen = false;
  Serial.println("Door closed");
}

// Fungsi buzzer saat sampai di lantai
void playArrivalBuzzer() {
  // Bunyi "beep beep" saat sampai lantai
  for (int i = 0; i < 2; i++) {
    tone(buzzer, 2000, 200);  // Frekuensi 2000Hz, durasi 200ms
    delay(300);
  }
  noTone(buzzer);
}