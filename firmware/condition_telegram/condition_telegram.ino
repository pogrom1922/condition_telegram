#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <Preferences.h>

#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <ir_Electra.h>

#include "secrets.h"

// --- Hardware ---------------------------------------------------------------
constexpr uint16_t IR_RECV_PIN = 27;
constexpr uint16_t IR_SEND_PIN = 26;
constexpr uint16_t CAPTURE_BUFFER_SIZE = 1024;
constexpr uint8_t RECV_TIMEOUT_MS = 50;

IRrecv irrecv(IR_RECV_PIN, CAPTURE_BUFFER_SIZE, RECV_TIMEOUT_MS, true);
decode_results irResults;
IRElectraAc ac(IR_SEND_PIN);

// --- Telegram / Wi-Fi -------------------------------------------------------
WiFiClientSecure telegramClient;
UniversalTelegramBot bot(BOT_TOKEN, telegramClient);

constexpr unsigned long TELEGRAM_POLL_MS = 900;
constexpr unsigned long WIFI_RETRY_MS = 10000;
unsigned long lastTelegramPoll = 0;
unsigned long lastWifiRetry = 0;

// --- Persistence ------------------------------------------------------------
Preferences prefs;
bool stateKnown = false;

// ---------------------------------------------------------------------------

String modeName(uint8_t mode) {
  switch (mode) {
    case kElectraAcAuto: return "AUTO";
    case kElectraAcCool: return "COOL";
    case kElectraAcDry:  return "DRY";
    case kElectraAcHeat: return "HEAT";
    case kElectraAcFan:  return "FAN";
    default: return "?";
  }
}

String fanName(uint8_t fan) {
  switch (fan) {
    case kElectraAcFanAuto: return "AUTO";
    case kElectraAcFanLow:  return "LOW";
    case kElectraAcFanMed:  return "MED";
    case kElectraAcFanHigh: return "HIGH";
    default: return "?";
  }
}

String onOff(bool value) {
  return value ? "ON" : "OFF";
}

String shortState() {
  String s;
  s.reserve(80);
  s += ac.getPower() ? "ON" : "OFF";
  s += " | ";
  s += String(ac.getTemp());
  s += "C | ";
  s += modeName(ac.getMode());
  s += " | FAN ";
  s += fanName(ac.getFan());
  return s;
}

String fullState() {
  String s;
  s.reserve(260);
  s += "Кондиционер\n";
  s += "Питание: " + onOff(ac.getPower()) + "\n";
  s += "Температура: " + String(ac.getTemp()) + " C\n";
  s += "Режим: " + modeName(ac.getMode()) + "\n";
  s += "Вентилятор: " + fanName(ac.getFan()) + "\n";
  s += "Swing V: " + onOff(ac.getSwingV()) + "\n";
  s += "Swing H: " + onOff(ac.getSwingH()) + "\n";
  s += "Turbo: " + onOff(ac.getTurbo()) + "\n";
  s += "Quiet: " + onOff(ac.getQuiet()) + "\n";
  s += "Источник: ";
  s += stateKnown ? "последнее известное состояние" : "начальное состояние ESP32";
  s += "\nDisplay: состояние не хранится, кнопка работает как переключатель";
  return s;
}

String remoteKeyboard() {
  return String(
    "[[{\"text\":\"⏻ Power\",\"callback_data\":\"PWR\"},"
      "{\"text\":\"💡 Display\",\"callback_data\":\"DISPLAY\"}],"
     "[{\"text\":\"− 1°C\",\"callback_data\":\"TEMP_DOWN\"},"
      "{\"text\":\"+ 1°C\",\"callback_data\":\"TEMP_UP\"}],"
     "[{\"text\":\"❄️ Cool\",\"callback_data\":\"MODE_COOL\"},"
      "{\"text\":\"🔥 Heat\",\"callback_data\":\"MODE_HEAT\"},"
      "{\"text\":\"♻️ Auto\",\"callback_data\":\"MODE_AUTO\"}],"
     "[{\"text\":\"💧 Dry\",\"callback_data\":\"MODE_DRY\"},"
      "{\"text\":\"🌬 Fan\",\"callback_data\":\"MODE_FAN\"}],"
     "[{\"text\":\"Fan Auto\",\"callback_data\":\"FAN_AUTO\"},"
      "{\"text\":\"Low\",\"callback_data\":\"FAN_LOW\"},"
      "{\"text\":\"Med\",\"callback_data\":\"FAN_MED\"},"
      "{\"text\":\"High\",\"callback_data\":\"FAN_HIGH\"}],"
     "[{\"text\":\"↕ Swing V\",\"callback_data\":\"SWING_V\"},"
      "{\"text\":\"↔ Swing H\",\"callback_data\":\"SWING_H\"}],"
     "[{\"text\":\"⚡ Turbo\",\"callback_data\":\"TURBO\"},"
      "{\"text\":\"🤫 Quiet\",\"callback_data\":\"QUIET\"}],"
     "[{\"text\":\"📋 Status\",\"callback_data\":\"STATUS\"}]]"
  );
}

bool accessConfigured() {
  return String(ALLOWED_CHAT_ID).length() > 0;
}

bool authorized(const String &chatId) {
  return accessConfigured() && chatId == String(ALLOWED_CHAT_ID);
}

void saveState() {
  // Display is a transient toggle flag; never persist it as an active command.
  ac.setLightToggle(false);
  uint8_t *raw = ac.getRaw();
  prefs.putBytes("raw", raw, kElectraAcStateLength);
}

bool loadState() {
  if (prefs.getBytesLength("raw") != kElectraAcStateLength) return false;

  uint8_t raw[kElectraAcStateLength];
  if (prefs.getBytes("raw", raw, sizeof(raw)) != sizeof(raw)) return false;

  ac.setRaw(raw, sizeof(raw));
  ac.setLightToggle(false);
  stateKnown = true;
  return true;
}

void setSafeDefaults() {
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
  stateKnown = false;
}

void sendCurrentState(bool displayToggle = false) {
  // Avoid receiving our own IR transmission.
  irrecv.disableIRIn();

  ac.setLightToggle(displayToggle);
  ac.send();

  delay(100);
  ac.setLightToggle(false);
  irrecv.enableIRIn();

  stateKnown = true;
  saveState();

  Serial.print(">> IR sent: ");
  Serial.println(shortState());
}

void handleIrReceiver() {
  if (!irrecv.decode(&irResults)) return;

  if (irResults.decode_type == ELECTRA_AC && irResults.bits == kElectraAcBits) {
    ac.setRaw(irResults.state, kElectraAcStateLength);

    const bool displayWasToggled = ac.getLightToggle();

    Serial.print("<< Remote: ");
    Serial.println(ac.toString());
    if (displayWasToggled) Serial.println("<< Remote included DISPLAY toggle");

    // Keep all normal state fields, but do not replay the transient Display flag later.
    ac.setLightToggle(false);
    stateKnown = true;
    saveState();
  }

  irrecv.resume();
}

bool parseOnOff(const String &value, bool &result) {
  if (value == "ON") {
    result = true;
    return true;
  }
  if (value == "OFF") {
    result = false;
    return true;
  }
  return false;
}

bool parseMode(const String &value, uint8_t &mode) {
  if (value == "AUTO") mode = kElectraAcAuto;
  else if (value == "COOL") mode = kElectraAcCool;
  else if (value == "DRY") mode = kElectraAcDry;
  else if (value == "HEAT") mode = kElectraAcHeat;
  else if (value == "FAN") mode = kElectraAcFan;
  else return false;
  return true;
}

bool parseFan(const String &value, uint8_t &fan) {
  if (value == "AUTO") fan = kElectraAcFanAuto;
  else if (value == "LOW") fan = kElectraAcFanLow;
  else if (value == "MED" || value == "MEDIUM") fan = kElectraAcFanMed;
  else if (value == "HIGH") fan = kElectraAcFanHigh;
  else return false;
  return true;
}

bool applySetCommand(String command, String &error) {
  command.trim();
  command.toUpperCase();

  char buffer[160];
  command.toCharArray(buffer, sizeof(buffer));

  char *savePtr = nullptr;
  char *token = strtok_r(buffer, " ", &savePtr);  // SET
  if (!token || String(token) != "SET") {
    error = "Формат: SET 23 COOL LOW";
    return false;
  }

  token = strtok_r(nullptr, " ", &savePtr);
  if (!token) {
    error = "Не указана температура";
    return false;
  }
  int temp = String(token).toInt();
  if (temp < kElectraAcMinTemp || temp > kElectraAcMaxTemp) {
    error = "Температура: 16..32 C";
    return false;
  }

  token = strtok_r(nullptr, " ", &savePtr);
  if (!token) {
    error = "Не указан режим: AUTO/COOL/DRY/HEAT/FAN";
    return false;
  }
  uint8_t mode;
  if (!parseMode(String(token), mode)) {
    error = "Режим: AUTO/COOL/DRY/HEAT/FAN";
    return false;
  }

  token = strtok_r(nullptr, " ", &savePtr);
  if (!token) {
    error = "Не указана скорость: AUTO/LOW/MED/HIGH";
    return false;
  }
  uint8_t fan;
  if (!parseFan(String(token), fan)) {
    error = "Вентилятор: AUTO/LOW/MED/HIGH";
    return false;
  }

  // Required SET fields.
  ac.on();
  ac.setTemp(temp);
  ac.setMode(mode);
  ac.setFan(fan);

  // Optional fields: SV=ON, SH=OFF, TURBO=ON, QUIET=OFF
  while ((token = strtok_r(nullptr, " ", &savePtr)) != nullptr) {
    String option(token);
    int eq = option.indexOf('=');
    if (eq <= 0) {
      error = "Доп. параметры: SV=ON SH=OFF TURBO=ON QUIET=OFF";
      return false;
    }

    String key = option.substring(0, eq);
    String value = option.substring(eq + 1);
    bool enabled;
    if (!parseOnOff(value, enabled)) {
      error = "Для доп. параметров используйте ON или OFF";
      return false;
    }

    if (key == "SV" || key == "SWINGV") ac.setSwingV(enabled);
    else if (key == "SH" || key == "SWINGH") ac.setSwingH(enabled);
    else if (key == "TURBO") ac.setTurbo(enabled);
    else if (key == "QUIET") ac.setQuiet(enabled);
    else {
      error = "Неизвестный параметр: " + key;
      return false;
    }
  }

  sendCurrentState(false);
  return true;
}

bool applyButtonAction(const String &action, bool &sentIr) {
  sentIr = true;

  if (action == "PWR") ac.setPower(!ac.getPower());
  else if (action == "TEMP_UP") {
    uint8_t t = ac.getTemp();
    if (t < kElectraAcMaxTemp) ac.setTemp(t + 1);
  }
  else if (action == "TEMP_DOWN") {
    uint8_t t = ac.getTemp();
    if (t > kElectraAcMinTemp) ac.setTemp(t - 1);
  }
  else if (action == "MODE_AUTO") ac.setMode(kElectraAcAuto);
  else if (action == "MODE_COOL") ac.setMode(kElectraAcCool);
  else if (action == "MODE_DRY") ac.setMode(kElectraAcDry);
  else if (action == "MODE_HEAT") ac.setMode(kElectraAcHeat);
  else if (action == "MODE_FAN") ac.setMode(kElectraAcFan);
  else if (action == "FAN_AUTO") ac.setFan(kElectraAcFanAuto);
  else if (action == "FAN_LOW") ac.setFan(kElectraAcFanLow);
  else if (action == "FAN_MED") ac.setFan(kElectraAcFanMed);
  else if (action == "FAN_HIGH") ac.setFan(kElectraAcFanHigh);
  else if (action == "SWING_V") ac.setSwingV(!ac.getSwingV());
  else if (action == "SWING_H") ac.setSwingH(!ac.getSwingH());
  else if (action == "TURBO") ac.setTurbo(!ac.getTurbo());
  else if (action == "QUIET") ac.setQuiet(!ac.getQuiet());
  else if (action == "DISPLAY") {
    sendCurrentState(true);
    return true;
  }
  else if (action == "STATUS") {
    sentIr = false;
    return true;
  }
  else return false;

  sendCurrentState(false);
  return true;
}

void sendRemote(const String &chatId) {
  bot.sendMessageWithInlineKeyboard(
    chatId,
    fullState(),
    "",
    remoteKeyboard()
  );
}

String helpText() {
  return String(
    "Команды:\n"
    "/remote - открыть кнопочный пульт\n"
    "/status - состояние\n"
    "/id - показать chat_id\n\n"
    "Полное состояние одной командой:\n"
    "SET 23 COOL LOW\n"
    "SET 25 HEAT AUTO\n"
    "SET 23 COOL LOW SV=ON SH=OFF\n\n"
    "Также: ON, OFF, DISPLAY, STATUS\n"
    "Режимы: AUTO COOL DRY HEAT FAN\n"
    "Fan: AUTO LOW MED HIGH"
  );
}

void processTextMessage(const String &chatId, String text) {
  text.trim();

  if (text == "/id") {
    bot.sendMessage(chatId, "Ваш chat_id: " + chatId, "");
    return;
  }

  if (!authorized(chatId)) {
    if (!accessConfigured()) {
      bot.sendMessage(chatId,
        "Управление заблокировано: ALLOWED_CHAT_ID не задан.\n"
        "Ваш chat_id: " + chatId + "\n"
        "Впишите его в secrets.h и прошейте ESP32 заново.", "");
    } else {
      bot.sendMessage(chatId, "Доступ запрещён.", "");
    }
    return;
  }

  String upper = text;
  upper.toUpperCase();

  if (upper == "/START" || upper == "/REMOTE" || upper == "REMOTE") {
    sendRemote(chatId);
    return;
  }

  if (upper == "/HELP" || upper == "HELP") {
    bot.sendMessage(chatId, helpText(), "");
    return;
  }

  if (upper == "/STATUS" || upper == "STATUS") {
    bot.sendMessage(chatId, fullState(), "");
    return;
  }

  if (upper == "ON") {
    ac.on();
    sendCurrentState(false);
    bot.sendMessage(chatId, shortState(), "");
    return;
  }

  if (upper == "OFF") {
    ac.off();
    sendCurrentState(false);
    bot.sendMessage(chatId, shortState(), "");
    return;
  }

  if (upper == "DISPLAY") {
    sendCurrentState(true);
    bot.sendMessage(chatId, "Display переключён | " + shortState(), "");
    return;
  }

  if (upper.startsWith("SET ")) {
    String error;
    if (applySetCommand(upper, error)) {
      bot.sendMessage(chatId, "Отправлено: " + shortState(), "");
    } else {
      bot.sendMessage(chatId, "Ошибка: " + error, "");
    }
    return;
  }

  bot.sendMessage(chatId, "Неизвестная команда.\n\n" + helpText(), "");
}

void processCallback(int index) {
  const String chatId = bot.messages[index].chat_id;
  const String queryId = bot.messages[index].query_id;
  const String action = bot.messages[index].text;

  if (!authorized(chatId)) {
    bot.answerCallbackQuery(queryId, "Доступ запрещён", true);
    return;
  }

  bool sentIr = false;
  if (!applyButtonAction(action, sentIr)) {
    bot.answerCallbackQuery(queryId, "Неизвестная кнопка", true);
    return;
  }

  if (action == "STATUS") {
    bot.answerCallbackQuery(queryId, shortState());
    bot.sendMessage(chatId, fullState(), "");
  } else if (action == "DISPLAY") {
    bot.answerCallbackQuery(queryId, "Display переключён | " + shortState());
  } else {
    bot.answerCallbackQuery(queryId, shortState());
  }
}

void handleTelegramMessages(int count) {
  for (int i = 0; i < count; i++) {
    if (bot.messages[i].type == "callback_query") {
      processCallback(i);
    } else {
      processTextMessage(bot.messages[i].chat_id, bot.messages[i].text);
    }
  }
}

void syncTimeForTls() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  unsigned long started = millis();
  time_t now = time(nullptr);
  while (now < 24 * 3600 && millis() - started < 15000) {
    delay(200);
    now = time(nullptr);
  }
}

void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.print("Wi-Fi: connecting to ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 20000) {
    delay(300);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi OK, IP: ");
    Serial.println(WiFi.localIP());
    syncTimeForTls();
  } else {
    Serial.println("Wi-Fi not connected; IR remote still works locally.");
  }
}

void maintainWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiRetry < WIFI_RETRY_MS) return;

  lastWifiRetry = millis();
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void pollTelegram() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastTelegramPoll < TELEGRAM_POLL_MS) return;

  lastTelegramPoll = millis();

  int count = bot.getUpdates(bot.last_message_received + 1);
  while (count > 0) {
    handleTelegramMessages(count);
    count = bot.getUpdates(bot.last_message_received + 1);
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  ac.begin();
  irrecv.enableIRIn();

  prefs.begin("acremote", false);
  if (!loadState()) setSafeDefaults();

  Serial.println();
  Serial.println("ESP32 ELECTRA_AC Telegram remote");
  Serial.print("Initial state: ");
  Serial.println(shortState());

  connectWifi();
  telegramClient.setCACert(TELEGRAM_CERTIFICATE_ROOT);

  if (!accessConfigured()) {
    Serial.println("ALLOWED_CHAT_ID is empty: only /id can be used until configured.");
  }
}

void loop() {
  handleIrReceiver();
  maintainWifi();
  pollTelegram();
}
