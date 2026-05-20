# ESP32-CAM Face ID

ESP32-CAM orqali yuz tanish tizimi. Bitta server — ESP32 rasm oladi, serverga yuboradi, server yuzni aniqlaydi.

## Arxitektura

```
ESP32-CAM (rasm) → WiFi (HTTP) → Server (FastAPI + face_recognition) → Natija
```

## O'rnatish

```bash
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
python server.py
```

Server `http://0.0.0.0:8000` da ishga tushadi.

## ESP32-CAM

`esp32cam_faceid/esp32cam_faceid.ino` faylida:
- `WIFI_NOMI` → WiFi nomi
- `WIFI_PAROLI` → WiFi paroli
- `192.168.1.100` → Server IP manzili

Arduino IDE: Board → AI Thinker ESP32-CAM, Upload Speed → 115200

## API

| Endpoint | Method | Vazifasi |
|----------|--------|----------|
| `/register?name=X` | POST | Yuz ro'yxatga olish (file: jpg) |
| `/verify` | POST | Yuzni tekshirish (file: jpg) |
| `/faces` | GET | Ro'yxatdagilar |
| `/faces/{name}` | DELETE | O'chirish |

## Misollar

```bash
# Yuz qo'shish
curl -X POST "http://localhost:8000/register?name=Ali" -F "file=@ali.jpg"

# Tekshirish
curl -X POST "http://localhost:8000/verify" -F "file=@test.jpg"

# Ro'yxat
curl http://localhost:8000/faces
```
