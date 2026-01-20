import serial
import time
import cv2
from ultralytics import YOLO
import threading
import queue
import os

# --- Configuration ---
# Update this path if your device changes (e.g. /dev/ttyUSB1)
SERIAL_PORT = '/dev/ttyUSB0'
BAUD_RATE = 115200
CONFIDENCE_THRESHOLD = 0.5
TARGET_CLASS = 'bottle'
MODEL_NAME = "yolo11n.pt"

# --- Threaded Camera Class ---
# Optimized for lowest latency in Linux environments
class ThreadedCamera:
    def __init__(self, src=0, width=320, height=320):
        self.src = src
        self.width = width
        self.height = height
        self.cap = cv2.VideoCapture(self.src)

        # Optimize Camera Buffer
        self.cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)
        self.cap.set(cv2.CAP_PROP_FRAME_WIDTH, self.width)
        self.cap.set(cv2.CAP_PROP_FRAME_HEIGHT, self.height)

        self.grabbed, self.frame = self.cap.read()
        self.started = False
        self.read_lock = threading.Lock()

    def start(self):
        if self.started:
            print("Camera already running.")
            return self
        self.started = True
        self.thread = threading.Thread(target=self.update, args=())
        self.thread.daemon = True
        self.thread.start()
        return self

    def update(self):
        while self.started:
            grabbed, frame = self.cap.read()
            with self.read_lock:
                self.grabbed = grabbed
                self.frame = frame
            # No sleep here - grab as fast as hardware allows to clear buffer

    def read(self):
        with self.read_lock:
            if not self.grabbed:
                return None
            return self.frame.copy()

    def stop(self):
        self.started = False
        if self.cap.isOpened():
            self.cap.release()

class BottleDetector:
    def __init__(self):
        self.ser = None
        self.running = True
        self.camera_active = False
        self.show_camera = False # Set True to see window on monitor
        self.cam_thread = None
        self.last_bottle_time = 0

        # 1. Initialize Serial
        print(f"Connecting to {SERIAL_PORT} at {BAUD_RATE} baud...")
        while self.ser is None:
            try:
                # write_timeout prevents Python from hanging if Arduino is busy
                self.ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=0.1, write_timeout=0.1)
                time.sleep(2) # Wait for Arduino reset
            except serial.SerialException:
                print("Waiting for serial device...")
                time.sleep(1)

        # 2. Initialize Model with CPU Optimizations
        print("Loading Model...")
        try:
            base_model = YOLO(MODEL_NAME)
            print("Attempting to load/export OpenVINO model for CPU acceleration...")
            # This creates a folder named yolo11n_openvino_model
            # INT8 quantization provides massive speedup on CPU
            export_path = base_model.export(format="openvino", half=True, int8=True)
            self.model = YOLO(export_path, task="detect")
            print("Running optimized OpenVINO model.")
        except Exception as e:
            print(f"Optimization warning: {e}. Falling back to standard PyTorch.")
            self.model = base_model

        print("System Ready. Sending Handshake.")
        self.ser.write(b"HANDSHAKE\n")

    def start_camera(self):
        if not self.camera_active:
            # Re-initialize camera thread
            print("Powering ON Camera & Detection...")
            self.cam_thread = ThreadedCamera(src=0, width=320, height=320).start()
            self.camera_active = True
            print("Camera Thread Started.")

    def stop_camera(self):
        if self.camera_active:
            print("Powering OFF Camera & Detection (Saving Power)...")
            if self.cam_thread:
                self.cam_thread.stop()
                self.cam_thread = None
            self.camera_active = False
            cv2.destroyAllWindows()

    def run(self):
        print("Waiting for Arduino commands (WAKE/SLEEP)...")

        while self.running:
            # --- 1. Fast Serial Check ---
            if self.ser.in_waiting > 0:
                try:
                    line = self.ser.readline().decode('utf-8').strip()
                    if line == "WAKE":
                        self.start_camera()
                    elif line == "SLEEP":
                        self.stop_camera()
                except Exception as e:
                    print(f"Serial Error: {e}")

            # --- 2. Key Input (Only if window enabled) ---
            if self.show_camera and self.camera_active:
                key = cv2.waitKey(1) & 0xFF
                if key == ord('q'): self.running = False

            # --- 3. Optimized Detection Loop ---
            if self.camera_active and self.cam_thread:
                frame = self.cam_thread.read()

                if frame is not None:
                    # Run Inference
                    results = self.model.predict(
                        source=frame,
                        imgsz=320,
                        conf=CONFIDENCE_THRESHOLD,
                        verbose=False,
                        device='cpu',
                        stream=True
                    )

                    detected = False

                    # Iterate generator
                    for result in results:
                        if result.boxes:
                            for cls_id in result.boxes.cls:
                                if self.model.names[int(cls_id)] == TARGET_CLASS:
                                    detected = True
                                    break

                            # Visualization (Optional)
                            if self.show_camera:
                                res_plotted = result.plot()
                                cv2.imshow("Bottle Detector View", res_plotted)

                    # Signal Logic
                    current_time = time.time()
                    if detected:
                        # Debounce logic (2.0s cooldown)
                        if (current_time - self.last_bottle_time) > 2.0:
                            print("!!! BOTTLE DETECTED !!!")
                            self.ser.write(b"BOTTLE\n")
                            self.last_bottle_time = current_time

                # Run at max speed while active

            else:
                # If camera is off (Idle/Sleep), sleep to save CPU
                time.sleep(0.05)

        # Cleanup
        self.stop_camera()
        self.ser.close()

if __name__ == "__main__":
    app = BottleDetector()
    try:
        app.run()
    except KeyboardInterrupt:
        app.stop_camera()
        print("Exiting...")
