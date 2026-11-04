from PyQt5 import QtWidgets, uic
from PyQt5.QtCore import QObject, pyqtSignal
import sys
import os
import subprocess
import threading
import serial.tools.list_ports

# ---------- Кросс-платформенные пути ----------
if getattr(sys, 'frozen', False):
    # Собранное приложение (PyInstaller) — ресурсы в _MEIPASS
    BASE_DIR = sys._MEIPASS
else:
    # Режим разработки
    BASE_DIR = os.path.dirname(os.path.abspath(__file__))

# Выбор папки и имени esptool в зависимости от ОС
if sys.platform == "darwin":
    ESPTOOL = os.path.join(BASE_DIR, "esptool-macos-arm64", "esptool")
elif sys.platform == "win32":
    ESPTOOL = os.path.join(BASE_DIR, "esptool-windows-amd64", "esptool.exe")
else:
    ESPTOOL = os.path.join(BASE_DIR, "esptool", "esptool")

APP_NAME = "Firmware Flasher"
APP_VERSION = "1.0.0"
APP_AUTHOR = "AMS_Spectre"
APP_DESCRIPTION = (
    "Утилита для прошивки ESP32 / ESP32-S3 / ESP32-C3 / ESP32-S2 "
    "через esptool без Arduino IDE.\n\n"
    "Прошивка собирается из папки BIN: bootloader (0x0), partitions (0x8000), "
    "boot_app0 (0xe000), app (0x10000). Если в папке есть *.merged.bin — "
    "он шьётся по адресу 0x0."
)

app = QtWidgets.QApplication([])
win = uic.loadUi(os.path.join(BASE_DIR, "firmware.ui"))


# ---------- Мост для лога из фонового потока ----------
class LogBridge(QObject):
    message = pyqtSignal(str)


bridge = LogBridge()
bridge.message.connect(lambda s: (win.textEdit.append(s), win.textEdit.ensureCursorVisible()))


def log(text: str):
    bridge.message.emit(text)


def refresh_ports():
    win.comboBox.clear()
    for p in serial.tools.list_ports.comports():
        win.comboBox.addItem(f"{p.device} - {p.description}")


# ---------- Поиск файлов ----------
def find_in_folder(folder, suffix):
    for n in os.listdir(folder):
        if n.lower().endswith(suffix.lower()):
            return os.path.join(folder, n)
    return None


def find_app_bin(folder):
    for n in os.listdir(folder):
        low = n.lower()
        if low.endswith(".bin") and "bootloader" not in low \
           and "partitions" not in low and "merged" not in low:
            return os.path.join(folder, n)
    return None


def find_boot_app0(folder=None):
    if folder:
        p = os.path.join(folder, "boot_app0.bin")
        if os.path.exists(p):
            return p

    candidates = [
        # Windows — портативная установка рядом с программой
        os.path.join(BASE_DIR, "esp32", "hardware", "esp32", "3.3.7",
                     "tools", "partitions", "boot_app0.bin"),
        # Windows — Arduino15
        os.path.join(os.environ.get("LOCALAPPDATA", ""), "Arduino15", "packages",
                     "esp32", "hardware", "esp32", "3.3.7",
                     "tools", "partitions", "boot_app0.bin"),
        # macOS — Arduino15
        os.path.expanduser("~/Library/Arduino15/packages/esp32/hardware/esp32/3.3.7/"
                           "tools/partitions/boot_app0.bin"),
    ]
    for p in candidates:
        if p and os.path.exists(p):
            return p

    # fallback: любая версия ядра
    bases = [
        os.path.join(os.environ.get("LOCALAPPDATA", ""), "Arduino15",
                     "packages", "esp32", "hardware", "esp32"),
        os.path.expanduser("~/Library/Arduino15/packages/esp32/hardware/esp32"),
    ]
    for base in bases:
        if os.path.isdir(base):
            for ver in os.listdir(base):
                p = os.path.join(base, ver, "tools", "partitions", "boot_app0.bin")
                if os.path.exists(p):
                    return p
    return None


# ---------- Сборка команды прошивки из папки ----------
def build_write_cmd_from_folder(folder: str):
    bootloader = find_in_folder(folder, ".bootloader.bin")
    partitions = find_in_folder(folder, ".partitions.bin")
    merged = find_in_folder(folder, ".merged.bin")
    app_bin = find_app_bin(folder)

    if merged:
        log(f"Использую merged-образ: {merged}")
        return ["--flash-mode", "keep", "--flash-freq", "keep",
                "--flash-size", "keep", "0x0", merged]

    if not bootloader or not partitions or not app_bin:
        log("[ОШИБКА] В папке не хватает файлов: "
            f"bootloader={bool(bootloader)}, partitions={bool(partitions)}, app={bool(app_bin)}")
        return None

    boot_app0 = find_boot_app0(folder)
    if not boot_app0:
        log("[ОШИБКА] Не найден boot_app0.bin. Скопируйте его в папку BIN "
            "или проверьте установку ESP32-ядра.")
        return None

    log("Комплект файлов:")
    log(f"  0x0     {bootloader}")
    log(f"  0x8000  {partitions}")
    log(f"  0xe000  {boot_app0}")
    log(f"  0x10000 {app_bin}")

    return [
        "--flash-mode", "keep", "--flash-freq", "keep", "--flash-size", "keep",
        "0x0", bootloader,
        "0x8000", partitions,
        "0xe000", boot_app0,
        "0x10000", app_bin,
    ]


# ---------- Запуск esptool ----------
def run_command(cmd: list, description: str):
    log(f"\n=== {description} ===")
    log(" ".join(cmd))

    if not os.path.exists(ESPTOOL):
        log(f"[ОШИБКА] Не найден esptool: {ESPTOOL}")
        return

    try:
        process = subprocess.Popen(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, encoding="utf-8", errors="replace", bufsize=1,
        )
        for line in process.stdout:
            log(line.rstrip())
        process.wait()
        if process.returncode == 0:
            log(f"[OK] {description} завершено")
        else:
            log(f"[FAIL] {description}, код {process.returncode}")
    except Exception as e:
        log(f"[ИСКЛЮЧЕНИЕ] {e}")


# ---------- Обработчики ----------
def on_open_port():
    refresh_ports()
    log("Список портов обновлён")


def on_close_port():
    win.comboBox.clear()
    log("Порт не выбран")


def on_browse_folder():
    folder = QtWidgets.QFileDialog.getExistingDirectory(win, "Выберите папку BIN")
    if folder:
        win.lineEdit.setText(folder)
        log(f"Выбрана папка: {folder}")


def on_execute():
    port_text = win.comboBox.currentText()
    if not port_text:
        log("[ОШИБКА] Выберите порт")
        return
    port = port_text.split(" ")[0]

    chip = win.comboBox_2.currentData() or "esp32s3"
    command = win.comboBox_3.currentText().lower()
    target = win.lineEdit.text().strip()

    if command == "прошивка":
        if not target:
            log("[ОШИБКА] Укажите папку BIN")
            return
        if not os.path.isdir(target):
            log("[ОШИБКА] Ожидается папка BIN, а не файл. Нажмите «Открыть» и выберите папку.")
            return

        tail = build_write_cmd_from_folder(target)
        if not tail:
            return

        cmd = [ESPTOOL, "--chip", chip, "--port", port, "--baud", "921600",
               "--before", "default-reset", "--after", "hard-reset",
               "write-flash", "-z"] + tail

    elif command == "чтение флеш-памяти":
        if not target:
            log("[ОШИБКА] Укажите папку, куда сохранить дамп")
            return
        out_path = os.path.join(target, "flash_dump.bin")
        cmd = [ESPTOOL, "--chip", chip, "--port", port,
               "read-flash", "0x0", "0x1000000", out_path]

    elif command == "стирание флеш-памяти":
        cmd = [ESPTOOL, "--chip", chip, "--port", port, "erase-flash"]

    elif command == "просмотр информации о чипе":
        cmd = [ESPTOOL, "--chip", chip, "--port", port, "flash-id"]

    else:
        log(f"[ОШИБКА] Неизвестная команда: {command}")
        return

    threading.Thread(target=run_command, args=(cmd, command), daemon=True).start()


def on_save_log():
    path, _ = QtWidgets.QFileDialog.getSaveFileName(win, "Сохранить лог", "log.txt", "Text (*.txt)")
    if path:
        with open(path, "w", encoding="utf-8") as f:
            f.write(win.textEdit.toPlainText())
        log(f"Лог сохранён: {path}")


def on_about():
    QtWidgets.QMessageBox.about(
        win,
        f"О программе — {APP_NAME}",
        f"<h3>{APP_NAME}</h3>"
        f"<p><b>Версия:</b> {APP_VERSION}</p>"
        f"<p><b>Автор:</b> {APP_AUTHOR}</p>"
        f"<p>{APP_DESCRIPTION.replace(chr(10), '<br>')}</p>"
        f"<hr>"
        f"<p><b>Использует:</b> esptool v5.6.0<br>"
        f"<b>Qt:</b> PyQt5<br>"
        f"<b>Python:</b> {sys.version.split()[0]}</p>",
    )


# ---------- Привязка ----------
win.pushButton_2.clicked.connect(on_open_port)
win.pushButton.clicked.connect(on_close_port)
win.pushButton_3.clicked.connect(on_browse_folder)
win.pushButton_6.clicked.connect(on_execute)
win.pushButton_4.clicked.connect(on_save_log)

if hasattr(win, "menu"):
    about_action = win.menu.addAction("О программе")
    about_action.triggered.connect(on_about)

refresh_ports()
win.comboBox_2.clear()
win.comboBox_2.addItem("ESP32-S3", "esp32s3")
win.comboBox_2.addItem("ESP32", "esp32")
win.comboBox_2.addItem("ESP32-C3", "esp32c3")
win.comboBox_2.addItem("ESP32-S2", "esp32s2")

win.comboBox_3.clear()
win.comboBox_3.addItems([
    "Прошивка", "Чтение флеш-памяти",
    "Стирание флеш-памяти", "Просмотр информации о чипе",
])

win.show()
sys.exit(app.exec())