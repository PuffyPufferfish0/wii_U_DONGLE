import tkinter as tk
from tkinter import scrolledtext
import serial
import time
import threading
from evdev import UInput, ecodes as e, AbsInfo

class WiiUReceiverGUI:
    def __init__(self, root):
        self.root = root
        self.root.title("Wii U GamePad PC Receiver")
        self.root.geometry("600x450")
        
        self.running = False
        self.ser = None
        self.ui = None
        self.thread = None

        # --- Top Frame for Controls ---
        top_frame = tk.Frame(root)
        top_frame.pack(pady=10, padx=10, fill=tk.X)

        tk.Label(top_frame, text="Serial Port:").pack(side=tk.LEFT)
        self.port_entry = tk.Entry(top_frame, width=15)
        self.port_entry.insert(0, "/dev/ttyUSB0")
        self.port_entry.pack(side=tk.LEFT, padx=5)

        self.start_btn = tk.Button(top_frame, text="Start Receiver", command=self.start_receiver, bg="green", fg="white")
        self.start_btn.pack(side=tk.LEFT, padx=5)

        self.stop_btn = tk.Button(top_frame, text="Stop Receiver", command=self.stop_receiver, state=tk.DISABLED, bg="red", fg="white")
        self.stop_btn.pack(side=tk.LEFT, padx=5)
        
        # Hardware Simulator Button (For testing without the ESP32)
        self.sim_btn = tk.Button(top_frame, text="Simulate 'A' Press", command=self.simulate_press, state=tk.DISABLED, bg="blue", fg="white")
        self.sim_btn.pack(side=tk.RIGHT, padx=5)

        # --- Console/Log Area ---
        self.log_area = scrolledtext.ScrolledText(root, wrap=tk.WORD, state=tk.DISABLED, font=("Consolas", 10))
        self.log_area.pack(padx=10, pady=5, fill=tk.BOTH, expand=True)
        
        self.log("=== Wii U GamePad Linux Receiver ===")
        self.log("Ready. Full Xbox 360 mapping loaded.")
        self.log("Run with 'sudo' to grant kernel uinput access.")

    def log(self, message):
        """Helper to print text to the GUI console safely"""
        self.log_area.config(state=tk.NORMAL)
        self.log_area.insert(tk.END, message + "\n")
        self.log_area.see(tk.END)
        self.log_area.config(state=tk.DISABLED)

    def start_receiver(self):
        port = self.port_entry.get()
        baud = 115200
        
        self.log(f"\nAttempting to initialize virtual controller...")
        
        # 1. Init Virtual Controller with FULL Xbox 360 Capabilities
        try:
            capabilities = {
                e.EV_KEY: [
                    e.BTN_A, e.BTN_B, e.BTN_X, e.BTN_Y, 
                    e.BTN_TL, e.BTN_TR,           # Left/Right Bumpers
                    e.BTN_SELECT, e.BTN_START,    # Select/Start (-/+)
                    e.BTN_MODE,                   # Guide/Home Button
                    e.BTN_THUMBL, e.BTN_THUMBR    # Joystick Clicks
                ],
                e.EV_ABS: [
                    # Axes: (value, min, max, fuzz, flat, resolution)
                    (e.ABS_X, AbsInfo(value=0, min=-32768, max=32767, fuzz=16, flat=128, resolution=0)),
                    (e.ABS_Y, AbsInfo(value=0, min=-32768, max=32767, fuzz=16, flat=128, resolution=0)),
                    (e.ABS_RX, AbsInfo(value=0, min=-32768, max=32767, fuzz=16, flat=128, resolution=0)),
                    (e.ABS_RY, AbsInfo(value=0, min=-32768, max=32767, fuzz=16, flat=128, resolution=0)),
                    (e.ABS_Z, AbsInfo(value=0, min=0, max=255, fuzz=0, flat=0, resolution=0)),
                    (e.ABS_RZ, AbsInfo(value=0, min=0, max=255, fuzz=0, flat=0, resolution=0)),
                    (e.ABS_HAT0X, AbsInfo(value=0, min=-1, max=1, fuzz=0, flat=0, resolution=0)),
                    (e.ABS_HAT0Y, AbsInfo(value=0, min=-1, max=1, fuzz=0, flat=0, resolution=0))
                ]
            }
            self.ui = UInput(capabilities, name='Wii U GamePad (Virtual Xbox)', vendor=0x045e, product=0x028e)
            self.log("Virtual Controller Created!")
            self.sim_btn.config(state=tk.NORMAL) # Enable simulator
        except Exception as ex:
            self.log(f"[ERROR] UInput failed: {ex}")
            self.log("Hint: You must run this script with 'sudo' for kernel access.")
            return

        # 2. Init Serial Connection
        try:
            self.ser = serial.Serial(port, baud, timeout=1)
            self.log(f"Listening for GamePad input on {port}...")
            
            # 3. Start Background Thread (only if serial succeeds)
            self.running = True
            self.thread = threading.Thread(target=self.receive_loop, daemon=True)
            self.thread.start()
        except Exception as ex:
            self.log(f"[WARNING] Serial failed: {ex}")
            self.log("Hardware not found, but Virtual Controller is active. You can use the Simulator button.")
            # We don't return here so you can still test the simulator!

        # Update GUI state
        self.start_btn.config(state=tk.DISABLED)
        self.stop_btn.config(state=tk.NORMAL)
        self.port_entry.config(state=tk.DISABLED)

    def stop_receiver(self):
        self.running = False
        if self.thread:
            self.thread.join(timeout=2.0)
            
        if self.ser and self.ser.is_open:
            self.ser.close()
        if self.ui:
            self.ui.close()
            
        self.log("\nReceiver Stopped. Virtual Controller Disconnected.")
        
        self.start_btn.config(state=tk.NORMAL)
        self.stop_btn.config(state=tk.DISABLED)
        self.sim_btn.config(state=tk.DISABLED)
        self.port_entry.config(state=tk.NORMAL)

    def simulate_press(self):
        """Hardware simulator: Pretends the ESP32 sent an 'A' button press"""
        if self.ui:
            self.log("SIMULATOR: Sending 'A' button press...")
            self.ui.write(e.EV_KEY, e.BTN_A, 1) # Press
            self.ui.syn()
            time.sleep(0.1) # Hold for 100ms
            self.ui.write(e.EV_KEY, e.BTN_A, 0) # Release
            self.ui.syn()

    def receive_loop(self):
        """This runs in a background thread to prevent the GUI from locking up"""
        while self.running and self.ser and self.ser.is_open:
            try:
                if self.ser.in_waiting > 0:
                    incoming_data = self.ser.readline().decode('utf-8').strip()

                    # Trigger the virtual button and log it to the GUI
                    if incoming_data == "BTN_A_DOWN":
                        self.ui.write(e.EV_KEY, e.BTN_A, 1)
                        self.ui.syn()
                        self.root.after(0, self.log, "Hardware: GamePad 'A' Pressed")
                    
                    elif incoming_data == "BTN_A_UP":
                        self.ui.write(e.EV_KEY, e.BTN_A, 0)
                        self.ui.syn()
                        self.root.after(0, self.log, "Hardware: GamePad 'A' Released")
                else:
                    time.sleep(0.01) # Small sleep to prevent CPU hogging
            except Exception as ex:
                if self.running:
                    self.root.after(0, self.log, f"[ERROR] Serial read error: {ex}")
                    self.root.after(0, self.stop_receiver)
                break

if __name__ == "__main__":
    root = tk.Tk()
    app = WiiUReceiverGUI(root)
    # Ensure the receiver shuts down cleanly if the user closes the window with the 'X' button
    root.protocol("WM_DELETE_WINDOW", lambda: (app.stop_receiver(), root.destroy()))
    root.mainloop()
