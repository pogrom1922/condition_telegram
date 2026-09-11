import asyncio
import logging
import threading
import time
from typing import Dict, Optional

import serial
from telegram import InlineKeyboardButton, InlineKeyboardMarkup, Update
from telegram.error import BadRequest
from telegram.ext import (
    ApplicationBuilder,
    CallbackQueryHandler,
    CommandHandler,
    ContextTypes,
    MessageHandler,
    filters,
)

from config import ALLOWED_USER_IDS, BOT_TOKEN, SERIAL_BAUD, SERIAL_PORT

logging.basicConfig(
    format="%(asctime)s | %(levelname)s | %(message)s",
    level=logging.INFO,
)
log = logging.getLogger("condition_bot")


def parse_state_line(line: str) -> Dict[str, str]:
    if not line.startswith("STATE "):
        return {}
    state: Dict[str, str] = {}
    for token in line.split()[1:]:
        if "=" in token:
            key, value = token.split("=", 1)
            state[key] = value
    return state


def format_state(state: Dict[str, str]) -> str:
    if not state:
        return "Нет данных о состоянии ESP32."

    modes = {
        "AUTO": "Авто", "COOL": "Охлаждение", "DRY": "Осушение",
        "HEAT": "Обогрев", "FAN": "Вентиляция",
    }
    fans = {
        "AUTO": "Авто", "LOW": "Низкая", "MED": "Средняя", "HIGH": "Высокая",
    }
    power = "Включён" if state.get("POWER") == "ON" else "Выключен"
    onoff = lambda key: "Вкл" if state.get(key) == "ON" else "Выкл"

    return (
        f"Кондиционер: {power}\n"
        f"Температура: {state.get('TEMP', '?')} °C\n"
        f"Режим: {modes.get(state.get('MODE', ''), state.get('MODE', '?'))}\n"
        f"Вентилятор: {fans.get(state.get('FAN', ''), state.get('FAN', '?'))}\n"
        f"Swing V: {onoff('SV')} | Swing H: {onoff('SH')}\n"
        f"Turbo: {onoff('TURBO')} | Quiet: {onoff('QUIET')}\n"
        f"Источник: {state.get('SOURCE', '—')}"
    )


def marked(label: str, selected: bool) -> str:
    return f"• {label}" if selected else label


def keyboard(state: Dict[str, str]) -> InlineKeyboardMarkup:
    temp = state.get("TEMP", "—")
    mode = state.get("MODE", "")
    fan = state.get("FAN", "")
    power_on = state.get("POWER") == "ON"

    return InlineKeyboardMarkup([
        [
            InlineKeyboardButton("⏻ Выключить" if power_on else "⏻ Включить", callback_data="POWER"),
            InlineKeyboardButton("Состояние", callback_data="STATUS"),
        ],
        [
            InlineKeyboardButton("−1 °C", callback_data="TEMP_DOWN"),
            InlineKeyboardButton(f"{temp} °C", callback_data="STATUS"),
            InlineKeyboardButton("+1 °C", callback_data="TEMP_UP"),
        ],
        [
            InlineKeyboardButton(marked("Cool", mode == "COOL"), callback_data="MODE_COOL"),
            InlineKeyboardButton(marked("Heat", mode == "HEAT"), callback_data="MODE_HEAT"),
            InlineKeyboardButton(marked("Auto", mode == "AUTO"), callback_data="MODE_AUTO"),
        ],
        [
            InlineKeyboardButton(marked("Dry", mode == "DRY"), callback_data="MODE_DRY"),
            InlineKeyboardButton(marked("Fan", mode == "FAN"), callback_data="MODE_FAN"),
        ],
        [
            InlineKeyboardButton(marked("Fan Auto", fan == "AUTO"), callback_data="FAN_AUTO"),
            InlineKeyboardButton(marked("Low", fan == "LOW"), callback_data="FAN_LOW"),
        ],
        [
            InlineKeyboardButton(marked("Med", fan == "MED"), callback_data="FAN_MED"),
            InlineKeyboardButton(marked("High", fan == "HIGH"), callback_data="FAN_HIGH"),
        ],
        [
            InlineKeyboardButton("Swing ↕", callback_data="SWING_V"),
            InlineKeyboardButton("Swing ↔", callback_data="SWING_H"),
            InlineKeyboardButton("Display", callback_data="DISPLAY"),
        ],
        [
            InlineKeyboardButton("Turbo", callback_data="TURBO"),
            InlineKeyboardButton("Quiet", callback_data="QUIET"),
        ],
    ])


class SerialBridge:
    def __init__(self, port: str, baud: int):
        self.port = port
        self.baud = baud
        self.ser: Optional[serial.Serial] = None
        self.stop_event = threading.Event()
        self.thread = threading.Thread(target=self._reader, daemon=True)
        self.write_lock = threading.Lock()
        self.condition = threading.Condition()
        self.event_seq = 0
        self.last_event = ""
        self.latest_state: Dict[str, str] = {}

    @property
    def connected(self) -> bool:
        return bool(self.ser and self.ser.is_open)

    def start(self) -> None:
        self.thread.start()

    def stop(self) -> None:
        self.stop_event.set()
        self._close()

    def _close(self) -> None:
        try:
            if self.ser:
                self.ser.close()
        except Exception:
            pass
        self.ser = None

    def _reader(self) -> None:
        while not self.stop_event.is_set():
            if not self.connected:
                try:
                    self.ser = serial.Serial(self.port, self.baud, timeout=0.25)
                    time.sleep(2.0)  # Opening COM can reset ESP32.
                    log.info("ESP32 connected: %s @ %s", self.port, self.baud)
                except Exception as exc:
                    log.warning("Cannot open %s: %s", self.port, exc)
                    self._close()
                    time.sleep(2.0)
                    continue

            try:
                assert self.ser is not None
                raw = self.ser.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").strip()
                if not line:
                    continue
                log.info("ESP32 << %s", line)

                if line.startswith("STATE "):
                    parsed = parse_state_line(line)
                    if parsed:
                        self.latest_state = parsed

                if line.startswith("STATE ") or line.startswith("ERR "):
                    with self.condition:
                        self.event_seq += 1
                        self.last_event = line
                        self.condition.notify_all()
            except Exception as exc:
                log.warning("Serial read error: %s", exc)
                self._close()
                time.sleep(1.0)

    def send_and_wait(self, command: str, timeout: float = 3.0) -> str:
        if not self.connected:
            raise RuntimeError(f"ESP32 не подключён к {self.port}")

        with self.condition:
            start_seq = self.event_seq

        with self.write_lock:
            assert self.ser is not None
            log.info("ESP32 >> %s", command)
            self.ser.write((command.strip() + "\n").encode("utf-8"))
            self.ser.flush()

        deadline = time.monotonic() + timeout
        with self.condition:
            while self.event_seq == start_seq:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return "TIMEOUT"
                self.condition.wait(remaining)
            return self.last_event


bridge = SerialBridge(SERIAL_PORT, SERIAL_BAUD)


def allowed(update: Update) -> bool:
    return bool(update.effective_user and update.effective_user.id in ALLOWED_USER_IDS)


async def deny(update: Update) -> bool:
    if allowed(update):
        return False
    user_id = update.effective_user.id if update.effective_user else "?"
    text = (
        f"Доступ запрещён. Ваш Telegram user ID: {user_id}\n"
        "Добавьте его в ALLOWED_USER_IDS в bot/config.py."
    )
    if update.callback_query:
        await update.callback_query.answer("Нет доступа", show_alert=True)
    elif update.effective_message:
        await update.effective_message.reply_text(text)
    return True


async def serial_command(command: str) -> str:
    return await asyncio.to_thread(bridge.send_and_wait, command)


async def current_state() -> Dict[str, str]:
    try:
        result = await serial_command("STATUS")
        if result.startswith("STATE "):
            return parse_state_line(result)
    except Exception:
        pass
    return bridge.latest_state


async def cmd_id(update: Update, context: ContextTypes.DEFAULT_TYPE) -> None:
    del context
    user_id = update.effective_user.id if update.effective_user else "?"
    await update.effective_message.reply_text(f"Ваш Telegram user ID: {user_id}")


async def cmd_start(update: Update, context: ContextTypes.DEFAULT_TYPE) -> None:
    del context
    if await deny(update):
        return
    state = await current_state()
    await update.effective_message.reply_text(format_state(state), reply_markup=keyboard(state))


async def cmd_help(update: Update, context: ContextTypes.DEFAULT_TYPE) -> None:
    del context
    if await deny(update):
        return
    await update.effective_message.reply_text(
        "Можно пользоваться кнопками или писать команды.\n\n"
        "SET 23 COOL LOW\n"
        "SET 25 HEAT AUTO\n"
        "SET 23 COOL LOW SV=ON SH=OFF\n"
        "ON / OFF / STATUS\n\n"
        "Режимы: AUTO, COOL, DRY, HEAT, FAN\n"
        "Вентилятор: AUTO, LOW, MED, HIGH"
    )


CALLBACKS = {
    "POWER": "POWER TOGGLE",
    "STATUS": "STATUS",
    "TEMP_UP": "TEMP +1",
    "TEMP_DOWN": "TEMP -1",
    "MODE_AUTO": "MODE AUTO",
    "MODE_COOL": "MODE COOL",
    "MODE_DRY": "MODE DRY",
    "MODE_HEAT": "MODE HEAT",
    "MODE_FAN": "MODE FAN",
    "FAN_AUTO": "FAN AUTO",
    "FAN_LOW": "FAN LOW",
    "FAN_MED": "FAN MED",
    "FAN_HIGH": "FAN HIGH",
    "SWING_V": "SWINGV TOGGLE",
    "SWING_H": "SWINGH TOGGLE",
    "DISPLAY": "DISPLAY",
    "TURBO": "TURBO TOGGLE",
    "QUIET": "QUIET TOGGLE",
}


async def on_button(update: Update, context: ContextTypes.DEFAULT_TYPE) -> None:
    del context
    query = update.callback_query
    if not query or await deny(update):
        return
    await query.answer()

    command = CALLBACKS.get(query.data)
    if not command:
        return
    try:
        response = await serial_command(command)
    except Exception as exc:
        await query.message.reply_text(f"Нет связи с ESP32: {exc}")
        return

    if response.startswith("ERR "):
        await query.message.reply_text(response)
        return

    state = parse_state_line(response) or bridge.latest_state
    try:
        await query.edit_message_text(format_state(state), reply_markup=keyboard(state))
    except BadRequest as exc:
        if "not modified" not in str(exc).lower():
            raise


VALID_PREFIXES = (
    "SET ", "ON", "OFF", "STATUS", "POWER ", "TEMP ", "MODE ", "FAN ",
    "SWINGV ", "SWINGH ", "DISPLAY", "TURBO ", "QUIET ",
)


async def on_text(update: Update, context: ContextTypes.DEFAULT_TYPE) -> None:
    del context
    if await deny(update):
        return
    text = (update.effective_message.text or "").strip().upper()
    if not any(text == p.strip() or text.startswith(p) for p in VALID_PREFIXES):
        await update.effective_message.reply_text(
            "Не понял команду. Пример: SET 23 COOL LOW\n/help — справка."
        )
        return

    try:
        response = await serial_command(text)
    except Exception as exc:
        await update.effective_message.reply_text(f"Нет связи с ESP32: {exc}")
        return

    if response == "TIMEOUT":
        await update.effective_message.reply_text("ESP32 не ответил на команду.")
        return
    if response.startswith("ERR "):
        await update.effective_message.reply_text(response)
        return

    state = parse_state_line(response) or bridge.latest_state
    await update.effective_message.reply_text(format_state(state), reply_markup=keyboard(state))


def main() -> None:
    if not BOT_TOKEN or BOT_TOKEN.startswith("PASTE_"):
        raise RuntimeError("Укажите BOT_TOKEN в bot/config.py")

    bridge.start()
    app = ApplicationBuilder().token(BOT_TOKEN).build()
    app.add_handler(CommandHandler("id", cmd_id))
    app.add_handler(CommandHandler("start", cmd_start))
    app.add_handler(CommandHandler("help", cmd_help))
    app.add_handler(CallbackQueryHandler(on_button))
    app.add_handler(MessageHandler(filters.TEXT & ~filters.COMMAND, on_text))

    log.info("Telegram bot started")
    try:
        app.run_polling(allowed_updates=Update.ALL_TYPES)
    finally:
        bridge.stop()


if __name__ == "__main__":
    main()
