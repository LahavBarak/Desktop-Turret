import serial
import time
import argparse
import matplotlib.pyplot as plt
import csv

def main():
    parser = argparse.ArgumentParser(description='Servo Position Control Test')
    parser.add_argument('angle', type=float, help='Target Angle (0-360)')
    parser.add_argument('--port', type=str, default='/dev/ttyACM0', help='Serial port')
    parser.add_argument('--baud', type=int, default=115200, help='Baud rate')
    parser.add_argument('--duration', type=float, default=2.0, help='Recording duration (s)')
    args = parser.parse_args()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.1)
        print(f"Waiting for boot (3s)...")
        time.sleep(3.5) 
        ser.reset_input_buffer()
        print(f"Connected to {args.port}")
    except serial.SerialException as e:
        print(f"Error: {e}")
        return

    # Send Target
    cmd = f"{args.angle}\n"
    print(f"Sending Target: {args.angle}")
    ser.write(cmd.encode())

    # Record Data
    times = []
    targets = []
    currents = []
    
    start_time = time.time()
    
    print("Recording response...")
    while time.time() - start_time < args.duration:
        try:
            line = ser.readline().decode(errors='ignore').strip()
            if not line:
                continue
                
            # Expected "Target,Current"
            parts = line.split(',')
            if len(parts) == 2:
                t_val = float(parts[0])
                c_val = float(parts[1])
                
                times.append(time.time() - start_time)
                targets.append(t_val)
                currents.append(c_val)
        except ValueError:
            pass

    ser.close()
    
    if not times:
        print("No data received.")
        return

    # Plot
    plt.figure(figsize=(10, 6))
    plt.plot(times, targets, 'r--', label='Target')
    plt.plot(times, currents, 'b-', label='Current')
    plt.xlabel('Time (s)')
    plt.ylabel('Angle (deg)')
    plt.title(f'Step Response: Target {args.angle}')
    plt.legend()
    plt.grid(True)
    plt.savefig('step_response.png')
    print("Saved step_response.png")
    # plt.show() 

    # Save CSV
    with open('step_data.csv', 'w') as f:
        writer = csv.writer(f)
        writer.writerow(['Time', 'Target', 'Current'])
        for i in range(len(times)):
            writer.writerow([times[i], targets[i], currents[i]])
    print("Saved step_data.csv")

if __name__ == "__main__":
    main()
