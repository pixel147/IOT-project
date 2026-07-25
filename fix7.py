text = open("main/voice/voice_xiaozhi.cpp", "r", encoding="utf-8").read()
# Find and replace the garbled Chinese text
import re
# Replace the say line - match the pattern
text = re.sub(r'ui_chat_voice_set_status\("Voice: say.*?"\)', 'ui_chat_voice_set_status("Voice: say xiaozhixiaozhi")', text)
open("main/voice/voice_xiaozhi.cpp", "w", encoding="utf-8").write(text)
print("Replaced")
