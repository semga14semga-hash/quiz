#include <SPI.h>
#include <nRF24L01.h>
#include <RF24.h>

RF24 radio(9, 10);

// Общая труба для отправки вопросов всем
const byte txPipe[6] = "QUIZ0";

// Общая труба для приёма ответов и подтверждений от пультов
const byte rxPipe[6] = "ANSWER";

#define MY_ID 1

struct Packet {
  byte type;        // 1 = вопрос, 2 = вариант, 3 = ответ, 4 = подтверждение вопроса, 5 = конец опроса, 6 = повторный вопрос
  byte sender;      // ID отправителя
  byte targetId;    // Целевой ID (0 = всем)
  byte part;
  byte last;
  char text[30];
};

Packet packet;

uint8_t currentQuestion = 0;
bool quizInProgress = false;

// Массив для отслеживания подтверждений от каждого пульта
bool ackReceived[9] = {false, false, false, false, false, false, false, false, false};
unsigned long quizSendTime = 0;
const unsigned long ACK_TIMEOUT = 1500; // 1.5 секунды на подтверждение

void switchToTx() {
  radio.stopListening();
  delay(10);
}

void switchToRx() {
  radio.startListening();
  delay(10);
}

void sendChunk(byte type, byte targetId, byte part, bool last, const char* data) {
  packet.type = type;
  packet.sender = currentQuestion; // Для вопросов - это номер вопроса
  packet.targetId = targetId;      // 0 = всем, 1-8 = конкретному пульту
  packet.part = part;
  packet.last = last;

  memset(packet.text, 0, sizeof(packet.text));
  strncpy(packet.text, data, 29);

  switchToTx();
  radio.write(&packet, sizeof(packet));
  delay(10);
  switchToRx();

  delay(15);
}

void sendText(byte type, byte targetId, String txt) {
  int len = txt.length();
  byte part = 0;

  for (int i = 0; i < len; i += 29) {
    char chunk[30];
    memset(chunk, 0, sizeof(chunk));
    txt.substring(i, i + 29).toCharArray(chunk, 30);

    bool last = (i + 29 >= len);
    sendChunk(type, targetId, part, last, chunk);

    part++;
    delay(20);
  }
}

void sendQuiz(uint8_t qNum, String q, String a1, String a2, String a3, String a4) {
  currentQuestion = qNum;
  
  Serial.println("\n--- SENDING QUIZ ---");
  
  // Отправляем всем пультам
  sendText(1, 0, q);
  delay(200);
  sendText(2, 0, a1);
  delay(200);
  sendText(2, 0, a2);
  delay(200);
  sendText(2, 0, a3);
  delay(200);
  sendText(2, 0, a4);

  // Инициализируем массив подтверждений
  for (int i = 1; i <= 8; i++) {
    ackReceived[i] = false;
  }

  quizSendTime = millis();
  quizInProgress = true;

  Serial.print("QUIZ #");
  Serial.print(qNum);
  Serial.println(" SENT TO ALL");
  Serial.println("WAITING FOR ACK FROM ALL PULTS...");
}

void sendQuizToSpecific(uint8_t pultId, uint8_t qNum, String q, String a1, String a2, String a3, String a4) {
  currentQuestion = qNum;
  
  Serial.print("\n--- RESENDING QUIZ TO PULT ");
  Serial.print(pultId);
  Serial.println(" ---");
  
  // Отправляем только конкретному пульту
  sendText(6, pultId, q); // type 6 = повторный вопрос
  delay(200);
  sendText(2, pultId, a1);
  delay(200);
  sendText(2, pultId, a2);
  delay(200);
  sendText(2, pultId, a3);
  delay(200);
  sendText(2, pultId, a4);

  ackReceived[pultId] = false;
  quizSendTime = millis();

  Serial.print("REPEAT QUIZ #");
  Serial.print(qNum);
  Serial.print(" SENT TO PULT ");
  Serial.println(pultId);
}

void sendEnd() {
  Serial.println("\n--- SENDING END SIGNAL ---");
  
  Packet endp;
  endp.type = 5; // конец опроса
  endp.sender = MY_ID;
  endp.targetId = 0;
  endp.part = 0;
  endp.last = 1;
  memset(endp.text, 0, sizeof(endp.text));

  switchToTx();
  for (int i = 0; i < 5; i++) {
    radio.write(&endp, sizeof(endp));
    delay(15);
  }
  switchToRx();

  Serial.println("END SENT TO ALL PULTS");
  quizInProgress = false;
}

void sendAck(byte pultId) {
  // Отправляем подтверждение пульту о получении ответа
  Packet ack;
  ack.type = 99; // Специальный тип для ACK ответа
  ack.sender = MY_ID;
  ack.targetId = pultId;
  ack.part = 0;
  ack.last = 1;
  memset(ack.text, 0, sizeof(ack.text));

  switchToTx();
  for (int i = 0; i < 2; i++) {
    radio.write(&ack, sizeof(ack));
    delay(5);
  }
  switchToRx();
}

void checkAnswers() {
  while (radio.available()) {
    radio.read(&packet, sizeof(packet));

    if (packet.type == 3) {
      // Получен ответ
      Serial.print("ID:");
      Serial.print(packet.sender);
      Serial.print("/Q:");
      Serial.print(currentQuestion);
      Serial.print("/A:");
      Serial.println(packet.text);
      
      // Отправляем ACK пульту
      sendAck(packet.sender);
    }

    if (packet.type == 4) {
      // Получено подтверждение получения вопроса
      if (packet.sender >= 1 && packet.sender <= 8) {
        ackReceived[packet.sender] = true;
        Serial.print("ID:");
        Serial.print(packet.sender);
        Serial.println(" - QUIZ RECEIVED");
      }
    }
  }
}

void waitForAllAck() {
  unsigned long startTime = millis();
  
  Serial.print("Waiting for ACK... ");
  
  while (millis() - startTime < ACK_TIMEOUT) {
    checkAnswers();
    delay(30);

    // Проверяем, получены ли подтверждения от всех пультов
    bool allAcked = true;
    for (int i = 1; i <= 8; i++) {
      if (!ackReceived[i]) {
        allAcked = false;
        break;
      }
    }

    if (allAcked) {
      Serial.println("ALL PULTS CONFIRMED!");
      delay(500);
      return;
    }
  }

  // Таймаут истёк - проверяем кто не ответил
  Serial.println("TIMEOUT!");
  Serial.println("Missing ACK from:");
  for (int i = 1; i <= 8; i++) {
    if (!ackReceived[i]) {
      Serial.print("PULT ");
      Serial.println(i);
    }
  }
}

String waitLine() {
  while (!Serial.available()) {
    checkAnswers();
    delay(10);
  }
  return Serial.readStringUntil('\n');
}

void createQuiz() {
  static uint8_t questionNum = 2;

  Serial.println("\nVOPROS:");
  String q = waitLine();

  Serial.println("VARIANT 1:");
  String a1 = waitLine();

  Serial.println("VARIANT 2:");
  String a2 = waitLine();

  Serial.println("VARIANT 3:");
  String a3 = waitLine();

  Serial.println("VARIANT 4:");
  String a4 = waitLine();

  sendQuiz(questionNum, q, a1, a2, a3, a4);
  waitForAllAck();
  questionNum++;
}

void printMenu() {
  Serial.println("\n=== RECEIVER MENU ===");
  Serial.println("/quiz - send default quiz (question #1)");
  Serial.println("/new - create and send new quiz");
  Serial.println("/end - signal end of question");
  Serial.println("/help - show this menu");
}

void setup() {
  Serial.begin(9600);
  delay(1000);

  if (!radio.begin()) {
    Serial.println("NRF24 error!");
    while (1);
  }

  radio.setChannel(0x60);
  radio.setPALevel(RF24_PA_MAX);
  radio.setDataRate(RF24_1MBPS);

  // Открываем трубу для отправки вопросов
  radio.openWritingPipe(txPipe);
  
  // Открываем трубу для приёма ответов и подтверждений
  radio.openReadingPipe(1, rxPipe);

  radio.startListening();

  Serial.println("\n=== QUIZ RECEIVER STARTED ===");
  Serial.print("TX Pipe: ");
  Serial.println((char*)txPipe);
  Serial.print("RX Pipe: ");
  Serial.println((char*)rxPipe);
  
  printMenu();
}

void loop() {
  checkAnswers();

  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();

    if (cmd == "/quiz") {
      sendQuiz(
        1,
        "Сколько апельсинов съела маша после обеда?",
        "28 апельсинов",
        "Геометрия",
        "Недостаточно много",
        "Что такое апельсин?"
      );
      waitForAllAck();
    }

    else if (cmd == "/new") {
      createQuiz();
    }

    else if (cmd == "/end") {
      sendEnd();
    }

    else if (cmd == "/help") {
      printMenu();
    }

    else {
      Serial.println("Unknown command! Type /help for menu");
    }
  }
}
