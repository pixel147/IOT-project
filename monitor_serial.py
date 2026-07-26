"""Read ESP32-P4 serial monitor output for a few seconds and save to file."""
import sys
import time
import threading
import os

# Add esp_idf Python env
sys.path.insert(0, r'E:\esp_idf\Espressif\python_env\idf5.5_py3.11_env\Scripts')

try:
    import serial
except ImportError:
    # Try to find pyserial in IDF
    idf_path = r'E:\esp_idf\Espressif\frameworks\esp-idf-v5.5.4'
    sys.path.insert(0, os.path.join(idf_path, 'tools'))
    # Install pyserial if needed
    import subprocess
    subprocess.run([
        r'E:\esp_idf\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe',
        '-m', 'pip', 'install', 'pyserial'
    ], capture_output=True)
    import serial

MONITOR_SECONDS = 30
SERIAL_PORT = 'COM7'
BAUD = 115200
OUTPUT_FILE = r'E:\esp_practice\first_practice\monitor_log.txt'

log_lines = []
stop_event = threading.Event()

def serial_reader(ser):
    buf = b''
    while not stop_event.is_set():
        try:
            if ser.in_waiting:
                data = ser.read(ser.in_waiting)
                buf += data
                # Process lines
                while b'\n' in buf:
                    line, buf = buf.split(b'\n', 1)
                    try:
                        decoded = line.decode('utf-8', errors='replace').strip('\r')
                    except:
                        decoded = str(line)
                    log_lines.append(decoded)
                    print(decoded)
        except Exception as e:
            log_lines.append(f'[Serial error] {e}')
            break

try:
    ser = serial.Serial(SERIAL_PORT, BAUD, timeout=0.1)
    print(f'Connected to {SERIAL_PORT} at {BAUD} baud')

    # DTR reset
    ser.dtr = False
    time.sleep(0.2)
    ser.dtr = True
    time.sleep(0.5)

    # Reset
    ser.setRTS(True)
    time.sleep(0.1)
    ser.setRTS(False)
    time.sleep(0.1)

    reader = threading.Thread(target=serial_reader, args=(ser,), daemon=True)
    reader.start()

    time.sleep(MONITOR_SECONDS)
    stop_event.set()
    reader.join(timeout=2)
    ser.close()

    with open(OUTPUT_FILE, 'w', encoding='utf-8') as f:
        f.write('\n'.join(log_lines))
    print(f'\nSaved {len(log_lines)} lines to {OUTPUT_FILE}')

except Exception as e:
    print(f'Error: {e}')
    # Create the file anyway with what we have
    with open(OUTPUT_FILE, 'w', encoding='utf-8') as f:
        f.write('\n'.join(log_lines))
    print(f'Saved {len(log_lines)} lines to {OUTPUT_FILE}')
