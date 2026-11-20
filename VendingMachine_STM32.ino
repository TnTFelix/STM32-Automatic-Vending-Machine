#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <HardwareSerial.h>
#include <string.h>

// =================================================
// 1. UART STM32 <-> ESP32
// =================================================

Uart LinkSerial(PB11, PB10);   // RX, TX = USART3
#define Link LinkSerial

const uint32_t STATUS_MS = 200;
uint32_t lastStatusMs = 0;

char rxBuf[40];
uint8_t rxLen = 0;

char ipText[20] = "";
char ipMode = 'W';
bool ipReceived = false;

// =================================================
// 2. LCD I2C
// SDA = PB9
// SCL = PB8
// =================================================

LiquidCrystal_I2C lcd(0x27, 20, 4);

// =================================================
// 3. KEYPAD 4x4
// =================================================

const byte ROWS = 4;
const byte COLS = 4;

char keys[ROWS][COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};

byte rowPins[ROWS] = {PA0, PA1, PA2, PA3};
byte colPins[COLS] = {PA4, PA5, PA6, PA7};

Keypad keypad = Keypad(
  makeKeymap(keys),
  rowPins,
  colPins,
  ROWS,
  COLS
);

// =================================================
// 4. CAM BIEN XU
// =================================================

const uint8_t COIN_PIN = PB0;

const uint32_t COIN_DEBOUNCE_US = 150000UL;

volatile bool coinAcceptEnabled = false;
volatile uint16_t coinCount = 0;
volatile uint32_t lastCoinInterruptUs = 0;

// =================================================
// 5. HC-SR04
// TRIG = PB7
// ECHO = PB6
// =================================================

const uint8_t TRIG_PIN = PB7;
const uint8_t ECHO_PIN = PB6;

const float ITEM_PRESENT_CM = 8.0f;
const float ITEM_REMOVED_CM = 12.0f;

// Thoi gian cho san pham xuat hien
const uint32_t PRODUCT_TIMEOUT_MS = 15000UL;

// Thoi gian cho khach lay san pham
const uint32_t REMOVE_TIMEOUT_MS = 30000UL;

// =================================================
// 6. DIEU KHIEN 4 DONG CO QUA 2 L298N
// =================================================

// Motor 1 - SP1
// L298N 1: IN1 = PB5, IN2 = PB4

// Motor 2 - SP2
// L298N 1: IN3 = PB3, IN4 = PA15

// Motor 3 - SP3
// L298N 2: IN1 = PA12, IN2 = PA11

// Motor 4 - SP4
// L298N 2: IN3 = PA10, IN4 = PA9

const uint8_t motorPins[4][2] = {
  {PB5, PB4},
  {PB3, PA15},
  {PA12, PA11},
  {PA10, PA9}
};

// =================================================
// 7. THONG SO BAN HANG
// =================================================

const uint8_t prices[4] = {2, 2, 3, 3};

const uint32_t VND_PER_COIN = 1000;

const uint32_t BANK_TIMEOUT_MS =
  5UL * 60UL * 1000UL;

// =================================================
// 8. TRANG THAI MAY
// =================================================

enum MachineState {
  SELECT_PRODUCT,
  CONFIRM_PRODUCT,
  INSERT_COINS,
  DISPENSING,
  BANK_PAY
};

MachineState state = SELECT_PRODUCT;

int selectedProduct = -1;

uint16_t displayedCoins = 0;
uint16_t currentCoins = 0;

uint16_t bankSeq = 0;
uint32_t bankStartMs = 0;

char pendingKey = 0;
bool pendingPaid = false;

uint8_t dispensePhase = 0;

uint32_t ignoreKeysUntil = 0;

// =================================================
// 9. CAM BIEN XU - NGAT
// =================================================

void coinSensorISR() {

  if (!coinAcceptEnabled) {
    return;
  }

  uint32_t nowUs = micros();

  if ((uint32_t)(nowUs - lastCoinInterruptUs)
      >= COIN_DEBOUNCE_US) {

    coinCount++;
    lastCoinInterruptUs = nowUs;
  }
}

uint16_t getCoins() {

  noInterrupts();
  uint16_t result = coinCount;
  interrupts();

  return result;
}

// Xoa so du xu chi dung khi khoi tao/reset co chu dich.
// Khong goi ham nay khi huy giao dich hoac sau moi lan mua hang.
void coinClear() {

  noInterrupts();
  coinCount = 0;
  interrupts();
}

// Bat dau nhan xu moi nhung GIU LAI so du da co.
void coinStart() {

  noInterrupts();

  lastCoinInterruptUs = micros();
  coinAcceptEnabled = true;

  interrupts();
}

// =================================================
// 10. DIEU KHIEN DONG CO
// =================================================

// Dung ca 4 dong co
void stopAllMotors() {

  for (uint8_t i = 0; i < 4; i++) {

    digitalWrite(motorPins[i][0], LOW);
    digitalWrite(motorPins[i][1], LOW);
  }
}

// Chay dong co theo chieu thuan
void motorForward(uint8_t motor) {

  if (motor >= 4) return;

  digitalWrite(motorPins[motor][0], HIGH);
  digitalWrite(motorPins[motor][1], LOW);
}

// Dung mot dong co
void motorStop(uint8_t motor) {

  if (motor >= 4) return;

  digitalWrite(motorPins[motor][0], LOW);
  digitalWrite(motorPins[motor][1], LOW);
}

// =================================================
// 11. GIAO TIEP UART
// =================================================

void sendStatus() {

  char b[96];

  snprintf(
    b,
    sizeof(b),
    "S,%d,%d,%u,%d,%u,%lu,%u,%u,%u,%u\n",
    (int)state,
    selectedProduct,
    (unsigned)getCoins(),
    (int)dispensePhase,
    (unsigned)bankSeq,
    (unsigned long)VND_PER_COIN,
    prices[0],
    prices[1],
    prices[2],
    prices[3]
  );

  Link.print(b);
}

void handleLine(const char* line) {

  if (
    line[0] == 'K' &&
    line[1] == ',' &&
    line[2] != 0 &&
    line[3] == 0
  ) {

    if (strchr("1234ABC#", line[2]) != NULL) {
      pendingKey = line[2];
    }
  }

  else if (strcmp(line, "PAID") == 0) {

    pendingPaid = true;
  }

  else if (
    strncmp(line, "IP,", 3) == 0 &&
    line[3] != 0 &&
    line[4] == ','
  ) {

    ipMode = line[3];

    strncpy(
      ipText,
      line + 5,
      sizeof(ipText) - 1
    );

    ipText[sizeof(ipText) - 1] = 0;

    ipReceived = true;
  }
}

void readLink() {

  while (Link.available()) {

    char c = (char)Link.read();

    if (c == '\n' || c == '\r') {

      if (rxLen > 0) {

        rxBuf[rxLen] = 0;

        handleLine(rxBuf);

        rxLen = 0;
      }
    }

    else if (rxLen < sizeof(rxBuf) - 1) {

      rxBuf[rxLen++] = c;
    }

    else {

      rxLen = 0;
    }
  }
}

void service() {

  readLink();

  if (
    (uint32_t)(millis() - lastStatusMs)
    >= STATUS_MS
  ) {

    lastStatusMs = millis();

    sendStatus();
  }
}

void waitMs(uint32_t ms) {

  uint32_t start = millis();

  while ((uint32_t)(millis() - start) < ms) {

    service();

    pendingKey = 0;
    pendingPaid = false;

    delay(1);
  }
}

// =================================================
// 12. LCD HIEN THI
// =================================================

void showStartup() {

  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print("MAY BAN HANG");

  lcd.setCursor(0, 1);
  lcd.print("Dang khoi dong...");

  lcd.setCursor(0, 2);
  lcd.print("Doi ESP32 / WiFi");
}

void showNetwork() {

  lcd.clear();

  if (ipMode == 'A') {

    lcd.setCursor(0, 0);
    lcd.print("WIFI RIENG");

    lcd.setCursor(0, 1);
    lcd.print("Vao WiFi MayBanHang");

    lcd.setCursor(0, 2);
    lcd.print("Go dia chi:");
  }

  else {

    lcd.setCursor(0, 0);
    lcd.print("DA VAO WIFI");

    lcd.setCursor(0, 1);
    lcd.print("Mo web tren dien");

    lcd.setCursor(0, 2);
    lcd.print("Thoai, go dia chi:");
  }

  lcd.setCursor(0, 3);
  lcd.print(ipText);
}

void showMenu() {

  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print("MAY BAN HANG");

  lcd.setCursor(0, 1);
  lcd.print("Chua chon san pham");

  lcd.setCursor(0, 2);
  lcd.print("Nhan phim 1-4");

  lcd.setCursor(0, 3);
  lcd.print("De chon san pham");
}

void showProduct() {

  if (selectedProduct < 0 || selectedProduct > 3) {
    return;
  }

  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print("SAN PHAM DA CHON");

  lcd.setCursor(0, 1);
  lcd.print("SP");
  lcd.print(selectedProduct + 1);
  lcd.print(": ");
  lcd.print(prices[selectedProduct]);
  lcd.print(" xu / ");
  lcd.print(
    (uint32_t)prices[selectedProduct] * VND_PER_COIN
  );

  lcd.setCursor(0, 2);
  lcd.print("B: tra bang xu");

  lcd.setCursor(0, 3);
  lcd.print("A: chuyen khoan QR");
}

void showPayment(uint16_t amount) {

  if (selectedProduct < 0 || selectedProduct > 3) {
    return;
  }

  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print("DANG BO XU - SP");
  lcd.print(selectedProduct + 1);

  lcd.setCursor(0, 1);
  lcd.print("Da bo: ");
  lcd.print(amount);
  lcd.print("/");
  lcd.print(prices[selectedProduct]);
  lcd.print(" xu");

  if (amount >= prices[selectedProduct]) {

    lcd.setCursor(0, 2);
    lcd.print("DA DU XU");

    lcd.setCursor(0, 3);
    lcd.print("#: MUA C: HUY");
  }

  else {

    lcd.setCursor(0, 2);
    lcd.print("Moi bo xu vao khe");

    lcd.setCursor(0, 3);
    lcd.print("C: HUY GIAO DICH");
  }
}

void showBank() {

  if (selectedProduct < 0 || selectedProduct > 3) {
    return;
  }

  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print("CHUYEN KHOAN - SP");
  lcd.print(selectedProduct + 1);

  lcd.setCursor(0, 1);
  lcd.print(
    (uint32_t)prices[selectedProduct] * VND_PER_COIN
  );
  lcd.print(" VND");

  lcd.setCursor(0, 2);
  lcd.print("Quet QR tren web");

  lcd.setCursor(0, 3);
  lcd.print("D:xac nhan C:huy");
}

// =================================================
// 13. HC-SR04
// =================================================

float readDistanceCm() {

  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(3);

  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);

  digitalWrite(TRIG_PIN, LOW);

  unsigned long duration = pulseIn(
    ECHO_PIN,
    HIGH,
    30000UL
  );

  if (duration == 0) {
    return 999.0f;
  }

  return duration * 0.0343f / 2.0f;
}

// =================================================
// 14. HUY GIAO DICH
// =================================================

void cancelTransaction() {

  coinAcceptEnabled = false;

  // Huy giao dich nhung khong mat so du xu da nap.
  stopAllMotors();

  selectedProduct = -1;

  currentCoins = getCoins();
  displayedCoins = currentCoins;

  dispensePhase = 0;

  state = SELECT_PRODUCT;

  showMenu();
}

// =================================================
// 15. XUAT SAN PHAM
// =================================================

bool dispenseProduct(bool byBank) {

  if (selectedProduct < 0 || selectedProduct > 3) {
    return false;
  }

  currentCoins = getCoins();

  if (
    !byBank &&
    currentCoins < prices[selectedProduct]
  ) {
    return false;
  }

  coinAcceptEnabled = false;

  state = DISPENSING;

  dispensePhase = 1;

  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print("DANG XUAT SAN PHAM");

  lcd.setCursor(0, 1);
  lcd.print("San pham: SP");
  lcd.print(selectedProduct + 1);

  lcd.setCursor(0, 2);
  lcd.print("Dong co dang chay");

  // Chay dong co cua san pham da chon
  motorForward(selectedProduct);

  // Cho san pham xuat hien
  uint32_t startProduct = millis();

  bool itemDetected = false;

  while (
    (uint32_t)(millis() - startProduct)
    < PRODUCT_TIMEOUT_MS
  ) {

    service();

    if (readDistanceCm() <= ITEM_PRESENT_CM) {

      itemDetected = true;
      break;
    }

    waitMs(100);
  }

  // Dung dong co
  motorStop(selectedProduct);

  if (!itemDetected) {

    stopAllMotors();

    dispensePhase = 0;

    lcd.clear();

    lcd.setCursor(0, 0);
    lcd.print("LOI XUAT SAN PHAM");

    lcd.setCursor(0, 1);
    lcd.print("Khong thay san pham");

    lcd.setCursor(0, 2);
    lcd.print("Kiem tra dong co");

    waitMs(2500);

    // San pham chua xuat hien: khong tru xu, giu nguyen so du.
    currentCoins = getCoins();
    displayedCoins = currentCoins;

    selectedProduct = -1;

    state = SELECT_PRODUCT;

    showMenu();

    return false;
  }

  // San pham da xuat hien: giao dich bang xu moi tru dung gia san pham.
  // Thanh toan QR khong lam thay doi so du xu dang co.
  if (!byBank) {

    noInterrupts();

    if (coinCount >= prices[selectedProduct]) {
      coinCount -= prices[selectedProduct];
    }

    interrupts();
  }

  currentCoins = getCoins();
  displayedCoins = currentCoins;

  // San pham da xuat hien
  dispensePhase = 2;

  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print("VUI LONG LAY HANG");

  lcd.setCursor(0, 1);
  lcd.print("Da thay san pham");

  lcd.setCursor(0, 2);
  lcd.print("Dang doi lay hang");

  // Cho khach lay san pham
  uint32_t startRemove = millis();

  bool itemRemoved = false;

  while (
    (uint32_t)(millis() - startRemove)
    < REMOVE_TIMEOUT_MS
  ) {

    service();

    if (readDistanceCm() >= ITEM_REMOVED_CM) {

      itemRemoved = true;
      break;
    }

    waitMs(100);
  }

  if (!itemRemoved) {

    lcd.clear();

    lcd.setCursor(0, 0);
    lcd.print("SAN PHAM CHUA LAY");

    lcd.setCursor(0, 1);
    lcd.print("Vui long kiem tra");

    waitMs(2000);
  }

  // Hoan tat
  dispensePhase = 4;

  stopAllMotors();

  lcd.clear();

  lcd.setCursor(0, 1);
  lcd.print("GIAO HANG HOAN TAT");

  lcd.setCursor(0, 2);
  lcd.print("Cam on quy khach!");

  waitMs(2500);

  // Bao luu so xu con lai de mua san pham tiep theo.
  currentCoins = getCoins();
  displayedCoins = currentCoins;

  selectedProduct = -1;

  state = SELECT_PRODUCT;

  dispensePhase = 0;

  pendingKey = 0;
  pendingPaid = false;

  showMenu();

  return true;
}

// =================================================
// 16. THANH TOAN QR
// =================================================

void startBankPayment() {

  if (selectedProduct < 0 || selectedProduct > 3) {
    return;
  }

  coinAcceptEnabled = false;

  // Chuyen sang QR khong xoa so du xu hien co.
  bankSeq++;

  bankStartMs = millis();

  state = BANK_PAY;

  showBank();
}

// =================================================
// 17. SETUP
// =================================================

void setup() {

  // Tat JTAG, giu SWD de su dung PB3, PB4, PA15
  // Day la cac chan JTAG mac dinh cua STM32F103
  #if defined(AFIO)
    AFIO->MAPR =
      (AFIO->MAPR & ~(7UL << 24)) | (2UL << 24);
  #endif

  // UART3: PB11 RX, PB10 TX
  Link.begin(115200);

  // LCD I2C remap:
  // PB9 SDA, PB8 SCL
  Wire.setSDA(PB9);
  Wire.setSCL(PB8);
  Wire.begin();

  lcd.init();
  lcd.backlight();

  // Cam bien xu
  pinMode(COIN_PIN, INPUT_PULLUP);

  attachInterrupt(
    digitalPinToInterrupt(COIN_PIN),
    coinSensorISR,
    FALLING
  );

  // HC-SR04
  pinMode(TRIG_PIN, OUTPUT);
  digitalWrite(TRIG_PIN, LOW);

  pinMode(ECHO_PIN, INPUT);

  // 4 dong co L298N
  // Khoi tao GPIO va dua ca hai chan dieu khien ve LOW.
  // Luu y: trong khoang truoc khi setup() chay, GPIO van co the floating;
  // nen phan cung can pull-down / khoa ENA-ENB de dam bao motor khong giat.
  for (uint8_t i = 0; i < 4; i++) {

    pinMode(motorPins[i][0], OUTPUT);
    pinMode(motorPins[i][1], OUTPUT);

    digitalWrite(motorPins[i][0], LOW);
    digitalWrite(motorPins[i][1], LOW);
  }

  stopAllMotors();

  // Keypad
  keypad.setDebounceTime(40);
  keypad.setHoldTime(500);

  coinAcceptEnabled = false;

  // Khoi dong nguon moi: bo dem xu bat dau tu 0.
  // So du hien tai chi duoc giu trong RAM trong mot phien hoat dong.
  coinClear();

  selectedProduct = -1;

  state = SELECT_PRODUCT;

  showStartup();

  // Cho ESP32 gui IP
  uint32_t t0 = millis();

  while (
    !ipReceived &&
    (uint32_t)(millis() - t0) < 25000UL
  ) {

    service();
    delay(10);
  }

  if (ipReceived) {

    showNetwork();
  }

  else {

    lcd.clear();

    lcd.setCursor(0, 0);
    lcd.print("KHONG THAY ESP32");

    lcd.setCursor(0, 1);
    lcd.print("Van ban xu duoc");

    lcd.setCursor(0, 2);
    lcd.print("Kiem tra day UART");
  }

  waitMs(4000);

  showMenu();

  ignoreKeysUntil = millis() + 1500;
}

// =================================================
// 18. LOOP
// =================================================

void loop() {

  service();

  if ((int32_t)(millis() - ignoreKeysUntil) < 0) {

    keypad.getKey();

    pendingKey = 0;
    pendingPaid = false;

    delay(2);

    return;
  }

  char key = keypad.getKey();

  if (key == NO_KEY && pendingKey != 0) {
    key = pendingKey;
  }

  pendingKey = 0;

  bool paid = pendingPaid;
  pendingPaid = false;

  currentCoins = getCoins();

  // Cap nhat so xu
  if (
    state == INSERT_COINS &&
    currentCoins != displayedCoins
  ) {

    displayedCoins = currentCoins;

    showPayment(currentCoins);
  }

  switch (state) {

    // ---------------------------------------------
    // CHON SAN PHAM
    // ---------------------------------------------

    case SELECT_PRODUCT:

      if (key >= '1' && key <= '4') {

        selectedProduct = key - '1';

        showProduct();

        state = CONFIRM_PRODUCT;
      }

      break;

    // ---------------------------------------------
    // XAC NHAN SAN PHAM
    // ---------------------------------------------

    case CONFIRM_PRODUCT:

      if (key >= '1' && key <= '4') {

        selectedProduct = key - '1';

        showProduct();
      }

      else if (key == 'B') {

        state = INSERT_COINS;

        coinStart();

        currentCoins = getCoins();
        displayedCoins = currentCoins;
        showPayment(currentCoins);
      }

      else if (key == 'A') {

        startBankPayment();
      }

      else if (key == 'C') {

        cancelTransaction();
      }

      break;

    // ---------------------------------------------
    // THANH TOAN BANG XU
    // ---------------------------------------------

    case INSERT_COINS:

      if (key == '#') {

        currentCoins = getCoins();

        if (currentCoins >= prices[selectedProduct]) {

          dispenseProduct(false);
        }

        else {

          lcd.setCursor(0, 2);
          lcd.print("CHUA DU XU!          ");

          waitMs(500);

          showPayment(currentCoins);
        }
      }

      else if (key == 'C') {

        cancelTransaction();
      }

      break;

    // ---------------------------------------------
    // THANH TOAN QR
    // ---------------------------------------------

    case BANK_PAY:

      if (key == 'D' || paid) {

        dispenseProduct(true);
      }

      else if (key == 'C') {

        cancelTransaction();
      }

      else if (
        (uint32_t)(millis() - bankStartMs)
        > BANK_TIMEOUT_MS
      ) {

        cancelTransaction();
      }

      break;

    // ---------------------------------------------
    // DANG XUAT SAN PHAM
    // ---------------------------------------------

    case DISPENSING:

      break;
  }

  delay(5);
}
