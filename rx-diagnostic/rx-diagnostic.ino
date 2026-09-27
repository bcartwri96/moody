#include <Arduino.h>

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("rx diagnostic application reached setup");
}

void loop() {
  Serial.println("rx diagnostic heartbeat");
  delay(1000);
}
