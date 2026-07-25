text = open("E:/esp_practice/first_practice/main/voice/voice_xiaozhi.cpp", "rb").read()
old = b'auto\\",\r\n                     \\"session_id'
new = b'auto\\","\r\n                     "\\"session_id'
if old in text:
    text = text.replace(old, new, 1)
    open("E:/esp_practice/first_practice/main/voice/voice_xiaozhi.cpp", "wb").write(text)
    print("Fixed!")
else:
    print("Pattern not found")
    idx = text.find(b"auto")
    while idx != -1:
        ctx = text[idx:idx+120]
        if b"session" in ctx:
            print(f"Context at {idx}: {ctx}")
        idx = text.find(b"auto", idx+1)
