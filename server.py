import json
import numpy as np
import cv2
import face_recognition
from fastapi import FastAPI, UploadFile, File, HTTPException
from pathlib import Path

app = FastAPI(title="ESP32-CAM Face ID")

BASE_DIR = Path(__file__).parent
FACES_DIR = BASE_DIR / "faces"
FACES_DIR.mkdir(exist_ok=True)

THRESHOLD = 0.6


def load_registered_faces() -> dict:
    faces = {}
    for f in FACES_DIR.glob("*.json"):
        data = json.loads(f.read_text())
        faces[data["name"]] = np.array(data["embedding"])
    return faces


def decode_image(contents: bytes) -> np.ndarray | None:
    bgr = cv2.imdecode(np.frombuffer(contents, np.uint8), cv2.IMREAD_COLOR)
    if bgr is None:
        return None
    return cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)


def get_face_embedding(rgb: np.ndarray) -> np.ndarray | None:
    locations = face_recognition.face_locations(rgb)
    if not locations:
        return None
    encodings = face_recognition.face_encodings(rgb, locations)
    if not encodings:
        return None
    return encodings[0]


@app.post("/register")
async def register_face(name: str, file: UploadFile = File(...)):
    rgb = decode_image(await file.read())
    if rgb is None:
        raise HTTPException(400, "Rasm o'qib bo'lmadi")

    embedding = get_face_embedding(rgb)
    if embedding is None:
        raise HTTPException(400, "Yuz topilmadi")

    face_data = {"name": name, "embedding": embedding.tolist()}
    (FACES_DIR / f"{name}.json").write_text(json.dumps(face_data))
    return {"status": "ok", "message": f"'{name}' ro'yxatga olindi"}


@app.post("/verify")
async def verify_face(file: UploadFile = File(...)):
    rgb = decode_image(await file.read())
    if rgb is None:
        raise HTTPException(400, "Rasm o'qib bo'lmadi")

    embedding = get_face_embedding(rgb)
    if embedding is None:
        return {"status": "no_face", "message": "Yuz topilmadi"}

    registered = load_registered_faces()
    if not registered:
        return {"status": "empty", "message": "Hech kim ro'yxatga olinmagan"}

    best_match = None
    best_distance = 999.0

    for name, saved_emb in registered.items():
        distance = float(np.linalg.norm(embedding - saved_emb))
        if distance < best_distance:
            best_distance = distance
            best_match = name

    confidence = round(1.0 - best_distance, 3)

    if best_distance <= THRESHOLD:
        return {"status": "ok", "name": best_match, "confidence": confidence}
    return {"status": "unknown", "message": "Tanilmadi", "confidence": confidence}


@app.get("/faces")
def list_faces():
    names = [f.stem for f in FACES_DIR.glob("*.json")]
    return {"faces": names, "count": len(names)}


@app.delete("/faces/{name}")
def delete_face(name: str):
    path = FACES_DIR / f"{name}.json"
    if not path.exists():
        raise HTTPException(404, f"'{name}' topilmadi")
    path.unlink()
    return {"status": "ok", "message": f"'{name}' o'chirildi"}


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8000)
