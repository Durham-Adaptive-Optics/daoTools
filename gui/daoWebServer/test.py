#!/usr/bin/env python3
"""
Test script for the DAO Shared Memory Web Viewer

This script creates some test shared memory arrays for demonstration.
"""

import numpy as np
import sys
import os

try:
    import dao
except ImportError:
    print("Error: 'dao' module not found. Please ensure it's installed and in your Python path.")
    sys.exit(1)

def create_test_arrays():
    """Create various test shared memory arrays."""
    
    print("Creating test shared memory arrays...")
    
    # Test array 1: 1D array (reshape to 2D for dao compatibility)
    print("Creating 1D test array...")
    array_1d = np.sin(np.linspace(0, 4*np.pi, 100)).reshape(100, 1)
    shm_1d = dao.shm('/tmp/test_1d.im.shm', array_1d, logLevel=0)
    
    # Test array 2: 2D array
    print("Creating 2D test array...")
    x, y = np.meshgrid(np.linspace(-2, 2, 50), np.linspace(-2, 2, 50))
    array_2d = np.sin(x) * np.cos(y)
    shm_2d = dao.shm('/tmp/test_2d.im.shm', array_2d, logLevel=0)
    
    # Test array 3: 3D array (small for performance)
    print("Creating 3D test array...")
    array_3d = np.random.rand(10, 20, 20)
    shm_3d = dao.shm('/tmp/test_3d.im.shm', array_3d, logLevel=0)
    
    # Test array 4: Integer array
    print("Creating integer test array...")
    array_int = np.random.randint(0, 100, (25, 25), dtype=np.int32)
    shm_int = dao.shm('/tmp/test_int.im.shm', array_int, logLevel=0)
    
    # Test array 5: Small table-like array
    print("Creating small table array...")
    array_table = np.array([[1.0, 2.0], [3.0, 4.0]])
    shm_table = dao.shm('/tmp/test_table.im.shm', array_table, logLevel=0)
    
    print("\nTest arrays created successfully!")
    print("\nAvailable test files:")
    print("- test_1d.im.shm      (1D sine wave)")
    print("- test_2d.im.shm      (2D sine-cosine pattern)")
    print("- test_3d.im.shm      (3D random data)")
    print("- test_int.im.shm     (2D integer array)")
    print("- test_table.im.shm   (Small 2x2 table)")
    
    return [shm_1d, shm_2d, shm_3d, shm_int, shm_table]

def update_test_arrays(shm_objects):
    """Update test arrays with new data to demonstrate real-time updates."""
    
    import time
    
    print("\nStarting real-time updates...")
    print("Press Ctrl+C to stop")
    
    try:
        counter = 0
        while True:
            # Update 1D array with moving sine wave
            t = counter * 0.1
            array_1d = np.sin(np.linspace(0 + t, 4*np.pi + t, 100)).reshape(100, 1)
            shm_objects[0].set_data(array_1d)
            
            # Update 2D array with rotating pattern
            x, y = np.meshgrid(np.linspace(-2, 2, 50), np.linspace(-2, 2, 50))
            array_2d = np.sin(x + t) * np.cos(y + t)
            shm_objects[1].set_data(array_2d)
            
            # Update 3D array with new random data occasionally
            if counter % 10 == 0:
                array_3d = np.random.rand(10, 20, 20)
                shm_objects[2].set_data(array_3d)
            
            # Update integer array with random values
            if counter % 5 == 0:
                array_int = np.random.randint(0, 100, (25, 25), dtype=np.int32)
                shm_objects[3].set_data(array_int)
            
            counter += 1
            time.sleep(0.1)  # 10 Hz update rate
            
    except KeyboardInterrupt:
        print("\nStopping updates...")

def cleanup_test_arrays():
    """Clean up test arrays."""
    
    test_files = [
        '/tmp/test_1d.im.shm',
        '/tmp/test_2d.im.shm',
        '/tmp/test_3d.im.shm',
        '/tmp/test_int.im.shm',
        '/tmp/test_table.im.shm'
    ]
    
    print("Cleaning up test files...")
    for file in test_files:
        try:
            if os.path.exists(file):
                os.remove(file)
                print(f"Removed {file}")
        except Exception as e:
            print(f"Error removing {file}: {e}")

def main():
    """Main test function."""
    
    print("DAO Shared Memory Web Viewer Test Script")
    print("=" * 45)
    
    if len(sys.argv) > 1:
        command = sys.argv[1].lower()
        
        if command == 'cleanup':
            cleanup_test_arrays()
            return
        elif command == 'create':
            shm_objects = create_test_arrays()
            return
        elif command == 'update':
            print("Creating test arrays first...")
            shm_objects = create_test_arrays()
            update_test_arrays(shm_objects)
            return
        elif command == 'help':
            print("Usage: python test.py [command]")
            print("Commands:")
            print("  create  - Create test arrays")
            print("  update  - Create test arrays and start real-time updates")
            print("  cleanup - Remove test arrays")
            print("  help    - Show this help")
            return
        else:
            print(f"Unknown command: {command}")
            print("Use 'python test.py help' for usage information")
            return
    
    # Default behavior: create arrays and start updates
    shm_objects = create_test_arrays()
    
    print("\nYou can now:")
    print("1. Start the web viewer: python app.py")
    print("2. Or run updates: python test.py update")
    print("3. Or cleanup: python test.py cleanup")

if __name__ == "__main__":
    main()
