text = open("main/voice/voice_xiaozhi.cpp", "r", encoding="utf-8").read()
# Fix garbled Chinese text - replace various garbled versions of "say ????"
old_status = '''ui_chat_voice_set_status("Voice: say ??\xa0?????\x8f?\x99?")'''
new_status = '''ui_chat_voice_set_status("Voice: say xiaozhixiaozhi")'''
# Also fix the simpler garbled version
text = text.replace('say ??\xa0?????\x8f?\x99?', 'say xiaozhixiaozhi')
text = text.replace('say ??????', 'say xiaozhixiaozhi')
if old_status in text:
    text = text.replace(old_status, new_status, 1)
    open("main/voice/voice_xiaozhi.cpp", "w", encoding="utf-8").write(text)
    print("Fixed Chinese text!")
else:
    # Check what's there
    for line in text.split("\n"):
        if "say" in line and "Voice" in line:
            print(f"Found say line: {repr(line.strip())}")
