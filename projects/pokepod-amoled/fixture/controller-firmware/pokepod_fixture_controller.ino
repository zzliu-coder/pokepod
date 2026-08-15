// Reference USB controller for the PokePod six-contact fixture.
// BOOT and RESET are open-drain: OUTPUT LOW asserts, INPUT releases.

#ifndef POKEPOD_FIXTURE_BOOT_PIN
#define POKEPOD_FIXTURE_BOOT_PIN 2
#endif
#ifndef POKEPOD_FIXTURE_RESET_PIN
#define POKEPOD_FIXTURE_RESET_PIN 3
#endif
#ifndef POKEPOD_FIXTURE_POWER_PIN
#define POKEPOD_FIXTURE_POWER_PIN 4
#endif
#ifndef POKEPOD_FIXTURE_HAS_POWER
#define POKEPOD_FIXTURE_HAS_POWER 0
#endif

static void assertLow(uint8_t pin) {
  digitalWrite(pin, LOW);
  pinMode(pin, OUTPUT);
}

static void releaseLine(uint8_t pin) {
  pinMode(pin, INPUT);
}

static bool lineEquals(const String &line, const char *expected) {
  return line.equals(expected);
}

void setup() {
  releaseLine(POKEPOD_FIXTURE_BOOT_PIN);
  releaseLine(POKEPOD_FIXTURE_RESET_PIN);
#if POKEPOD_FIXTURE_HAS_POWER
  pinMode(POKEPOD_FIXTURE_POWER_PIN, OUTPUT);
  digitalWrite(POKEPOD_FIXTURE_POWER_PIN, HIGH);
#endif
  Serial.begin(115200);
}

void loop() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (lineEquals(line, "PING")) {
    Serial.println("OK");
  } else if (lineEquals(line, "BOOT ASSERT")) {
    assertLow(POKEPOD_FIXTURE_BOOT_PIN);
    Serial.println("OK");
  } else if (lineEquals(line, "BOOT RELEASE")) {
    releaseLine(POKEPOD_FIXTURE_BOOT_PIN);
    Serial.println("OK");
  } else if (lineEquals(line, "RESET PULSE")) {
    assertLow(POKEPOD_FIXTURE_RESET_PIN);
    delay(120);
    releaseLine(POKEPOD_FIXTURE_RESET_PIN);
    Serial.println("OK");
#if POKEPOD_FIXTURE_HAS_POWER
  } else if (lineEquals(line, "POWER OFF")) {
    digitalWrite(POKEPOD_FIXTURE_POWER_PIN, LOW);
    Serial.println("OK");
  } else if (lineEquals(line, "POWER ON")) {
    digitalWrite(POKEPOD_FIXTURE_POWER_PIN, HIGH);
    Serial.println("OK");
#endif
  } else {
    Serial.println("ERROR unsupported-command");
  }
}
