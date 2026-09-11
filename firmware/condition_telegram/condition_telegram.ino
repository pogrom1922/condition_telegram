#include <Preferences.h>

#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <ir_Electra.h>

// ESP32-WROOM-32 / NodeMCU-32S
constexpr uint16_t IR_RECV_PIN = 27;
constexpr uint16_t IR_SEND_PIN = 26;
constexpr uint16_t CAPTURE_BUFFER_SIZE = 1024;
constexpr uint8_t RECV_TIMEOUT_MS = 50;

IRrecv irrecv(IR_RECV_PIN, CAPTURE_BUFFER_SIZE, RECV_TIMEOUT_MS, true);
decode_results irResults;
IRElectraAc ac(IR_SEND_PIN);
Preferences prefs;

String serialBuffer;

String modeName(uint8_t mode) {
  switch (mode) {
    case kElectraAcAuto: return "AUTO";
    case kElectraAcCool: return "COOL";
    case kElectraAcDry:  return "DRY";
    case kElectraAcHeat: return "HEAT";
    case kElectraAcFan:  return "FAN";
    default: return "UNKNOWN";
  }
}

String fanName(uint8_t fan) {
  switch (fan) {
    case kElectraAcFanAuto: return "AUTO";
    case kElectraAcFanLow:  return "LOW";
    case kElectraAcFanMed:  return "MED";
    case kElectraAcFanHigh: return "HIGH";
    default: return "UNKNOWN";
  }
}

const char* onOff(bool value) {
  return value ? "ON" : "OFF";
}

void printState(const char* source) {
  Serial.print("STATE SOURCE=");
  Serial.print(source);
  Serial.print(" POWER=");
  Serial.print(onOff(ac.getPower()));
  Serial.print(" TEMP=");
  Serial.print(ac.getTemp());
  Serial.print(" MODE=");
  Serial.print(modeName(ac.getMode()));
  Serial.print(" FAN=");
  Serial.print(fanName(ac.getFan()));
  Serial.print(" SV=");
  Serial.print(onOff(ac.getSwingV()));
  Serial.print(" SH=");
  Serial.print(onOff(ac.getSwingH()));
  Serial.print(" TURBO=");
  Serial.print(onOff(ac.getTurbo()));
  Serial.print(" QUIET=");
  Serial.println(onOff(ac.getQuiet()));
}

void saveState() {
  uint8_t* raw = ac.getRaw();
  prefs.putBytes("raw", raw, kElectraAcStateLength);
}

void loadState() {
  if (prefs.getBytesLength("raw") == kElectraAcStateLength) {
    uint8_t raw[kElectraAcStateLength];
    prefs.getBytes("raw", raw, sizeof(raw));
    ac.setRaw(raw, sizeof(raw));
    // Display is a momentary toggle in ELECTRA_AC, not persistent state.
    ac.setLightToggle(false);
    return;
  }

  ac.stateReset();
  ac.off();
  ac.setMode(kElectraAcCool);
  ac.setTemp(24);
  ac.setFan(kElectraAcFanAuto);
  ac.setSwingV(false);
  ac.setSwingH(false);
  ac.setTurbo(false);
  ac.setQuiet(false);
  ac.setLightToggle(false);
  saveState();
}

void sendStableState(const char* source) {
  // Never repeat a previously received Display toggle accidentally.
  ac.setLightToggle(false);

  irrecv.disableIRIn();
  ac.send();
  delay(100);
  irrecv.enableIRIn();

  saveState();
  Serial.println("OK");
  printState(source);
}

void sendDisplayToggle() {
  ac.setLightToggle(true);

  irrecv.disableIRIn();
  ac.send();
  delay(100);
  irrecv.enableIRIn();

  // Toggle is one-shot. Do not persist it.
  ac.setLightToggle(false);
  saveState();
  Serial.println("OK DISPLAY");
  printState("SERIAL");
}

bool parseMode(String value, uint8_t& result) {
  value.toUpperCase();
  if (value == "AUTO") result = kElectraAcAuto;
  else if (value == "COOL") result = kElectraAcCool;
  else if (value == "DRY") result = kElectraAcDry;
  else if (value == "HEAT") result = kElectraAcHeat;
  else if (value == "FAN") result = kElectraAcFan;
  else return false;
  return true;
}

bool parseFan(String value, uint8_t& result) {
  value.toUpperCase();
  if (value == "AUTO") result = kElectraAcFanAuto;
  else if (value == "LOW") result = kElectraAcFanLow;
  else if (value == "MED") result = kElectraAcFanMed;
  else if (value == "HIGH") result = kElectraAcFanHigh;
  else return false;
  return true;
}

bool applySwitch(String value, bool current, bool& result) {
  value.toUpperCase();
  if (value == "ON") result = true;
  else if (value == "OFF") result = false;
  else if (value == "TOGGLE") result = !current;
  else return false;
  return true;
}

int splitTokens(const String& input, String tokens[], int maxTokens) {
  int count = 0;
  int start = 0;
  const int len = input.length();

  while (start < len && count < maxTokens) {
    while (start < len && input[start] == ' ') start++;
    if (start >= len) break;

    int end = input.indexOf(' ', start);
    if (end < 0) end = len;
    tokens[count++] = input.substring(start, end);
    start = end + 1;
  }
  return count;
}

bool parseOptionalFlag(const String& token, const String& key, bool current, bool& value) {
  const String prefix = key + "=";
  if (!token.startsWith(prefix)) return false;
  String arg = token.substring(prefix.length());
  return applySwitch(arg, current, value);
}

void handleSet(String tokens[], int count) {
  if (count < 4) {
    Serial.println("ERR FORMAT: SET 23 COOL LOW [SV=ON] [SH=OFF] [TURBO=OFF] [QUIET=OFF]");
    return;
  }

  const int temp = tokens[1].toInt();
  if (temp < kElectraAcMinTemp || temp > kElectraAcMaxTemp) {
    Serial.println("ERR TEMP must be 16..32");
    return;
  }

  uint8_t mode;
  if (!parseMode(tokens[2], mode)) {
    Serial.println("ERR MODE: AUTO COOL DRY HEAT FAN");
    return;
  }

  uint8_t fan;
  if (!parseFan(tokens[3], fan)) {
    Serial.println("ERR FAN: AUTO LOW MED HIGH");
    return;
  }

  bool sv = ac.getSwingV();
  bool sh = ac.getSwingH();
  bool turbo = ac.getTurbo();
  bool quiet = ac.getQuiet();

  for (int i = 4; i < count; i++) {
    String t = tokens[i];
    t.toUpperCase();
    bool parsed = false;

    if (t.startsWith("SV=")) parsed = parseOptionalFlag(t, "SV", sv, sv);
    else if (t.startsWith("SH=")) parsed = parseOptionalFlag(t, "SH", sh, sh);
    else if (t.startsWith("TURBO=")) parsed = parseOptionalFlag(t, "TURBO", turbo, turbo);
    else if (t.startsWith("QUIET=")) parsed = parseOptionalFlag(t, "QUIET", quiet, quiet);

    if (!parsed) {
      Serial.print("ERR unknown option: ");
      Serial.println(tokens[i]);
      return;
    }
  }

  ac.on();
  ac.setTemp(temp);
  ac.setMode(mode);
  ac.setFan(fan);
  ac.setSwingV(sv);
  ac.setSwingH(sh);
  ac.setTurbo(turbo);
  ac.setQuiet(quiet);
  sendStableState("SERIAL");
}

void handleCommand(String line) {
  line.trim();
  line.toUpperCase();
  if (line.length() == 0) return;

  String tokens[10];
  const int count = splitTokens(line, tokens, 10);
  if (count == 0) return;

  if (tokens[0] == "STATUS") {
    printState("STATUS");
    return;
  }

  if (tokens[0] == "SET") {
    handleSet(tokens, count);
    return;
  }

  if (tokens[0] == "ON") {
    ac.on();
    sendStableState("SERIAL");
    return;
  }

  if (tokens[0] == "OFF") {
    ac.off();
    sendStableState("SERIAL");
    return;
  }

  if (tokens[0] == "POWER" && count == 2 && tokens[1] == "TOGGLE") {
    ac.setPower(!ac.getPower());
    sendStableState("SERIAL");
    return;
  }

  if (tokens[0] == "TEMP" && count == 2) {
    int temp = ac.getTemp();
    if (tokens[1] == "+1") temp++;
    else if (tokens[1] == "-1") temp--;
    else temp = tokens[1].toInt();

    if (temp < kElectraAcMinTemp || temp > kElectraAcMaxTemp) {
      Serial.println("ERR TEMP must be 16..32");
      return;
    }
    ac.setTemp(temp);
    ac.on();
    sendStableState("SERIAL");
    return;
  }

  if (tokens[0] == "MODE" && count == 2) {
    uint8_t mode;
    if (!parseMode(tokens[1], mode)) {
      Serial.println("ERR MODE: AUTO COOL DRY HEAT FAN");
      return;
    }
    ac.setMode(mode);
    ac.on();
    sendStableState("SERIAL");
    return;
  }

  if (tokens[0] == "FAN" && count == 2) {
    uint8_t fan;
    if (!parseFan(tokens[1], fan)) {
      Serial.println("ERR FAN: AUTO LOW MED HIGH");
      return;
    }
    ac.setFan(fan);
    ac.on();
    sendStableState("SERIAL");
    return;
  }

  if ((tokens[0] == "SWINGV" || tokens[0] == "SWINGH") && count == 2) {
    const bool vertical = tokens[0] == "SWINGV";
    const bool current = vertical ? ac.getSwingV() : ac.getSwingH();
    bool value;
    if (!applySwitch(tokens[1], current, value)) {
      Serial.println("ERR use ON, OFF or TOGGLE");
      return;
    }
    if (vertical) ac.setSwingV(value);
    else ac.setSwingH(value);
    sendStableState("SERIAL");
    return;
  }

  if ((tokens[0] == "TURBO" || tokens[0] == "QUIET") && count == 2) {
    const bool turbo = tokens[0] == "TURBO";
    const bool current = turbo ? ac.getTurbo() : ac.getQuiet();
    bool value;
    if (!applySwitch(tokens[1], current, value)) {
      Serial.println("ERR use ON, OFF or TOGGLE");
      return;
    }
    if (turbo) ac.setTurbo(value);
    else ac.setQuiet(value);
    sendStableState("SERIAL");
    return;
  }

  if (tokens[0] == "DISPLAY") {
    sendDisplayToggle();
    return;
  }

  Serial.println("ERR unknown command");
}

void readSerialCommands() {
  while (Serial.available()) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\n' || c == '\r') {
      if (serialBuffer.length() > 0) {
        handleCommand(serialBuffer);
        serialBuffer = "";
      }
    } else if (serialBuffer.length() < 180) {
      serialBuffer += c;
    }
  }
}

void readPhysicalRemote() {
  if (!irrecv.decode(&irResults)) return;

  if (irResults.decode_type == ELECTRA_AC && irResults.bits == 104) {
    ac.setRaw(irResults.state, kElectraAcStateLength);

    // Display command is a momentary toggle. The real display state cannot be
    // inferred from the protocol, so remove the toggle before saving state.
    const bool displayEvent = ac.getLightToggle();
    ac.setLightToggle(false);
    saveState();

    printState(displayEvent ? "REMOTE_DISPLAY" : "REMOTE");
  }

  irrecv.resume();
}

void setup() {
  Serial.begin(115200);
  serialBuffer.reserve(180);

  prefs.begin("conditioner", false);
  ac.begin();
  loadState();
  irrecv.enableIRIn();

  delay(300);
  Serial.println("READY ESP32 ELECTRA_AC SERIAL BRIDGE");
  printState("BOOT");
}

void loop() {
  readPhysicalRemote();
  readSerialCommands();
}
