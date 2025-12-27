import serial
import time
import cv2
from ultralytics import YOLO
import sys
import threading

# Configuration
SERIAL_PORT = '/dev/ttyUSB0'
BAUD_RATE = 9600
CONFIDENCE_THRESHOLD = 0.5
# YOLO Class ID for 'bottle'.
# Note: In COCO (default YOLO model), 'bottle' is usually ID 39.
# We will filter by name to be safe.
TARGET_CLASS = 'bottle'

class BottleDetector:
    def __init__(self):
        self.ser = None
        self.running = True
        self.camera_active = False
        self.show_camera = False
        self.cap = None
        self.last_bottle_time = 0  # Timestamp of last sent signal

        # Initialize Serial
        print(f"Connecting to {SERIAL_PORT}...")
        while self.ser is None:
            try:
                self.ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
                time.sleep(2) # Wait for Arduino reset
            except serial.SerialException:
                print("Waiting for serial device...")
                time.sleep(2)

        # Initialize Model (Load once to memory to save startup time later)
        print("Loading YOLOv11n model...")
        self.model = YOLO("yolo11n.pt")
        print("Model loaded. Sending Handshake.")

        # Handshake
        self.ser.write(b"HANDSHAKE\n")

    def start_camera(self):
        if not self.cap or not self.cap.isOpened():
            self.cap = cv2.VideoCapture(0)
            # Reduce resolution for speed
            self.cap.set(cv2.CAP_PROP_FRAME_WIDTH, 320)
            self.cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 240)
            self.camera_active = True
            print("Camera Started. Detection Loop Active.")

    def stop_camera(self):
        if self.cap and self.cap.isOpened():
            self.cap.release()
        self.camera_active = False
        cv2.destroyAllWindows()
        print("Camera Stopped. Power Saving Mode.")

    def run(self):
        print("System Ready. Waiting for Arduino commands...")

        while self.running:
            # 1. Check Serial Commands
            if self.ser.in_waiting > 0:
                try:
                    line = self.ser.readline().decode('utf-8').strip()
                    if line == "WAKE":
                        self.start_camera()
                    elif line == "SLEEP":
                        self.stop_camera()
                except Exception as e:
                    print(f"Serial Error: {e}")

            # 2. Key Input (Non-blocking check via cv2 only if window needed)
            key = cv2.waitKey(1) & 0xFF
            if key == ord('c') or key == ord('C'):
                self.show_camera = True
                print("Camera View: ON")
            elif key == ord('e') or key == ord('E'):
                self.show_camera = False
                cv2.destroyAllWindows()
                print("Camera View: OFF")
            elif key == ord('q'):
                self.running = False

            # 3. Detection Logic (Only if camera is active)
            if self.camera_active and self.cap.isOpened():
                ret, frame = self.cap.read()
                if ret:
                    # Run Inference
                    results = self.model.predict(frame, verbose=False, conf=CONFIDENCE_THRESHOLD)

                    detected = False

                    # Analyze results
                    for result in results:
                        for box in result.boxes:
                            cls_id = int(box.cls[0])
                            cls_name = self.model.names[cls_id]

                            if cls_name == TARGET_CLASS:
                                detected = True
                                # Draw box if view is enabled
                                if self.show_camera:
                                    x1, y1, x2, y2 = map(int, box.xyxy[0])
                                    cv2.rectangle(frame, (x1, y1), (x2, y2), (0, 255, 0), 2)
                                    cv2.putText(frame, "BOTTLE", (x1, y1 - 10),
                                              cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 0), 2)

                    # Logic: If detected AND enough time passed since last detection
                    current_time = time.time()
                    if detected:
                        if (current_time - self.last_bottle_time) > 2.0:
                            print("Bottle Detected! Sending signal...")
                            self.ser.write(b"BOTTLE\n")
                            self.last_bottle_time = current_time
                        else:
                            # Detected but waiting for cooldown
                            pass

                    # Display logic
                    if self.show_camera:
                        cv2.imshow("Bottle Detector View", frame)
                    else:
                        try:
                            if cv2.getWindowProperty("Bottle Detector View", 0) >= 0:
                                cv2.destroyWindow("Bottle Detector View")
                        except:
                            pass

                # Throttle to approx 2 Frames Per Second (0.5s delay)
                time.sleep(0.5)

            # Small sleep to prevent CPU hogging in idle mode
            if not self.camera_active:
                time.sleep(0.1)

        # Cleanup
        self.stop_camera()
        self.ser.close()

if __name__ == "__main__":
    app = BottleDetector()
    try:
        app.run()
    except KeyboardInterrupt:
        print("Exiting...")
        app.stop_camera()
