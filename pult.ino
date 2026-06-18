#include <SPI.h>
#include <Wire.h>
#include <GyverOLED.h>
#include <nRF24L01.h>
#include <RF24.h>

GyverOLED<SSD1306_128x64, OLED_NO_BUFFER> oled;

RF24 radio(9, 10);

// Адреса труб (каналов)
const byte rxPipe[6] = "QUIZ0";      // Общая труба для приёма вопросов (для всех пультов)
const byte txPipe[6] = "ANSWER";     // Общая труба для отправки ответов (для всех пультов)

#define BTN1 6
#define BTN2 5
#define BTN3 4
#define BTN4 3
#define BTN_SEND 2

#define MY_ID 1  // ИЗМЕНИТЕ НА 1-8 ДЛЯ КАЖДОГО ПУЛЬТА

struct Packet {
  byte type;        // 1 = вопрос, 2 = вариант, 3 = ответ, 4 = подтверждение вопроса, 5 = конец опроса, 6 = повторный вопрос
  byte sender;      // ID отправителя
  byte targetId;    // Для целевого пульта (если 0 - всем)
  byte part;
  byte last;
  char text[30];
};

Packet packet;

String currentText = "";
String question = "";
String options[4];
int optionCount = 0;
bool quizReady = false;
int selected = 0;
int lastSelected = 0;
unsigned long lastButton = 0;
bool answerConfirmed = false;
bool questionActive = false;
uint8_t currentQuestionNum = 0;

void drawFullDisplay() {
  oled.clear();
  oled.update();

  oled.setCursor(0, 0);
  oled.autoPrintln(1);
  oled.println(question);

  // Смещение на 2 пикселя влево
  oled.setCursor(2, 4);
  if (selected == 1) oled.print(">");
  else oled.print(" ");
  oled.println(options[0]);

  oled.setCursor(2, 5);
  if (selected == 2) oled.print(">");
  else oled.print(" ");
  oled.println(options[1]);

  oled.setCursor(2, 6);
  if (selected == 3) oled.print(">");
  else oled.print(" ");
  oled.println(options[2]);

  oled.setCursor(2, 7);
  if (selected == 4) oled.print(">");
  else oled.print(" ");
  oled.println(options[3]);

  oled.update();
}

void updateCursor(int oldSelected, int newSelected) {
  // Очищаем старый курсор
  oled.setCursor(2, 3 + oldSelected);
  oled.print(" ");

  // Рисуем новый курсор
  oled.setCursor(2, 3 + newSelected);
  oled.print(">");

  oled.update();
}

void sendAckQuiz() {
  // Отправляем подтверждение получения вопроса
  Packet ack;
  ack.type = 4;
  ack.sender = MY_ID;
  ack.targetId = 0;
  ack.part = 0;
  ack.last = 1;
  memset(ack.text, 0, sizeof(ack.text));
  itoa(currentQuestionNum, ack.text, 10);

  radio.stopListening();
  delay(3);

  // Отправляем несколько раз для надёжности
  for (int i = 0; i < 3; i++) {
    radio.write(&ack, sizeof(ack));
    delay(5);
  }

  delay(3);
  radio.startListening();
}

void processMessage(byte type, String msg) {
  if (type == 1) {
    // Получен новый вопрос
    question = msg;
    optionCount = 0;
    quizReady = false;
    selected = 1;
    lastSelected = 1;
    answerConfirmed = false;
    questionActive = true;

    Serial.println("QUESTION RECEIVED:");
    Serial.println(question);
  }

  if (type == 2) {
    // Получен вариант ответа
    if (optionCount < 4) {
      options[optionCount] = msg;
      optionCount++;

      Serial.print("OPTION ");
      Serial.print(optionCount);
      Serial.print(": ");
      Serial.println(msg);

      if (optionCount == 4) {
        quizReady = true;
        Serial.println("ALL OPTIONS RECEIVED!");
        drawFullDisplay();
        
        // Отправляем подтверждение получения вопроса
        sendAckQuiz();
      }
    }
  }

  if (type == 5) {
    // Конец опроса
    questionActive = false;
    quizReady = false;
    optionCount = 0;
    oled.clear();
    oled.setCursor(0, 0);
    oled.println("QUESTION CLOSED");
    oled.update();
  }

  if (type == 6) {
    // Повторный вопрос (если не было подтверждения)
    question = msg;
    optionCount = 0;
    quizReady = false;
    selected = 1;
    lastSelected = 1;
    answerConfirmed = false;
    questionActive = true;

    Serial.println("REPEAT QUESTION RECEIVED:");
    Serial.println(question);
  }
}

void handleRadio() {
  while (radio.available()) {
    radio.read(&packet, sizeof(packet));

    // Проверяем, этот пакет для нас или для всех
    if (packet.targetId != 0 && packet.targetId != MY_ID) {
      continue; // Пропускаем, если не для нас
    }

    // Если это вопрос или повторный вопрос, сохраняем номер
    if (packet.type == 1 || packet.type == 6) {
      currentQuestionNum = packet.sender; // Используем sender как номер вопроса
    }

    currentText += packet.text;

    if (packet.last) {
      processMessage(packet.type, currentText);
      currentText = "";
    }
  }
}

void sendAnswer() {
  if (!quizReady) return;
  if (selected == 0) return;
  if (!questionActive) return;

  answerConfirmed = false;
  unsigned long sendTime = millis();
  const unsigned long TIMEOUT = 1000; // Таймаут 1 секунда
  int attempts = 0;

  // Отправляем ответ до получения подтверждения
  while (!answerConfirmed && (millis() - sendTime < TIMEOUT)) {
    Packet ans;
    ans.type = 3;
    ans.sender = MY_ID;
    ans.targetId = 0;
    ans.part = 0;
    ans.last = 1;
    memset(ans.text, 0, sizeof(ans.text));

    itoa(selected, ans.text, 10);

    radio.stopListening();
    delay(3);

    radio.write(&ans, sizeof(ans));
    attempts++;

    delay(3);
    radio.startListening();

    // Ждём подтверждения
    unsigned long ackWait = millis();
    while (!answerConfirmed && (millis() - ackWait < 100)) {
      handleRadio();
      delay(2);
    }
  }

  oled.clear();
  oled.setCursor(0, 0);

  if (answerConfirmed) {
    oled.println("ANSWER SENT");
    Serial.print("ANSWER CONFIRMED! Attempts: ");
    Serial.println(attempts);
  } else {
    oled.println("SEND FAILED");
    Serial.print("SEND TIMEOUT! Attempts: ");
    Serial.println(attempts);
  }

  oled.update();

  delay(1500);
  quizReady = false;
  questionActive = false;
}

void handleButtons() {
  if (!quizReady) return;
  if (millis() - lastButton < 200) return;

  if (digitalRead(BTN1) == LOW) {
    lastSelected = selected;
    selected = 1;
    if (lastSelected != selected) {
      updateCursor(lastSelected, selected);
    }
    lastButton = millis();
  }

  if (digitalRead(BTN2) == LOW) {
    lastSelected = selected;
    selected = 2;
    if (lastSelected != selected) {
      updateCursor(lastSelected, selected);
    }
    lastButton = millis();
  }

  if (digitalRead(BTN3) == LOW) {
    lastSelected = selected;
    selected = 3;
    if (lastSelected != selected) {
      updateCursor(lastSelected, selected);
    }
    lastButton = millis();
  }

  if (digitalRead(BTN4) == LOW) {
    lastSelected = selected;
    selected = 4;
    if (lastSelected != selected) {
      updateCursor(lastSelected, selected);
    }
    lastButton = millis();
  }

  if (digitalRead(BTN_SEND) == LOW) {
    sendAnswer();
    lastButton = millis();
  }
}

void setup() {
  Serial.begin(9600);
  delay(1000);

  Serial.print("Pult ID: ");
  Serial.println(MY_ID);

  oled.init();
  Wire.setClock(400000L);
  oled.clear();
  oled.update();

  oled.setCursor(0, 0);
  oled.println("Pult #");
  oled.println(MY_ID);
  oled.println("WAIT QUIZ");
  oled.update();

  pinMode(BTN1, INPUT_PULLUP);
  pinMode(BTN2, INPUT_PULLUP);
  pinMode(BTN3, INPUT_PULLUP);
  pinMode(BTN4, INPUT_PULLUP);
  pinMode(BTN_SEND, INPUT_PULLUP);

  if (!radio.begin()) {
    Serial.println("NRF24 ERROR!");
    while (1);
  }

  radio.setChannel(0x60);
  radio.setPALevel(RF24_PA_MAX);
  radio.setDataRate(RF24_1MBPS);

  // Открываем трубу для чтения вопросов (общая для всех)
  radio.openReadingPipe(1, rxPipe);
  
  // Открываем трубу для письма ответов (общая для всех)
  radio.openWritingPipe(txPipe);

  radio.startListening();

  Serial.print("RX Pipe: ");
  Serial.println((char*)rxPipe);
  Serial.print("TX Pipe: ");
  Serial.println((char*)txPipe);
  Serial.println("NRF24 OK - Waiting for quiz");
}

void loop() {
  handleRadio();
  handleButtons();
}
