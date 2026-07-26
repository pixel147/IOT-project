"""Monitor serial for 180 seconds - filter key events for voice test."""
import sys, time, threading
sys.path.insert(0, r'E:\esp_idf\Espressif\python_env\idf5.5_py3.11_env\Scripts')
try:
    import serial
except ImportError:
    import subprocess
    subprocess.run([r'E:\esp_idf\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe',
                    '-m', 'pip', 'install', 'pyserial'], capture_output=True)
    import serial

MONITOR_SECONDS = 180
OUTPUT_FILE = r'E:\esp_practice\first_practice\monitor_test.txt'
log_lines = []
stop = threading.Event()

def reader(ser):
    buf = b''
    while not stop.is_set():
        try:
            if ser.in_waiting:
                data = ser.read(ser.in_waiting)
                buf += data
                while b'\n' in buf:
                    line, buf = buf.split(b'\n', 1)
                    try:
                        decoded = line.decode('utf-8', errors='replace').strip('\r')
                    except:
                        decoded = str(line)
                    log_lines.append(decoded)
                    u = decoded.upper()
                    if any(kw in u for kw in ['XIAOZHI','WS_','MCP','HELLO','SESSION','STT','TTS',
                                               'WAKE','STATE=','LISTEN','TOOLS']):
                        print(decoded)
        except:
            break

ser = serial.Serial('COM7', 115200, timeout=0.1)
print(f'Connected to COM7 at 115200')
ser.dtr=False;time.sleep(0.2);ser.dtr=True;time.sleep(0.5)
ser.setRTS(True);time.sleep(0.1);ser.setRTS(False)
t = threading.Thread(target=reader, args=(ser,), daemon=True)
t.start()
time.sleep(MONITOR_SECONDS)
stop.set(); t.join(timeout=2); ser.close()
with open(OUTPUT_FILE,'w',encoding='utf-8') as f:
    f.write('\n'.join(log_lines))
print(f'\nSaved {len(log_lines)} lines to {OUTPUT_FILE}')
