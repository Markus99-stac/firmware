# -*- mode: python ; coding: utf-8 -*-

a = Analysis(
    ['main.py'],
    pathex=[],
    binaries=[],
    datas=[
        ('firmware.ui', '.'),
        ('downloads_icon.icns', '.'),
        ('new_file.ico', '.'),
        ('run.ico', '.'),
        ('esptool-macos-arm64', 'esptool-macos-arm64'),
    ],
    hiddenimports=[
        'serial',
        'serial.tools',
        'serial.tools.list_ports',
    ],
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
    upx=False,            # UPX на macOS часто ломает бинарники — лучше выключить
    console=False,
    icon='downloads_icon.icns',
)

app = BUNDLE(
    exe,
    name='Firmware.app',
    icon='downloads_icon.icns',
    bundle_identifier='com.ams_spectre.firmware',
    version='1.0.0',
)