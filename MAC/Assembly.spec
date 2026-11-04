# Firmware.spec (для macOS)
a = Analysis(
    ['main.py'],
    pathex=[],
    binaries=[],
    datas=[
        ('firmware.ui', '.'),
        ('downloads_icon.ico', '.'),
        ('new_file.ico', '.'),
        ('run.ico', '.'),
        ('esptool-macos-arm64', 'esptool-macos-arm64'), # Папка с Mac-бинарниками
    ],
    hiddenimports=['serial', 'serial.tools.list_ports'],
    hookspath=[],
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
)

pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='Firmware',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    console=False,
    icon='downloads_icon.ico', # PyInstaller конвертирует в .icns, если установлен Pillow
)

app = BUNDLE(
    exe,
    name='Firmware.app',
    icon='downloads_icon.ico',
    bundle_identifier='com.ams_spectre.firmware',
    version='1.0.0',
)