#!/usr/bin/env python
import dao
import subprocess
import yaml
import numpy as np
import glob
import pickle
import argparse
import time
from datetime import datetime



def get_output(command):
    """
    Runs a shell command and returns its output as a string.
    """
    try:
        result = subprocess.check_output(command, shell=True, text=True)
        return result.strip()
    except subprocess.CalledProcessError:
        return ""

def create_process_snapshot():
    # Initialize the data structure for storing tmux session details
    tmux_data = []

    # Get the list of tmux sessions
    sessions = get_output("tmux ls -F '#{session_name}'").splitlines()

    if not sessions:
        print("No tmux sessions found.")
        return

    # Loop through each session
    for session in sessions:
        session_data = {"session": session, "windows": []}

        # Get the list of windows in the session
        windows = get_output(f"tmux list-windows -t {session} -F '#{{window_index}}'").splitlines()

        for window in windows:
            window_data = {"window": int(window), "panes": []}

            # Get the list of panes in the window
            panes = get_output(f"tmux list-panes -t {session}:{window} -F '#{{pane_index}}'").splitlines()

            for pane in panes:
                pane_data = {"pane": int(pane)}

                # Get the pane_pid
                pane_pid = get_output(f"tmux list-panes -t {session}:{window} -F '#{{pane_pid}}'").splitlines()
                pane_pid = pane_pid[int(pane)] if pane_pid and int(pane) < len(pane_pid) else None

                if pane_pid:
                    # Get the child process of the pane PID
                    child_pid = get_output(f"pgrep -P {pane_pid}")

                    if child_pid:
                        # Instead of relying on ps or /proc directly, 
                        # Use a combination of readlink and ps to get the full command with arguments
                        cmd_path = get_output(f"readlink -f /proc/{child_pid}/exe")
                        
                        # Get command arguments from /proc/[pid]/cmdline with careful handling
                        try:
                            with open(f"/proc/{child_pid}/cmdline", 'rb') as f:
                                cmdline_bytes = f.read()
                                print(f"Command line bytes: {cmdline_bytes}")
                            
                            # Split by null bytes
                            args = cmdline_bytes.split(b'\0')
                            args = [arg.decode('utf-8', errors='replace') for arg in args if arg]
                            
                            if args:
                                # First element is the command itself
                                cmd = args[0]
                                
                                # Reconstruct with proper quoting
                                for i in range(1, len(args)):
                                    arg = args[i]
                                    if ' ' in arg:
                                        args[i] = f'"{arg}"'
                                
                                final_cmd = ' '.join(args)
                                pane_data["running_command"] = final_cmd
                            else:
                                # Fallback to basic command if we couldn't get args
                                pane_data["running_command"] = cmd_path if cmd_path else "(Unknown command)"
                        except (IOError, FileNotFoundError):
                            # If /proc access fails, try a direct ps approach with custom format
                            cmdline = get_output(f"ps -p {child_pid} -o command=")
                            print(f"Command line from ps: {cmdline}")
                            # Try to add quotes manually for arguments with spaces
                            words = []
                            current_word = ""
                            in_quotes = False
                            quote_char = None
                            
                            for char in cmdline:
                                if char == ' ' and not in_quotes:
                                    if current_word:
                                        words.append(current_word)
                                        current_word = ""
                                elif char in ['"', "'"]:
                                    if not in_quotes:
                                        in_quotes = True
                                        quote_char = char
                                    elif char == quote_char:
                                        in_quotes = False
                                        quote_char = None
                                    current_word += char
                                else:
                                    current_word += char
                            
                            if current_word:
                                words.append(current_word)
                            
                            # Add quotes to words with spaces that don't already have quotes
                            for i in range(len(words)):
                                if ' ' in words[i] and not (words[i].startswith('"') or words[i].startswith("'")):
                                    words[i] = f'"{words[i]}"'
                            
                            pane_data["running_command"] = ' '.join(words)
                    else:
                        pane_data["running_command"] = "(No active child process)"
                else:
                    pane_data["pane_pid"] = "(Not found)"

                # Add pane data to the window
                window_data["panes"].append(pane_data)

            # Add window data to the session
            session_data["windows"].append(window_data)

        # Add session data to the overall tmux data
        tmux_data.append(session_data)
    
    return tmux_data

def create_shm_snapshot(output_file, config_data):
    snapshot = {'config': config_data, 'data': {}}
    for filename in glob.glob(f"/tmp/*im.shm"):
        shm = dao.shm(filename)
        data = shm.get_data()
        snapshot['data'][filename] = data
    with open(output_file, 'wb') as f:
        pickle.dump(snapshot, f)
        
def load_process_snapshot(process):
    print(f'Loading snapshot... {process}')
    for session_data in process:
        session_name = session_data['session']

        # Create a new tmux session if it doesn't exist
        get_output(f"tmux new-session -d -s {session_name}")

        for window_data in session_data['windows']:
            window_index = window_data['window']
            
            # Create a new tmux window
            get_output(f"tmux new-window -t {session_name}:{window_index}")

            for pane_data in window_data['panes']:
                pane_index = pane_data['pane']
                running_command = pane_data.get('running_command', '')
                print(f"Running command: {running_command}") 

                # Split the command and create a new pane with the specified command
                if running_command:
                    # Create a new tmux pane with the running command
                    #get_output(f"tmux split-window -t {session_name}:{window_index} -h")
                    get_output(f"tmux send-keys -t {session_name}:{window_index}.{pane_index} '{running_command}' C-m")
        
        print(f"Created tmux session '{session_name}' with windows and panes.")

def load_shm_snapshot(snapshot_file):
    print(f'Loading snapshot... {snapshot_file}')
    for filename, data in snapshot_file.items():
        print(f"Recreating shared memory file: {filename}")
        shm = dao.shm(filename, data)
        
def load_snapshot(snapshot_file):
    print(f'Loading snapshot... {snapshot_file}')
    with open(snapshot_file, 'rb') as f:
        snapshot = pickle.load(f)
    print(snapshot)
    config = snapshot['config']
    shm = snapshot['data']
    
    load_shm_snapshot(shm)
    load_process_snapshot(config)
    
    
def create_snapshot(snapshot_file):
    print(f'Creating snapshot... {snapshot_file}')
    process_data = create_process_snapshot()
    print(process_data)
    create_shm_snapshot(snapshot_file, process_data)

def main():
    parser = argparse.ArgumentParser(description='Process some shared memory files.')
    parser.add_argument('cmd', choices=['load', 'create'], help='Command to execute: load or create')
    parser.add_argument('yaml_file', nargs='?', default=None, help='Path to the YAML configuration file (optional)')

    args = parser.parse_args()



    
    if args.cmd == 'load':
        if args.yaml_file is None:
            print('Please provide a YAML file to load the snapshot from.')
            parser.print_help()
            exit(1)
        load_snapshot(args.yaml_file)
    elif args.cmd == 'create':
        if args.yaml_file is None:
            yaml_file = f"timestamp_{datetime.now().strftime('%Y%m%d_%H%M%S')}.dao"
        else:
            yaml_file = args.yaml_file
        create_snapshot(yaml_file)
    else:
        print('Invalid command. Use "load" or "create".')
        parser.print_help()

if __name__ == '__main__':
    main()