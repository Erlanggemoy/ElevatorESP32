#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>

LiquidCrystal_I2C lcd(0x27, 16, 2);
Servo servoMotorPintu;

// Pin untuk motor stepper 
const int pinStep = 5;
const int pinArah = 4;
const int pinEnable = 6;

// Tombol lantai
const int tombol1 = 10;
const int tombol2 = 3;
const int tombol3 = 46;

// Buzzer
const int buzzer = 21;

// Tombol lantai 2 (naik dan turun)
const int tombol2Naik = 12;    // Tombol naik di lantai 2
const int tombol2Turun = 11;   // Tombol turun di lantai 2

// Tombol manual buka/tutup pintu
const int tombolBukaPintu = 15;
const int tombolTutupPintu = 7;

// LED indikator lantai
const int led1 = 16;
const int led2 = 17;
const int led3 = 18;

int lantaiSekarang = 1;
bool pintuTerbuka = false;

// Arah pergerakan lift
enum Arah {
  DIAM,
  BERGERAK_NAIK,
  BERGERAK_TURUN
};

Arah arahSekarang = DIAM;

// Struktur permintaan lantai dengan arah
struct PermintaanLantai {
  int lantai;
  Arah arah;           // Arah yang diinginkan dari lantai tersebut
  bool dariDalam;      // True jika permintaan dari dalam lift
};

// Komponen RTOS
QueueHandle_t antrianLantai;         // Queue untuk permintaan lantai
SemaphoreHandle_t semaphoreLCD;      // Semaphore untuk akses LCD
SemaphoreHandle_t semaphorePintu;    // Binary semaphore untuk kontrol pintu
SemaphoreHandle_t mutexMotor;        // Mutex untuk akses motor stepper

// Array untuk tracking permintaan per lantai
bool permintaanNaik[4] = {false, false, false, false};     // Index 1-3 untuk lantai 1-3
bool permintaanTurun[4] = {false, false, false, false};    // Index 1-3 untuk lantai 1-3
bool permintaanDalam[4] = {false, false, false, false};    // Permintaan dari dalam lift

// Deklarasi Fungsi (Function Prototypes)
void taskPengelolaPermintaan(void *parameter);
void taskKontrolLift(void *parameter);
void taskKontrolPintu(void *parameter);
void taskUpdateLCD(void *parameter);
void taskUpdateLED(void *parameter);
bool adaPermintaan();
void tentukanArahAwal();
void bergerakNaikDanLayani();
void bergerakTurunDanLayani();
bool harusBerhenti(int lantai, Arah arah);
void layaniLantai(int lantai, Arah arah);
bool adaPermintaanNaik();
bool adaPermintaanTurun();
bool adaPermintaanDiAtas();
bool adaPermintaanDiBawah();
void pindahKeLantai(int lantaiTujuan);
void updateLED();
void updateLCD();
void bukaPintu();
void tutupPintu();
void bunyikanBuzzer();

// ISR untuk tombol lantai (dari dalam lift)
void IRAM_ATTR isrTombol1() {
  PermintaanLantai req = {1, DIAM, true};
  xQueueSendFromISR(antrianLantai, &req, NULL);
}

void IRAM_ATTR isrTombol2() {
  PermintaanLantai req = {2, DIAM, true};
  xQueueSendFromISR(antrianLantai, &req, NULL);
}

void IRAM_ATTR isrTombol3() {
  PermintaanLantai req = {3, DIAM, true};
  xQueueSendFromISR(antrianLantai, &req, NULL);
}

// ISR untuk tombol lantai 2 dengan arah
void IRAM_ATTR isrTombol2Naik() {
  PermintaanLantai req = {2, BERGERAK_NAIK, false};
  xQueueSendFromISR(antrianLantai, &req, NULL);
}

void IRAM_ATTR isrTombol2Turun() {
  PermintaanLantai req = {2, BERGERAK_TURUN, false};
  xQueueSendFromISR(antrianLantai, &req, NULL);
}

void IRAM_ATTR isrBukaPintu() {
  xSemaphoreGiveFromISR(semaphorePintu, NULL);
}

void IRAM_ATTR isrTutupPintu() {
  PermintaanLantai req = {-1, DIAM, false};
  xQueueSendFromISR(antrianLantai, &req, NULL);
}

void setup() {
  Serial.begin(115200);
  
  pinMode(pinStep, OUTPUT);
  pinMode(pinArah, OUTPUT);
  pinMode(pinEnable, OUTPUT);

  pinMode(tombol1, INPUT_PULLUP);
  pinMode(tombol2, INPUT_PULLUP);
  pinMode(tombol3, INPUT_PULLUP);
  pinMode(tombol2Naik, INPUT_PULLUP);
  pinMode(tombol2Turun, INPUT_PULLUP);
  pinMode(tombolBukaPintu, INPUT_PULLUP);
  pinMode(tombolTutupPintu, INPUT_PULLUP);

  pinMode(led1, OUTPUT);
  pinMode(led2, OUTPUT);
  pinMode(led3, OUTPUT);
  pinMode(buzzer, OUTPUT);

  lcd.init();
  lcd.backlight();

  servoMotorPintu.attach(19);

  digitalWrite(pinEnable, LOW);

  // Inisialisasi komponen RTOS
  antrianLantai = xQueueCreate(20, sizeof(PermintaanLantai));
  semaphoreLCD = xSemaphoreCreateBinary();
  semaphorePintu = xSemaphoreCreateBinary();
  mutexMotor = xSemaphoreCreateMutex();
  
  xSemaphoreGive(semaphoreLCD);

  // Pasang interrupt
  attachInterrupt(digitalPinToInterrupt(tombol1), isrTombol1, FALLING);
  attachInterrupt(digitalPinToInterrupt(tombol2), isrTombol2, FALLING);
  attachInterrupt(digitalPinToInterrupt(tombol3), isrTombol3, FALLING);
  attachInterrupt(digitalPinToInterrupt(tombol2Naik), isrTombol2Naik, FALLING);
  attachInterrupt(digitalPinToInterrupt(tombol2Turun), isrTombol2Turun, FALLING);
  attachInterrupt(digitalPinToInterrupt(tombolBukaPintu), isrBukaPintu, FALLING);
  attachInterrupt(digitalPinToInterrupt(tombolTutupPintu), isrTutupPintu, FALLING);

  // Buat tasks
  xTaskCreatePinnedToCore(taskKontrolLift, "KontrolLift", 4096, NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(taskPengelolaPermintaan, "PengelolaPermintaan", 2048, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(taskKontrolPintu, "KontrolPintu", 2048, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(taskUpdateLCD, "UpdateLCD", 2048, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(taskUpdateLED, "UpdateLED", 1024, NULL, 1, NULL, 0);

  tutupPintu();
  updateLCD();
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}

// Task untuk mengelola permintaan yang masuk
void taskPengelolaPermintaan(void *parameter) {
  PermintaanLantai req;
  
  while (1) {
    if (xQueueReceive(antrianLantai, &req, portMAX_DELAY)) {
      if (req.lantai < 0) {
        continue; // Abaikan sinyal tutup pintu
      }
      
      // Catat permintaan
      if (req.dariDalam) {
        permintaanDalam[req.lantai] = true;
        Serial.printf("Permintaan dari dalam: Lantai %d\n", req.lantai);
      } else {
        if (req.arah == BERGERAK_NAIK) {
          permintaanNaik[req.lantai] = true;
          Serial.printf("Permintaan NAIK dari luar: Lantai %d\n", req.lantai);
        } else if (req.arah == BERGERAK_TURUN) {
          permintaanTurun[req.lantai] = true;
          Serial.printf("Permintaan TURUN dari luar: Lantai %d\n", req.lantai);
        }
      }
    }
  }
}

// Task untuk kontrol lift dengan algoritma SCAN
void taskKontrolLift(void *parameter) {
  while (1) {
    // Cek apakah ada permintaan yang perlu dilayani
    if (adaPermintaan()) {
      if (arahSekarang == DIAM) {
        // Tentukan arah awal
        tentukanArahAwal();
      }
      
      // Gerakkan lift sesuai algoritma SCAN
      if (arahSekarang == BERGERAK_NAIK) {
        bergerakNaikDanLayani();
      } else if (arahSekarang == BERGERAK_TURUN) {
        bergerakTurunDanLayani();
      }
    } else {
      arahSekarang = DIAM;
    }
    
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// Task untuk kontrol pintu
void taskKontrolPintu(void *parameter) {
  while (1) {
    if (xSemaphoreTake(semaphorePintu, pdMS_TO_TICKS(100)) == pdTRUE) {
      bukaPintu();
      vTaskDelay(pdMS_TO_TICKS(3000));
      tutupPintu();
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// Task untuk update LCD
void taskUpdateLCD(void *parameter) {
  while (1) {
    updateLCD();
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// Task untuk update LED
void taskUpdateLED(void *parameter) {
  while (1) {
    updateLED();
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// Cek apakah ada permintaan
bool adaPermintaan() {
  for (int i = 1; i <= 3; i++) {
    if (permintaanNaik[i] || permintaanTurun[i] || permintaanDalam[i]) {
      return true;
    }
  }
  return false;
}

// Tentukan arah awal berdasarkan permintaan terdekat
void tentukanArahAwal() {
  // Cari permintaan terdekat
  for (int i = 1; i <= 3; i++) {
    if (i > lantaiSekarang && (permintaanNaik[i] || permintaanTurun[i] || permintaanDalam[i])) {
      arahSekarang = BERGERAK_NAIK;
      Serial.println("Arah: NAIK");
      return;
    }
  }
  
  for (int i = 3; i >= 1; i--) {
    if (i < lantaiSekarang && (permintaanNaik[i] || permintaanTurun[i] || permintaanDalam[i])) {
      arahSekarang = BERGERAK_TURUN;
      Serial.println("Arah: TURUN");
      return;
    }
  }
}

// Gerak naik dan layani permintaan yang searah
void bergerakNaikDanLayani() {
  for (int lantai = lantaiSekarang; lantai <= 3; lantai++) {
    if (harusBerhenti(lantai, BERGERAK_NAIK)) {
      pindahKeLantai(lantai);
      layaniLantai(lantai, BERGERAK_NAIK);
    }
  }
  
  // Setelah sampai puncak, balik arah jika ada permintaan turun
  if (adaPermintaanTurun() || adaPermintaanDiBawah()) {
    arahSekarang = BERGERAK_TURUN;
    Serial.println("Arah berubah: TURUN");
  } else {
    arahSekarang = DIAM;
  }
}

// Gerak turun dan layani permintaan yang searah
void bergerakTurunDanLayani() {
  for (int lantai = lantaiSekarang; lantai >= 1; lantai--) {
    if (harusBerhenti(lantai, BERGERAK_TURUN)) {
      pindahKeLantai(lantai);
      layaniLantai(lantai, BERGERAK_TURUN);
    }
  }
  
  // Setelah sampai dasar, balik arah jika ada permintaan naik
  if (adaPermintaanNaik() || adaPermintaanDiAtas()) {
    arahSekarang = BERGERAK_NAIK;
    Serial.println("Arah berubah: NAIK");
  } else {
    arahSekarang = DIAM;
  }
}

// Cek apakah harus berhenti di lantai ini
bool harusBerhenti(int lantai, Arah arah) {
  // Selalu berhenti jika ada permintaan dari dalam lift
  if (permintaanDalam[lantai]) {
    return true;
  }
  
  // Berhenti jika permintaan dari luar searah dengan pergerakan
  if (arah == BERGERAK_NAIK && permintaanNaik[lantai]) {
    return true;
  }
  
  if (arah == BERGERAK_TURUN && permintaanTurun[lantai]) {
    return true;
  }
  
  return false;
}

// Layani lantai (buka pintu dan hapus permintaan)
void layaniLantai(int lantai, Arah arah) {
  Serial.printf("Melayani lantai %d\n", lantai);
  
  // Bunyikan buzzer saat sampai di lantai
  bunyikanBuzzer();
  
  //bukaPintu();
  //vTaskDelay(pdMS_TO_TICKS(3000));
  //tutupPintu();
  
  // Jadi:
  xSemaphoreGive(semaphorePintu);        // minta buka pintu
  // Tunggu sedikit sampai pintu dibuka oleh TaskKontrolPintu (opsional)
  vTaskDelay(pdMS_TO_TICKS(2000));      // tunggu waktu buka + waktu tunggu (sertakan margin)

  // Hapus permintaan yang dilayani
  permintaanDalam[lantai] = false;
  
  if (arah == BERGERAK_NAIK) {
    permintaanNaik[lantai] = false;
  } else if (arah == BERGERAK_TURUN) {
    permintaanTurun[lantai] = false;
  }
}

// Fungsi bantu untuk cek permintaan
bool adaPermintaanNaik() {
  for (int i = 1; i <= 3; i++) {
    if (permintaanNaik[i]) return true;
  }
  return false;
}

bool adaPermintaanTurun() {
  for (int i = 1; i <= 3; i++) {
    if (permintaanTurun[i]) return true;
  }
  return false;
}

bool adaPermintaanDiAtas() {
  for (int i = lantaiSekarang + 1; i <= 3; i++) {
    if (permintaanNaik[i] || permintaanTurun[i] || permintaanDalam[i]) return true;
  }
  return false;
}

bool adaPermintaanDiBawah() {
  for (int i = 1; i < lantaiSekarang; i++) {
    if (permintaanNaik[i] || permintaanTurun[i] || permintaanDalam[i]) return true;
  }
  return false;
}

// Fungsi pindah lift ke tujuan (dengan proteksi mutex)
/*void pindahKeLantai(int lantaiTujuan) {
  if (lantaiTujuan == lantaiSekarang) return;
  
  if (xSemaphoreTake(mutexMotor, portMAX_DELAY) == pdTRUE) {
    Serial.printf("Bergerak dari lantai %d ke %d\n", lantaiSekarang, lantaiTujuan);
    
    int jumlahStep = abs(lantaiTujuan - lantaiSekarang) * 1000;
    digitalWrite(pinArah, lantaiTujuan > lantaiSekarang ? HIGH : LOW);

    for (int i = 0; i < jumlahStep; i++) {
      digitalWrite(pinStep, HIGH);
      delayMicroseconds(800);
      digitalWrite(pinStep, LOW);
      delayMicroseconds(800);
    }

    lantaiSekarang = lantaiTujuan;
    xSemaphoreGive(mutexMotor);
  }
} */

void pindahKeLantai(int lantaiTujuan) {
  if (lantaiTujuan == lantaiSekarang) return;

  if (xSemaphoreTake(mutexMotor, portMAX_DELAY) == pdTRUE) {
    Serial.printf("Bergerak dari lantai %d ke %d\n", lantaiSekarang, lantaiTujuan);

    int jumlahStep = abs(lantaiTujuan - lantaiSekarang) * 1000;
    digitalWrite(pinArah, lantaiTujuan > lantaiSekarang ? HIGH : LOW);

    for (int i = 0; i < jumlahStep; i++) {
      digitalWrite(pinStep, HIGH);
      delayMicroseconds(800);
      digitalWrite(pinStep, LOW);
      delayMicroseconds(800);

      // beri kesempatan ke task lain (sesuaikan interval)
      if ((i % 200) == 0) {
        // vTaskDelay(1); // lebih aman tapi memperlambat
        taskYIELD();     // cepat, hanya menyerahkan CPU
      }
    }

    lantaiSekarang = lantaiTujuan;
    xSemaphoreGive(mutexMotor);
  }
}


// Fungsi update LED lantai
void updateLED() {
  digitalWrite(led1, lantaiSekarang == 1 ? HIGH : LOW);
  digitalWrite(led2, lantaiSekarang == 2 ? HIGH : LOW);
  digitalWrite(led3, lantaiSekarang == 3 ? HIGH : LOW);
}

void updateLCD() {
  if (xSemaphoreTake(semaphoreLCD, pdMS_TO_TICKS(100)) == pdTRUE) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Lantai: ");
    lcd.print(lantaiSekarang);
    
    // Tampilkan arah
    lcd.print(" ");
    if (arahSekarang == BERGERAK_NAIK) {
      lcd.print("^");
    } else if (arahSekarang == BERGERAK_TURUN) {
      lcd.print("v");
    }

    lcd.setCursor(0, 1);
    lcd.print(pintuTerbuka ? "Pintu: Terbuka" : "Pintu: Tertutup");
    
    xSemaphoreGive(semaphoreLCD);
  }
}

// Fungsi membuka pintu lift
void bukaPintu() {
  servoMotorPintu.write(90);
  pintuTerbuka = true;
  Serial.println("Pintu terbuka");
}

// Fungsi menutup pintu
void tutupPintu() {
  servoMotorPintu.write(0);
  pintuTerbuka = false;
  Serial.println("Pintu tertutup");
}

// Fungsi buzzer saat sampai di lantai
void bunyikanBuzzer() {
  // Bunyi "beep beep" saat sampai lantai
  for (int i = 0; i < 2; i++) {
    digitalWrite(buzzer, HIGH);
    vTaskDelay(pdMS_TO_TICKS(200));
    digitalWrite(buzzer, LOW);
    vTaskDelay(pdMS_TO_TICKS(300));
  }
}