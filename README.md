# ESP32 кондиционер + Telegram

Управление обычным ИК-кондиционером с протоколом `ELECTRA_AC` через ESP32-WROOM-32.

В этой версии **ESP32 не нужен Wi-Fi**. Он подключён к Windows-компьютеру по USB (`COM3`). Интернетом пользуется программа Telegram-бота на компьютере.

## Архитектура

```text
iPhone / интернет
       |
       v
Telegram Bot API
       |
       | getUpdates (HTTPS)
       v
Windows-компьютер
  bot/bot.py
       |
       | USB / COM3 / 115200
       v
ESP32-WROOM-32
   |          |
   |          +-- TSOP4838 <- штатный пульт
   |
   +-- GPIO26 -> SS8050 -> TSAL6200 -> кондиционер
```

Компьютеру не нужен открытый входящий порт. Python-программа сама получает обновления Telegram и передаёт команды ESP32 строками через COM-порт.

## Что умеет

- кнопочный пульт в Telegram;
- текстовые команды вида `SET 23 COOL LOW`;
- охлаждение, обогрев, авто, осушение и вентиляция;
- температура 16–32 °C;
- вентилятор AUTO / LOW / MED / HIGH;
- вертикальный и горизонтальный Swing;
- Turbo и Quiet;
- Display как команда-переключатель;
- включение и выключение;
- TSOP4838 постоянно слушает штатный пульт и обновляет последнее известное состояние;
- последнее состояние сохраняется в памяти ESP32 после перезагрузки.

> Состояние является **последним известным**, а не подтверждением от кондиционера: сам внутренний блок по ИК ответ не передаёт.

## Подключение

Схема: [`docs/wiring.svg`](docs/wiring.svg)

Основные подключения:

```text
TSOP4838 OUT -> GPIO27
TSOP4838 GND -> GND
TSOP4838 VS  -> 3.3V

GPIO26 -> 1 кОм -> база SS8050
SS8050 эмиттер -> GND

5V -> 33 Ом -> анод TSAL6200
катод TSAL6200 -> коллектор SS8050
```

У SS8050 обязательно проверь распиновку конкретного производителя перед включением.

## 1. Прошивка ESP32

Открой в Arduino IDE:

```text
firmware/condition_telegram/condition_telegram.ino
```

Нужно установить библиотеку:

```text
IRremoteESP8266
```

Плата: `NodeMCU-32S` или `ESP32 Dev Module`.

Для текущей платы Windows определяет USB-UART как Silicon Labs CP210x. В нашем случае порт был `COM3`.

После прошивки ESP32 принимает по USB текстовые команды и отвечает строкой состояния.

Примеры для Монитора порта:

```text
SET 23 COOL LOW
SET 25 HEAT AUTO
SET 23 COOL LOW SV=ON SH=OFF
STATUS
ON
OFF
TEMP +1
FAN LOW
SWINGV TOGGLE
DISPLAY
```

`SET 23 COOL LOW` формирует **один полный ИК-пакет**: включение + 23 °C + охлаждение + низкая скорость вентилятора.

## 2. Telegram-бот на Windows

Требуется Python 3.

Файлы находятся в каталоге:

```text
bot/
```

Скопируй:

```text
config.example.py -> config.py
```

В `config.py` укажи:

```python
BOT_TOKEN = "токен от BotFather"
ALLOWED_USER_IDS = {123456789}
SERIAL_PORT = "COM3"
SERIAL_BAUD = 115200
```

`config.py` добавлен в `.gitignore`, поэтому токен Telegram не попадёт в открытый GitHub.

### Как узнать свой Telegram user ID

1. Получи токен у `@BotFather`.
2. Пока можешь оставить пример ID в `config.py`.
3. Запусти бота.
4. Напиши ему `/id`.
5. Бот покажет твой числовой Telegram user ID.
6. Запиши его в `ALLOWED_USER_IDS` и перезапусти программу.

## 3. Запуск на Windows

**Перед запуском закрой Монитор порта Arduino IDE.** COM3 может одновременно использовать только одна программа.

Запусти:

```text
bot/run.bat
```

При первом запуске скрипт создаст `.venv` и установит зависимости из `requirements.txt`.

Затем напиши боту:

```text
/start
```

Появится кнопочный пульт.

## Кнопки Telegram

Доступны:

- Включить / выключить;
- температура −1 / +1;
- Cool / Heat / Auto / Dry / Fan;
- Fan Auto / Low / Med / High;
- Swing V / Swing H;
- Display;
- Turbo / Quiet;
- Состояние.

После нажатия кнопки Python отправляет в COM3 короткую команду, например:

```text
TEMP +1
MODE HEAT
FAN LOW
```

ESP32 меняет только нужный параметр в сохранённом состоянии и затем передаёт кондиционеру полный пакет `ELECTRA_AC`.

## Текстовые команды в Telegram

Основной формат:

```text
SET <температура> <режим> <вентилятор>
```

Примеры:

```text
SET 23 COOL LOW
SET 25 HEAT AUTO
SET 22 DRY LOW
SET 24 COOL HIGH SV=ON
SET 23 COOL LOW SV=ON SH=OFF TURBO=OFF QUIET=OFF
```

Режимы:

```text
AUTO COOL DRY HEAT FAN
```

Скорости:

```text
AUTO LOW MED HIGH
```

Дополнительно бот принимает `ON`, `OFF` и `STATUS`.

## Синхронизация со штатным пультом

TSOP4838 подключён к GPIO27 постоянно. Когда кто-то нажимает кнопку штатного пульта и ESP32 принимает корректный `ELECTRA_AC` пакет, ESP32:

1. принимает полное состояние;
2. обновляет свою копию;
3. сохраняет её во внутреннюю память;
4. передаёт новое состояние программе на Windows через COM3.

`Display` в этом протоколе является командой-переключателем, поэтому ESP32 намеренно не сохраняет её как постоянный признак. Это предотвращает случайное повторное переключение табло при следующей команде.

## Структура репозитория

```text
condition_telegram/
├── bot/
│   ├── bot.py
│   ├── config.example.py
│   ├── requirements.txt
│   └── run.bat
├── firmware/
│   └── condition_telegram/
│       └── condition_telegram.ino
├── docs/
│   └── wiring.svg
├── .gitignore
└── README.md
```
