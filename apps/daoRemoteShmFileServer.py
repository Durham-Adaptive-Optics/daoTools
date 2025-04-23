#!/usr/bin/env python3

import zmq
import os
import glob
import numpy as np
import sys
import argparse
from daoFileServer_pb2 import FileRequest, FileResponse
import dao

class daoRemoteShmFileServer:
    def __init__(self, port=5555, shm_dir="/tmp"):
        self.context = zmq.Context()
        self.socket = self.context.socket(zmq.REP)
        self.socket.bind(f"tcp://*:{port}")
        self.shm_dir = shm_dir
        self.pub_port_counter = 5600  # Starting port for server's publishers
        self.sub_port_counter = 5800  # Starting port for client publishers (server subscribes)
        self.active_shms = {}  # {filepath: (shm, pub_port, sub_port)}
        print(f"File server listening on port {port}")

    def handle_list_files(self, request):
        response = FileResponse()
        try:
            # Only list .im.shm files in /tmp
            files = glob.glob(os.path.join(self.shm_dir, "*.im.shm"))
            # Convert to just filenames
            files = [os.path.basename(f) for f in files]
            response.status = FileResponse.SUCCESS
            response.file_list.extend(files)
        except Exception as e:
            response.status = FileResponse.ERROR
            response.error_message = str(e)
        return response

    def handle_open_file(self, request):
        response = FileResponse()
        try:
            file_path = os.path.join(self.shm_dir, os.path.basename(request.file_path))
            if not file_path.endswith('.im.shm'):
                file_path += '.im.shm'
                
            if not os.path.exists(file_path):
                response.status = FileResponse.FILE_NOT_FOUND
                return response

            # If we already have this file open, return existing metadata and current data
            if file_path in self.active_shms:
                shm, pub_port, sub_port = self.active_shms[file_path]
                data = shm.get_data()
                response.status = FileResponse.SUCCESS
                response.metadata.shape.extend(list(data.shape))
                response.metadata.dtype = str(data.dtype)
                response.metadata.pub_port = pub_port
                response.metadata.sub_port = sub_port
                # Serialize and include the initial data
                response.initial_data = data.tobytes()
                return response

            # Open the SHM file using daoShm
            shm = dao.shm(file_path)
            data = shm.get_data()
            
            # Set up publisher for server -> client updates
            pub_port = self.pub_port_counter
            self.pub_port_counter += 1
            shm.pubPort = pub_port
            shm.pubContext = zmq.Context()
            shm.pubEnable = True
            shm.pubThread.start()
            
            # Set up subscriber for client -> server updates
            sub_port = self.sub_port_counter
            self.sub_port_counter += 1
            shm.subPort = sub_port
            shm.subHost = 'localhost'
            shm.subContext = zmq.Context()
            shm.subEnable = True
            shm.subThread.start()
            
            # Store in active SHMs
            self.active_shms[file_path] = (shm, pub_port, sub_port)

            # Set response metadata
            response.status = FileResponse.SUCCESS
            response.metadata.shape.extend(list(data.shape))
            response.metadata.dtype = str(data.dtype)
            response.metadata.pub_port = pub_port
            response.metadata.sub_port = sub_port
            # Serialize and include the initial data
            response.initial_data = data.tobytes()

        except PermissionError:
            response.status = FileResponse.PERMISSION_DENIED
        except Exception as e:
            response.status = FileResponse.ERROR
            response.error_message = str(e)
        return response

    def run(self):
        while True:
            try:
                # Wait for next request from client
                message = self.socket.recv()
                request = FileRequest()
                request.ParseFromString(message)

                # Handle the request
                if request.request_type == FileRequest.LIST_FILES:
                    response = self.handle_list_files(request)
                elif request.request_type == FileRequest.OPEN_FILE:
                    response = self.handle_open_file(request)
                else:
                    response = FileResponse(
                        status=FileResponse.ERROR,
                        error_message="Unknown request type"
                    )

                # Send response back to client
                self.socket.send(response.SerializeToString())
            except Exception as e:
                # Send error response if something goes wrong
                response = FileResponse(
                    status=FileResponse.ERROR,
                    error_message=str(e)
                )
                self.socket.send(response.SerializeToString())

    def __del__(self):
        # Clean up all active SHMs
        for shm, _, _ in self.active_shms.values():
            try:
                shm.pubEnable = False
                shm.pubEvent.set()
                shm.pubThread.join()
                shm.subEnable = False
                shm.subEvent.set() 
                shm.subThread.join()
                shm.close()
            except:
                pass

if __name__ == "__main__":
    # Add command line arguments for port
    parser = argparse.ArgumentParser(description='DAO Shared Memory File Server')
    parser.add_argument('--port', type=int, default=5555, help='Port to listen on (default: 5555)')
    args = parser.parse_args()
    
    # Create and run server with specified port
    server = daoRemoteShmFileServer(port=args.port)
    server.run()