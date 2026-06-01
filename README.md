# ESP32-CAM Face ID

ESP32-CAM orqali yuz tanish tizimi. ESP32 rasmni serverga yuboradi, server yuzni aniqlaydi va natijani qaytaradi.

## Arxitektura

```text
ESP32-CAM -> WiFi/HTTP(S) -> FastAPI server -> Face detection/recognition -> Natija
```

## Server

```bash
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
python server.py
```

`server.py` default holatda `http://0.0.0.0:8050` da ishga tushadi. Agar tashqi runner yoki reverse proxy ishlatilsa, ESP32 setup sahifasidagi server URL shu manzilga mos bo'lishi kerak.

## ESP32-CAM

Asosiy firmware: `firmware/src/main.cpp`.

ESP32-CAM WiFi ga ulana olmasa setup rejimiga o'tadi:

- WiFi: `ESP32-CAM-Setup`
- Parol: `12345678`
- Browser: `http://192.168.4.1`

Setup sahifasida WiFi nomi, WiFi paroli va server `/verify` URL manzili kiritiladi. Saqlangandan keyin ESP32 qayta ishga tushadi va online rejimda serverga heartbeat, verify natijalari va stream URL yuboradi.

PlatformIO:

```bash
cd firmware
pio run
pio run --target upload
pio device monitor
```

## API

| Endpoint | Method | Vazifasi |
|----------|--------|----------|
| `/register?name=X` | POST | Yuz ro'yxatga olish (`files`: jpg) |
| `/register/add?name=X` | POST | Mavjud odamga yangi rasmlar qo'shish |
| `/verify` | POST | Yuzni tekshirish (`file`: jpg) |
| `/faces` | GET | Ro'yxatdagilar |
| `/faces/{name}` | DELETE | O'chirish |
| `/esp32/heartbeat` | POST | ESP32 online statusini yangilash |
| `/esp32/status` | GET | ESP32 holati |
| `/esp32/stream` | GET | ESP32 stream URL |

## Misollar

```bash
# Yuz qo'shish
curl -X POST "http://localhost:8050/register?name=Ali" -F "files=@ali.jpg"

# Tekshirish
curl -X POST "http://localhost:8050/verify" -F "file=@test.jpg"

# Ro'yxat
curl http://localhost:8050/faces
```
