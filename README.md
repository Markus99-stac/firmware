# firmware - программа для прошивки скетчами ардуино 

фото интерфейса

# Подерживает платы:
## ESP32-S3 
## 

# Использование



# Инструкция для сборки

python3 -m venv .venv
source .venv/bin/activate
pip install pillow

python3 -c "from PIL import Image; Image.open('downloads_icon.ico').save('downloads_icon.icns')"


python3 -m pip install pillow

pip install pyserial

    pip install pyinstaller
    pyinstaller --noconfirm Firmware.spec

#


# Что было использовано 

https://github.com/espressif/esptool


