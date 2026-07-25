import sys
with open("main/voice/voice_xiaozhi.cpp", "r", encoding="utf-8") as f:
    content = f.read()
idx = content.find('"mode":"auto"')
print("Found at:", idx)
if idx > 0:
    print(repr(content[idx-30:idx+150]))
