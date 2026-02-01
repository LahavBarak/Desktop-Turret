import serial
import time
import pandas as pd
import matplotlib.pyplot as plt
import argparse
import sys

def main():
    parser = argparse.ArgumentParser(description='Trigger Pull Test Controller')
    parser.add_argument('--port', type=str, default='/dev/ttyACM0', help='Serial port')
    parser.add_argument('--baud', type=int, default=115200, help='Baud rate')
    parser.add_argument('--output', type=str, default='trigger_data.csv', help='Output CSV file')
    args = parser.parse_args()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=1)
        print(f"Connected to {args.port} at {args.baud}")
    except serial.SerialException as e:
        print(f"Error connecting to serial: {e}")
        sys.exit(1)

    # Wait for board to reset/ready
    time.sleep(2) 
    ser.reset_input_buffer()

    print("Sending Go Command...")
    ser.write(b'g')

    data = []
    
    print("Recording data...")
    try:
        while True:
            line = ser.readline().decode('utf-8', errors='ignore').strip()
            if not line:
                continue
            
            print(f"Received: {line}")
            
            if line == "DONE":
                print("Test Complete.")
                break
            
            if line == "STARTING":
                continue

            try:
                # Expected format: "ANGLE,ACCUMULATED"
                parts = line.split(',')
                if len(parts) == 2:
                    angle = float(parts[0])
                    acc = float(parts[1])
                    timestamp = time.time()
                    data.append({'timestamp': timestamp, 'angle': angle, 'accumulated': acc})
            except ValueError:
                pass # Ignore bad lines

    except KeyboardInterrupt:
        print("Interrupted.")
    finally:
        ser.close()

    if not data:
        print("No data recorded.")
        return

    # Process Data
    df = pd.DataFrame(data)
    df['relative_time'] = df['timestamp'] - df['timestamp'].iloc[0]
    
    print(f"Saving to {args.output}")
    df.to_csv(args.output, index=False)

    # Plot
    plt.figure(figsize=(10, 6))
    plt.plot(df['relative_time'], df['accumulated'], label='Accumulated Angle')
    plt.plot(df['relative_time'], df['angle'], label='Raw Angle', alpha=0.5)
    plt.axhline(y=180, color='r', linestyle='--', label='Target (180)')
    plt.xlabel('Time (s)')
    plt.ylabel('Angle (degrees)')
    plt.title('Trigger Pull Test Results')
    plt.legend()
    plt.grid(True)
    plt.savefig('trigger_plot.png')
    print("Plot saved to trigger_plot.png")
    plt.show()

if __name__ == '__main__':
    main()
