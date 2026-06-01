import json
import time
import numpy as np
import cv2
from fastapi import FastAPI, UploadFile, File, HTTPException, Query, Request
from fastapi.staticfiles import StaticFiles
from fastapi.responses import FileResponse
from fastapi.middleware.cors import CORSMiddleware
from pathlib import Path
from typing import List
from datetime import datetime
import urllib.request
import threading

app = FastAPI(title="ESP32-CAM Face ID Pro")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

BASE_DIR = Path(__file__).parent
FACES_DIR = BASE_DIR / "faces"
FACES_DIR.mkdir(exist_ok=True)
MODELS_DIR = BASE_DIR / "models"
MODELS_DIR.mkdir(exist_ok=True)
STATIC_DIR = BASE_DIR / "static"
STATIC_DIR.mkdir(exist_ok=True)
LOGS_DIR = BASE_DIR / "logs"
LOGS_DIR.mkdir(exist_ok=True)

COSINE_THRESHOLD = 0.363
ESP32_OFFLINE_AFTER_SECONDS = 45
INVALID_FACE_NAME_CHARS = set('<>:"/\\|?*')

FACE_DETECT_URL = "https://github.com/opencv/opencv_zoo/raw/main/models/face_detection_yunet/face_detection_yunet_2023mar.onnx"
FACE_RECOG_URL = "https://github.com/opencv/opencv_zoo/raw/main/models/face_recognition_sface/face_recognition_sface_2021dec.onnx"

face_detector = None
face_recognizer = None

# Real-time state
access_log: list = []
stats = {"total_verifications": 0, "total_recognized": 0, "total_unknown": 0, "total_no_face": 0}
esp32_status = {
    "last_seen": None,
    "ip": None,
    "stream_url": None,
    "rssi": None,
    "heap": None,
    "last_result": None,
    "online": False,
}
LOG_MAX = 200


def download_model(url: str, path: Path):
    if not path.exists():
        print(f"Model yuklanmoqda: {path.name}...")
        urllib.request.urlretrieve(url, str(path))
        print(f"Tayyor: {path.name}")


def init_models():
    global face_detector, face_recognizer
    detect_path = MODELS_DIR / "face_detection_yunet_2023mar.onnx"
    recog_path = MODELS_DIR / "face_recognition_sface_2021dec.onnx"
    download_model(FACE_DETECT_URL, detect_path)
    download_model(FACE_RECOG_URL, recog_path)
    face_detector = cv2.FaceDetectorYN.create(str(detect_path), "", (320, 240), 0.6, 0.3, 5000)
    face_recognizer = cv2.FaceRecognizerSF.create(str(recog_path), "")


def get_face_embedding(bgr: np.ndarray) -> np.ndarray | None:
    h, w = bgr.shape[:2]
    face_detector.setInputSize((w, h))
    _, faces = face_detector.detect(bgr)
    if faces is None or len(faces) == 0:
        return None
    aligned = face_recognizer.alignCrop(bgr, faces[0])
    return face_recognizer.feature(aligned).flatten()


def clean_face_name(name: str) -> str:
    cleaned = name.strip()
    if (
        not cleaned
        or cleaned in {".", ".."}
        or any(ch in INVALID_FACE_NAME_CHARS or ord(ch) < 32 for ch in cleaned)
    ):
        raise HTTPException(400, "Ismda fayl nomi uchun yaroqsiz belgi bor")
    return cleaned


def face_data_path(name: str) -> Path:
    cleaned = clean_face_name(name)
    path = (FACES_DIR / f"{cleaned}.json").resolve()
    if FACES_DIR.resolve() not in path.parents:
        raise HTTPException(400, "Noto'g'ri ism")
    return path


def load_face_data(name: str) -> dict | None:
    path = face_data_path(name)
    if not path.exists():
        return None
    return json.loads(path.read_text())


def load_all_faces() -> dict:
    faces = {}
    for f in FACES_DIR.glob("*.json"):
        data = json.loads(f.read_text())
        faces[data["name"]] = np.array(data["embedding"])
    return faces


def cosine_score(a: np.ndarray, b: np.ndarray) -> float:
    return float(np.dot(a, b) / (np.linalg.norm(a) * np.linalg.norm(b)))


def add_log_entry(action: str, name: str | None, confidence: float | None, source: str):
    entry = {
        "time": datetime.now().isoformat(),
        "action": action,
        "name": name,
        "confidence": confidence,
        "source": source,
    }
    access_log.insert(0, entry)
    if len(access_log) > LOG_MAX:
        access_log.pop()


def update_esp32_status(request: Request, payload: dict | None = None, result: dict | None = None):
    payload = payload or {}
    ip = payload.get("ip") or (request.client.host if request.client else None)
    esp32_status["last_seen"] = datetime.now().isoformat()
    esp32_status["ip"] = ip
    esp32_status["online"] = True
    if ip:
        esp32_status["stream_url"] = payload.get("stream_url") or f"http://{ip}/stream"
    if "rssi" in payload:
        esp32_status["rssi"] = payload.get("rssi")
    if "heap" in payload:
        esp32_status["heap"] = payload.get("heap")
    if result is not None:
        esp32_status["last_result"] = result


def refresh_esp32_online_state():
    if not esp32_status["last_seen"]:
        esp32_status["online"] = False
        return
    last = datetime.fromisoformat(esp32_status["last_seen"])
    diff = (datetime.now() - last).total_seconds()
    esp32_status["online"] = diff < ESP32_OFFLINE_AFTER_SECONDS


@app.on_event("startup")
def startup():
    init_models()
    # Load saved logs
    log_file = LOGS_DIR / "access.json"
    if log_file.exists():
        try:
            saved = json.loads(log_file.read_text())
            access_log.extend(saved[:LOG_MAX])
        except Exception:
            pass


@app.on_event("shutdown")
def shutdown():
    log_file = LOGS_DIR / "access.json"
    log_file.write_text(json.dumps(access_log[:LOG_MAX]))


# ─── PAGES ───────────────────────────────────────────────────────────────────

@app.get("/")
def index():
    return FileResponse(STATIC_DIR / "index.html")


# ─── REGISTER (multi-image) ─────────────────────────────────────────────────

@app.post("/register")
async def register_face(name: str = Query(...), files: List[UploadFile] = File(...)):
    """Bitta odam uchun 1 yoki ko'p rasm bilan ro'yxatga olish. Ko'p rasm = aniqroq."""
    name = clean_face_name(name)
    embeddings = []

    for file in files:
        contents = await file.read()
        bgr = cv2.imdecode(np.frombuffer(contents, np.uint8), cv2.IMREAD_COLOR)
        if bgr is None:
            continue
        emb = get_face_embedding(bgr)
        if emb is not None:
            embeddings.append(emb)

    if not embeddings:
        raise HTTPException(400, "Hech bir rasmda yuz topilmadi")

    # O'rtacha embedding — ko'p rasm = aniqroq
    avg_embedding = np.mean(embeddings, axis=0)
    # Normalize
    avg_embedding = avg_embedding / np.linalg.norm(avg_embedding)

    face_data = {
        "name": name,
        "embedding": avg_embedding.tolist(),
        "samples": len(embeddings),
        "created": datetime.now().isoformat(),
    }
    face_data_path(name).write_text(json.dumps(face_data))
    add_log_entry("register", name, None, "web")
    return {
        "status": "ok",
        "message": f"'{name}' ro'yxatga olindi ({len(embeddings)}/{len(files)} rasm ishlatildi)",
        "samples": len(embeddings),
    }


@app.post("/register/add")
async def add_samples(name: str = Query(...), files: List[UploadFile] = File(...)):
    """Mavjud odamga qo'shimcha rasmlar qo'shish — aniqlikni oshiradi."""
    name = clean_face_name(name)
    existing = load_face_data(name)
    if existing is None:
        raise HTTPException(404, f"'{name}' topilmadi. Avval /register dan foydalaning.")

    old_embedding = np.array(existing["embedding"])
    old_samples = existing.get("samples", 1)

    new_embeddings = []
    for file in files:
        contents = await file.read()
        bgr = cv2.imdecode(np.frombuffer(contents, np.uint8), cv2.IMREAD_COLOR)
        if bgr is None:
            continue
        emb = get_face_embedding(bgr)
        if emb is not None:
            new_embeddings.append(emb)

    if not new_embeddings:
        raise HTTPException(400, "Hech bir rasmda yuz topilmadi")

    # Weighted average: eski embedding * eski sample soni + yangilar
    total_samples = old_samples + len(new_embeddings)
    combined = old_embedding * old_samples
    for emb in new_embeddings:
        combined = combined + emb
    avg_embedding = combined / total_samples
    avg_embedding = avg_embedding / np.linalg.norm(avg_embedding)

    existing["embedding"] = avg_embedding.tolist()
    existing["samples"] = total_samples
    existing["updated"] = datetime.now().isoformat()
    face_data_path(name).write_text(json.dumps(existing))

    add_log_entry("add_samples", name, None, "web")
    return {
        "status": "ok",
        "message": f"'{name}' ga {len(new_embeddings)} ta yangi rasm qo'shildi (jami: {total_samples})",
        "total_samples": total_samples,
    }


# ─── VERIFY ─────────────────────────────────────────────────────────────────

@app.post("/verify")
async def verify_face(request: Request, file: UploadFile = File(...), source: str = Query("esp32")):
    contents = await file.read()
    bgr = cv2.imdecode(np.frombuffer(contents, np.uint8), cv2.IMREAD_COLOR)
    if bgr is None:
        raise HTTPException(400, "Rasm o'qib bo'lmadi")

    # Update ESP32 status
    if source == "esp32":
        update_esp32_status(request)

    embedding = get_face_embedding(bgr)
    stats["total_verifications"] += 1

    if embedding is None:
        stats["total_no_face"] += 1
        result = {"status": "no_face", "message": "Yuz topilmadi"}
        if source == "esp32":
            update_esp32_status(request, result=result)
        add_log_entry("verify_no_face", None, None, source)
        return result

    registered = load_all_faces()
    if not registered:
        return {"status": "empty", "message": "Hech kim ro'yxatga olinmagan"}

    best_match = None
    best_score = -1.0

    for name, saved_emb in registered.items():
        score = cosine_score(embedding, saved_emb)
        if score > best_score:
            best_score = score
            best_match = name

    confidence = round(best_score, 4)

    if best_score >= COSINE_THRESHOLD:
        stats["total_recognized"] += 1
        result = {"status": "ok", "name": best_match, "confidence": confidence}
        add_log_entry("recognized", best_match, confidence, source)
    else:
        stats["total_unknown"] += 1
        result = {"status": "unknown", "message": "Tanilmadi", "confidence": confidence}
        add_log_entry("unknown", None, confidence, source)

    if source == "esp32":
        update_esp32_status(request, result=result)
    return result


@app.post("/esp32/heartbeat")
async def esp32_heartbeat(request: Request):
    try:
        payload = await request.json()
        if not isinstance(payload, dict):
            payload = {}
    except Exception:
        payload = {}
    update_esp32_status(request, payload=payload)
    return {"status": "ok", "online": True}


# ─── FACES CRUD ──────────────────────────────────────────────────────────────

@app.get("/faces")
def list_faces():
    faces = []
    for f in FACES_DIR.glob("*.json"):
        data = json.loads(f.read_text())
        faces.append({
            "name": data["name"],
            "samples": data.get("samples", 1),
            "created": data.get("created", ""),
            "updated": data.get("updated", ""),
        })
    faces.sort(key=lambda x: x["name"])
    return {"faces": faces, "count": len(faces)}


@app.put("/faces/{name}")
async def rename_face(name: str, new_name: str = Query(...)):
    name = clean_face_name(name)
    new_name = clean_face_name(new_name)
    path = face_data_path(name)
    if not path.exists():
        raise HTTPException(404, f"'{name}' topilmadi")
    data = json.loads(path.read_text())
    data["name"] = new_name
    data["updated"] = datetime.now().isoformat()
    new_path = face_data_path(new_name)
    new_path.write_text(json.dumps(data))
    path.unlink()
    return {"status": "ok", "message": f"'{name}' → '{new_name}'"}


@app.delete("/faces/{name}")
def delete_face(name: str):
    name = clean_face_name(name)
    path = face_data_path(name)
    if not path.exists():
        raise HTTPException(404, f"'{name}' topilmadi")
    path.unlink()
    add_log_entry("delete", name, None, "web")
    return {"status": "ok", "message": f"'{name}' o'chirildi"}


# ─── MONITORING & STATS ─────────────────────────────────────────────────────

@app.get("/stats")
def get_stats():
    return {
        **stats,
        "registered_faces": len(list(FACES_DIR.glob("*.json"))),
        "recognition_rate": round(stats["total_recognized"] / max(stats["total_verifications"], 1) * 100, 1),
    }


@app.get("/logs")
def get_logs(limit: int = Query(50)):
    return {"logs": access_log[:limit]}


@app.get("/esp32/status")
def get_esp32_status():
    refresh_esp32_online_state()
    return esp32_status


@app.get("/esp32/stream")
def get_esp32_stream_url():
    """ESP32 stream URL ni qaytaradi (frontend to'g'ridan-to'g'ri ulanadi)"""
    refresh_esp32_online_state()
    if esp32_status.get("online") and esp32_status.get("stream_url"):
        return {"url": esp32_status["stream_url"]}
    return {"url": None}


app.mount("/static", StaticFiles(directory=str(STATIC_DIR)), name="static")

if __name__ == "__main__":
    import uvicorn
    print("Server: http://0.0.0.0:8050")
    uvicorn.run(app, host="0.0.0.0", port=8050)
