import serial
import time
import argparse
import sys
import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
from scipy.stats import linregress

def main():
    parser = argparse.ArgumentParser(description='Servo Velocity Test Runner')
    parser.add_argument('--port', type=str, default='/dev/ttyACM0', help='Serial port')
    parser.add_argument('--baud', type=int, default=115200, help='Baud rate')
    parser.add_argument('--output', type=str, default='velocity_data.csv', help='Output CSV file')
    args = parser.parse_args()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=1)
        print(f"Connected to {args.port} at {args.baud}")
    except serial.SerialException as e:
        print(f"Error connecting to serial: {e}")
        sys.exit(1)

    # Wait for board reset
    time.sleep(2)
    ser.reset_input_buffer()

    print("Sending Go Command...")
    ser.write(b'g')

    # Data structure: {pulse_width: [{'timestamp': t, 'accumulated': a}, ...]}
    all_data = {}
    current_pulse = None
    pulse_start_time = 0
    
    print("Recording data...")
    try:
        while True:
            line = ser.readline().decode('utf-8', errors='ignore').strip()
            if not line:
                continue
            
            # Check for protocol markers
            if line.startswith("PULSE:"):
                current_pulse = int(line.split(':')[1])
                all_data[current_pulse] = []
                pulse_start_time = time.time()
                print(f"\nStarting Pulse: {current_pulse}")
                continue
                
            if line == "SUB_DONE":
                print(f"Finished Pulse: {current_pulse}")
                current_pulse = None
                continue
                
            if line == "DONE":
                print("\nTest Sequence Complete.")
                break

            # Parse Data
            if current_pulse is not None:
                try:
                    # Expected: "CALIBRATED,ACCUMULATED,TIME_MS"
                    parts = line.split(',')
                    if len(parts) >= 2:
                        acc = float(parts[1])
                        
                        if len(parts) >= 3:
                            # Use Hardware Timestamp (ms -> s)
                            rel_time = float(parts[2]) / 1000.0
                        else:
                            # Fallback to PC time
                            rel_time = time.time() - pulse_start_time
                            
                        all_data[current_pulse].append({
                            'time': rel_time,
                            'accumulated': acc
                        })
                except ValueError:
                    pass

    except KeyboardInterrupt:
        print("Interrupted.")
    finally:
        ser.close()

    if not all_data:
        print("No data recorded.")
        return

    # Process and Plot
    plt.figure(figsize=(12, 8))
    
    # Prepare CSV data
    csv_rows = []

    print("\n--- Results ---")
    
    # Sort pulses for consistent plotting
    sorted_pulses = sorted(all_data.keys())
    
    for pulse in sorted_pulses:
        data_points = all_data[pulse]
        if not data_points:
            continue
            
        df = pd.DataFrame(data_points)
        
        # Outlier Removal (Iterative Residuals)
        if len(df) > 10:
            # Initial fit
            slope, intercept, _, _, _ = linregress(df['time'], df['accumulated'])
            predicted = slope * df['time'] + intercept
            residuals = np.abs(df['accumulated'] - predicted)
            std_dev = np.std(residuals)
            
            # Filter: keep points within 2 std devs (or at least reasonable threshold)
            # If std_dev is tiny (clean data), this might be too aggressive, so min threshold
            threshold = max(2 * std_dev, 5.0) # Minimum 5 degrees deviation allowed
            
            df_clean = df[residuals < threshold]
            
            if len(df) != len(df_clean):
                 print(f"Pulse {pulse}: Removed {len(df) - len(df_clean)} outliers")
            
            df = df_clean
        
        # Add to CSV list
        for _, row in df.iterrows():
            csv_rows.append({
                'pulse_width': pulse,
                'time': row['time'],
                'accumulated': row['accumulated']
            })

        # Calculate Slope (Velocity)
        # Using linear regression on time vs accumulated angle
        if len(df) > 5: # Need enough points
            slope, intercept, r_value, p_value, std_err = linregress(df['time'], df['accumulated'])
            print(f"for rotate pulse {pulse}, slope is {slope:.2f} deg/s")
            
            # Plot actual data
            plt.plot(df['time'], df['accumulated'], label=f'{pulse} (v={slope:.0f})')
        else:
            print(f"Pulse {pulse}: Not enough data")

    # Save CSV
    pd.DataFrame(csv_rows).to_csv(args.output, index=False)
    print(f"\nData saved to {args.output}")

    # Finalize Plot
    plt.xlabel('Time (s)')
    plt.ylabel('Accumulated Angle (deg)')
    plt.title('Servo Velocity vs Pulse Width')
    plt.axhline(y=180, color='r', linestyle='--', alpha=0.3, label='Target')
    plt.legend()
    plt.grid(True)
    plt.savefig('velocity_plot.png')
    print("Plot saved to velocity_plot.png")
    plt.show()

if __name__ == '__main__':
    main()
