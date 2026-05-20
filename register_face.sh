#!/bin/bash
# Yuz ro'yxatga olish
# Ishlatish: ./register_face.sh Bekmurod rasm.jpg

SERVER="http://localhost:8000"

if [ $# -lt 2 ]; then
  echo "Ishlatish: $0 <ism> <rasm.jpg>"
  echo "Misol:    $0 Bekmurod foto.jpg"
  exit 1
fi

NAME=$1
PHOTO=$2

if [ ! -f "$PHOTO" ]; then
  echo "Xato: '$PHOTO' fayl topilmadi"
  exit 1
fi

echo "Ro'yxatga olish: $NAME ← $PHOTO"
curl -s -X POST "$SERVER/register?name=$NAME" -F "file=@$PHOTO" | python3 -m json.tool

echo ""
echo "Ro'yxatdagilar:"
curl -s "$SERVER/faces" | python3 -m json.tool
