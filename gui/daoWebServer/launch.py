#!/usr/bin/env python3
"""
DAO Shared Memory Web Viewer Launcher

This script starts the web-based version of the DAO Shared Memory Viewer.
"""

import os
import sys
import subprocess
import webbrowser
import time
from pathlib import Path

def check_requirements():
    """Check if required packages are installed."""
    required_packages = [
        'flask',
        'flask_socketio',
        'numpy',
        'astropy',
        'dao'
    ]
    
    missing_packages = []
    
    for package in required_packages:
        try:
            __import__(package)
        except ImportError:
            if package == 'flask_socketio':
                try:
                    __import__('flask-socketio')
                except ImportError:
                    missing_packages.append('flask-socketio')
            else:
                missing_packages.append(package)
    
    if missing_packages:
        print("Missing required packages:")
        for package in missing_packages:
            print(f"  - {package}")
        print("\nInstall missing packages with:")
        print("  pip install -r requirements.txt")
        return False
    
    return True

def find_free_port(start_port=5000):
    """Find a free port starting from start_port."""
    import socket
    
    for port in range(start_port, start_port + 100):
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.bind(('localhost', port))
                return port
        except OSError:
            continue
    
    return start_port  # Fallback

def main():
    """Main launcher function."""
    print("DAO Shared Memory Web Viewer Launcher")
    print("=" * 40)
    
    # Get the directory of this script
    script_dir = Path(__file__).parent
    
    # Change to the web server directory
    os.chdir(script_dir)
    
    # Check requirements
    print("Checking requirements...")
    if not check_requirements():
        sys.exit(1)
    
    print("All requirements satisfied!")
    
    # Find a free port
    port = find_free_port()
    
    # Start the web server
    print(f"Starting web server on port {port}...")
    
    try:
        # Set environment variables
        env = os.environ.copy()
        env['FLASK_APP'] = 'app.py'
        env['FLASK_ENV'] = 'development'
        
        # Start the server process
        process = subprocess.Popen(
            [sys.executable, 'app.py'],
            env=env,
            cwd=script_dir
        )
        
        # Wait a moment for the server to start
        time.sleep(2)
        
        # Open browser
        url = f"http://localhost:{port}"
        print(f"Opening browser at: {url}")
        webbrowser.open(url)
        
        print("\nWeb server is running!")
        print("Press Ctrl+C to stop the server")
        
        # Wait for the process
        process.wait()
        
    except KeyboardInterrupt:
        print("\nStopping web server...")
        process.terminate()
        process.wait()
        print("Web server stopped.")
    
    except Exception as e:
        print(f"Error starting web server: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()
