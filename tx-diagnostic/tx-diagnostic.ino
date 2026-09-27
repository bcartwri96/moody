#include <Arduino.h>

void setup() {
  Serial0.begin(115200);
  delay(500);
  Serial0.println("tx diagnostic setup reached");
}

void loop() {
  Serial0.println("tx diagnostic alive");
  delay(1000);
}
