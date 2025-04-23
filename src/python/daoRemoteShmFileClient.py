#!/usr/bin/env python3

import zmq
import sys, os
import numpy as np
from daoFileServer_pb2 import FileRequest, FileResponse
import dao

class daoRemoteShmFileClient:
    def __init__(self, server_address="localhost", port=5555):
        self.context = zmq.Context()
        self.socket = self.context.socket(zmq.REQ)
        # Set socket timeout to prevent hanging
        self.socket.setsockopt(zmq.RCVTIMEO, 5000)  # 5 second timeout
        self.socket.connect(f"tcp://{server_address}:{port}")
        self.server_address = server_address
        self.port = port
        print(f"Connected to file server at {server_address}:{port}")
        self.active_shms = {}  # {filepath: shm}

    def list_files(self):
        request = FileRequest(
            request_type=FileRequest.LIST_FILES
        )
        self.socket.send(request.SerializeToString())
        response = FileResponse()
        response.ParseFromString(self.socket.recv())
        
        if response.status == FileResponse.SUCCESS:
            return response.file_list
        else:
            raise Exception(f"Error listing files: {response.error_message}")

    def open_file(self, file_path):
        """Open a SHM file and set up bidirectional sync with server"""
        # Always close the file if it's already open to ensure fresh data
        if file_path in self.active_shms:
            self.close_file(file_path)
            
        # Request fresh metadata from server every time
        request = FileRequest(
            request_type=FileRequest.OPEN_FILE,
            file_path=file_path
        )
        self.socket.send(request.SerializeToString())
        response = FileResponse()
        response.ParseFromString(self.socket.recv())
        
        if response.status != FileResponse.SUCCESS:
            if response.status == FileResponse.FILE_NOT_FOUND:
                raise FileNotFoundError(f"File not found: {file_path}")
            elif response.status == FileResponse.PERMISSION_DENIED:
                raise PermissionError(f"Permission denied: {file_path}")
            else:
                raise Exception(f"Error opening file: {response.error_message}")
                
        # Extract metadata
        shape = tuple(response.metadata.shape)
        dtype = np.dtype(response.metadata.dtype)
        pub_port = response.metadata.pub_port
        sub_port = response.metadata.sub_port
        print(f"Opened file {file_path} with shape {shape} and dtype {dtype}")
        print(f"Publisher port: {pub_port}, Subscriber port: {sub_port}")
        
        # Deserialize initial data if present
        if response.initial_data:
            initial_data = np.frombuffer(response.initial_data, dtype=dtype).reshape(shape)
        else:
            # Fallback to zeros if no initial data was provided
            initial_data = np.zeros(shape, dtype=dtype)
        
        # Create local SHM with the initial data
        local_name = f"/tmp/local_{file_path}_{os.getpid()}"
        local_shm = dao.shm(
            local_name, 
            initial_data
        )

        # Set up subscriber to receive server updates
        local_shm.subPort = pub_port
        local_shm.subHost = self.server_address
        local_shm.subContext = zmq.Context()
        local_shm.subEnable = True
        local_shm.subThread.start()

        local_shm.pubPort = sub_port
        local_shm.pubHost = self.server_address
        local_shm.pubContext = zmq.Context()
        local_shm.pubEnable = True
        local_shm.pubThread.start()

        # Store reference to local SHM
        self.active_shms[file_path] = local_shm
        
        return local_shm

    def close_file(self, file_path):
        """Close a local SHM file and clean up sync threads"""
        if file_path not in self.active_shms:
            return
        
        shm = self.active_shms[file_path]
        # Remove from active_shms right away to prevent double-closing
        del self.active_shms[file_path]
        
        try:
            # Stop subscriber thread
            shm.subEnable = False
            shm.subEvent.set()
            if shm.subThread.is_alive():
                shm.subThread.join(timeout=1.0)  # Only wait 1 second max
                
            # Stop publisher thread
            shm.pubEnable = False
            shm.pubEvent.set()
            if shm.pubThread.is_alive():
                shm.pubThread.join(timeout=1.0)  # Only wait 1 second max
                
            # Close and remove SHM file
            shm.close()
            
            # Delete the file from filesystem
            try:
                local_name = f"/tmp/local_{file_path}_{os.getpid()}"
                if os.path.exists(local_name):
                    os.remove(local_name)
            except OSError as e:
                print(f"Error deleting file: {e}")
                
        except Exception as e:
            print(f"Error during close: {e}")

    def __del__(self):
        """Clean up all open SHM files and sync threads"""
        # Close all active SHMs
        file_paths = list(self.active_shms.keys())
        for file_path in file_paths:
            self.close_file(file_path)
        
        # Close ZMQ socket and context
        try:
            self.socket.close()
            self.context.term()
        except:
            pass

def main():
    if len(sys.argv) < 2:
        print("Usage: file_client.py <command> [args]")
        print("Commands:")
        print("  list")
        print("  open <file_path>")
        print("  close <file_path>")
        return

    client = daoRemoteShmFileClient()
    command = sys.argv[1]

    try:
        if command == "list":
            files = client.list_files()
            print("\n".join(files))
        elif command == "open":
            if len(sys.argv) < 3:
                print("Error: open command requires a file path")
                return
            shm = client.open_file(sys.argv[2])
            print(f"Opened {sys.argv[2]}")
            print(f"Shape: {shm.get_data().shape}")
            print(f"dtype: {shm.get_data().dtype}")
            input("Press Enter to close connection...")
            client.close_file(sys.argv[2])
        elif command == "close":
            if len(sys.argv) < 3:
                print("Error: close command requires a file path")
                return
            client.close_file(sys.argv[2])
            print(f"Closed {sys.argv[2]}")
        else:
            print(f"Unknown command: {command}")
    except Exception as e:
        print(f"Error: {e}")

if __name__ == "__main__":
    main()