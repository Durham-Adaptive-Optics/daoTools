#!/usr/bin/env python3

import sys
import os
import re
import yaml
from PyQt5.QtWidgets import (
    QApplication, QWidget, QVBoxLayout, QHBoxLayout, QTableWidget, QTableWidgetItem,
    QCheckBox, QHeaderView, QLineEdit, QPushButton, QLabel, QSpinBox,
    QMainWindow, QStatusBar, QToolBar, QAction, QFileDialog, QMessageBox
)
from PyQt5.QtCore import QDir, Qt
from PyQt5.QtGui import QIcon, QColor
import signal
import time

# Import daoLaunch for process management
try:
    from daoLaunch import manage_process
except ImportError:
    print("Warning: daoLaunch not found. Launch functionality will not work.")
    manage_process = None


def launch_senders(config_data):
    """
    Launch daoMudpiSender processes based on configuration data.
    
    Args:
        config_data: Dictionary with 'destination' and 'streams' keys
    """
    if manage_process is None:
        raise ImportError("daoLaunch module not available")
    
    destination_ip = config_data['destination']
    streams = config_data['streams']
    
    print(f"Launching {len(streams)} sender processes...")
    
    for stream in streams:
        file_path = stream['file']
        port = stream['port']
        subsample = stream['subsample']
        semaphore = stream['semaphore']
        cpu_core = stream.get('cpu_core')
        
        # Convert cpu_core to string if it's an int, handle None
        if cpu_core is None:
            cpu_core = ''
        else:
            cpu_core = str(cpu_core).strip()
        
        # Extract base name for tmux session
        filename = os.path.basename(file_path)
        base_name = filename.replace('.im.shm', '')
        tmux_name = f"Telem_Send_{base_name}"
        
        # Build command with optional taskset
        if cpu_core:
            # Use taskset to pin to specific CPU core
            process_exe = 'taskset'
            process_args = f"-c {cpu_core} daoMudpiSender -L {file_path} {destination_ip} {port} {semaphore} -s {subsample}"
            print(f"  Launching {tmux_name}: taskset -c {cpu_core} daoMudpiSender -L {file_path} {destination_ip} {port} {semaphore} -s {subsample}")
        else:
            # No CPU affinity
            process_exe = 'daoMudpiSender'
            process_args = f"-L {file_path} {destination_ip} {port} {semaphore} -s {subsample}"
            print(f"  Launching {tmux_name}: daoMudpiSender {process_args}")
        
        # Launch the sender process
        manage_process(
            action='launch',
            tmuxname=tmux_name,
            user=None,
            machine=None,
            processExe=process_exe,
            processArgs=process_args,
            workingDir='.'
        )
        
        # Brief delay between launches
        time.sleep(0.1)
    
    print("All senders launched successfully!")


def launch_from_config_file(config_file):
    """
    Launch senders from a YAML configuration file (command-line mode).
    
    Args:
        config_file: Path to YAML configuration file
    """
    print(f"Loading configuration from: {config_file}")
    
    try:
        with open(config_file, 'r') as f:
            config_data = yaml.safe_load(f)
        
        # Validate configuration
        if 'destination' not in config_data:
            raise ValueError("Configuration missing 'destination' field")
        if 'streams' not in config_data:
            raise ValueError("Configuration missing 'streams' field")
        
        # Launch senders
        launch_senders(config_data)
        
        print("\nAll processes launched. Use 'tmux ls' to see running sessions.")
        print("To attach to a session: tmux attach -t <session_name>")
        
    except FileNotFoundError:
        print(f"Error: Configuration file not found: {config_file}")
        sys.exit(1)
    except yaml.YAMLError as e:
        print(f"Error parsing YAML file: {e}")
        sys.exit(1)
    except Exception as e:
        print(f"Error launching senders: {e}")
        sys.exit(1)
import time


class daoShmTelemetryConfigurator(QMainWindow):
    """Main application window for configuring SHM telemetry streaming."""
    
    def __init__(self):
        super().__init__()
        self.setWindowTitle("DAO SHM Telemetry Configurator")
        self.init_variables()
        self.init_ui()
        self.setup_connections()
        self.resize(800, 600)

    def init_variables(self):
        """Initialize class variables."""
        self.next_port = 8000
        self.selected_files = {}  # Dictionary to store {filename: {port: int, subsample: int, semaphore: int, cpu_core: str}}
        
        # Directory to monitor
        self.dir = QDir("/tmp")
        self.filters = ["*.im.shm"]
        self.file_location_prefix = "/tmp/"

    def init_ui(self):
        """Initialize the user interface."""
        # Central widget and main layout
        central_widget = QWidget()
        self.setCentralWidget(central_widget)
        main_layout = QVBoxLayout(central_widget)
        
        # Create status bar
        self.statusBar = QStatusBar()
        self.setStatusBar(self.statusBar)
        
        # Create toolbar
        self.setup_toolbar()
        
        # Setup control buttons (create widgets first, add to layout later)
        self.setup_control_buttons()
        
        # Setup file table
        self.setup_file_table()
        main_layout.addWidget(self.file_browser_widget)
        
        # Add control buttons at bottom
        main_layout.addWidget(self.control_widget)

    def setup_toolbar(self):
        """Setup the application toolbar."""
        toolbar = QToolBar("Main Toolbar")
        self.addToolBar(toolbar)
        
        # Refresh action
        refresh_action = QAction("Refresh", self)
        refresh_action.triggered.connect(self.updateFileList)
        toolbar.addAction(refresh_action)
        
        # Import config action
        import_action = QAction("Import Config", self)
        import_action.triggered.connect(self.import_config)
        toolbar.addAction(import_action)
        
        # Clear selection action
        clear_action = QAction("Clear All", self)
        clear_action.triggered.connect(self.clear_all_selections)
        toolbar.addAction(clear_action)

    def setup_file_table(self):
        """Setup the file table widget."""
        # Create a container widget for the file browser section
        file_browser_widget = QWidget()
        file_browser_layout = QVBoxLayout(file_browser_widget)
        file_browser_layout.setContentsMargins(0, 0, 0, 0)
        
        # Add destination IP input
        ip_layout = QHBoxLayout()
        ip_label = QLabel("Destination IP:")
        self.destination_ip_edit = QLineEdit()
        self.destination_ip_edit.setPlaceholderText("Enter destination IP (e.g., 192.168.1.100)")
        self.destination_ip_edit.setText("127.0.0.1")  # Default to localhost
        self.destination_ip_edit.textChanged.connect(self.validate_ip_address)
        ip_layout.addWidget(ip_label)
        ip_layout.addWidget(self.destination_ip_edit)
        file_browser_layout.addLayout(ip_layout)
        
        # Add search filter
        self.search_filter = QLineEdit()
        self.search_filter.setPlaceholderText("Search files...")
        self.search_filter.textChanged.connect(self.filter_files)
        file_browser_layout.addWidget(self.search_filter)
        
        # Create table widget with 6 columns: Select, Filename, Port, Subsample Rate, Semaphore, CPU Core
        self.tableWidget = QTableWidget(self)
        self.tableWidget.setColumnCount(6)
        self.tableWidget.setHorizontalHeaderLabels(["Select", "Filename", "Port", "Subsample Rate", "Semaphore", "CPU Core"])
        self.tableWidget.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeToContents)
        self.tableWidget.horizontalHeader().setSectionResizeMode(1, QHeaderView.Stretch)
        self.tableWidget.horizontalHeader().setSectionResizeMode(2, QHeaderView.ResizeToContents)
        self.tableWidget.horizontalHeader().setSectionResizeMode(3, QHeaderView.ResizeToContents)
        self.tableWidget.horizontalHeader().setSectionResizeMode(4, QHeaderView.ResizeToContents)
        self.tableWidget.horizontalHeader().setSectionResizeMode(5, QHeaderView.ResizeToContents)
        self.tableWidget.setSelectionBehavior(QTableWidget.SelectRows)
        
        # Add table to layout
        file_browser_layout.addWidget(self.tableWidget)
        
        # Store reference to the container widget
        self.file_browser_widget = file_browser_widget
        
        # Initial file listing
        self.updateFileList()

    def setup_control_buttons(self):
        """Setup control buttons at the bottom of the window."""
        control_widget = QWidget()
        control_layout = QHBoxLayout(control_widget)
        
        # Selected files count label
        self.selected_count_label = QLabel("Selected files: 0")
        control_layout.addWidget(self.selected_count_label)
        
        control_layout.addStretch()
        
        # Generate config button
        self.generate_config_button = QPushButton("Generate Config")
        self.generate_config_button.clicked.connect(self.generate_config)
        control_layout.addWidget(self.generate_config_button)
        
        # Launch button
        self.launch_button = QPushButton("Launch")
        self.launch_button.clicked.connect(self.launch)
        control_layout.addWidget(self.launch_button)
        
        self.control_widget = control_widget

    def setup_connections(self):
        """Setup signal-slot connections."""
        pass

    def updateFileList(self):
        """Update the list of shared memory files."""
        # Get updated file list
        fileList = self.dir.entryInfoList(self.filters, QDir.Files)
        self.tableWidget.setRowCount(len(fileList))
        
        # Populate table
        for row, fileInfo in enumerate(fileList):
            filename = fileInfo.fileName()
            
            # Create checkbox
            checkBox = QCheckBox(self)
            if filename in self.selected_files:
                checkBox.setChecked(True)
            # Connect checkbox state change to handler
            checkBox.stateChanged.connect(lambda state, r=row: self.on_checkbox_changed(r, state))
            
            # Create filename item
            filenameItem = QTableWidgetItem(filename)
            filenameItem.setFlags(filenameItem.flags() & ~Qt.ItemIsEditable)  # Make read-only
            
            # Create port spinbox
            portSpinBox = QSpinBox(self)
            portSpinBox.setRange(1024, 65535)
            if filename in self.selected_files:
                portSpinBox.setValue(self.selected_files[filename]['port'])
            else:
                portSpinBox.setValue(8000)
            portSpinBox.valueChanged.connect(lambda value, r=row: self.on_port_changed(r, value))
            
            # Create subsample rate spinbox
            subsampleSpinBox = QSpinBox(self)
            subsampleSpinBox.setRange(1, 1000)
            if filename in self.selected_files:
                subsampleSpinBox.setValue(self.selected_files[filename]['subsample'])
            else:
                subsampleSpinBox.setValue(1)
            subsampleSpinBox.valueChanged.connect(lambda value, r=row: self.on_subsample_changed(r, value))
            
            # Create semaphore spinbox
            semaphoreSpinBox = QSpinBox(self)
            semaphoreSpinBox.setRange(1, 10)
            if filename in self.selected_files:
                semaphoreSpinBox.setValue(self.selected_files[filename]['semaphore'])
            else:
                semaphoreSpinBox.setValue(2)
            semaphoreSpinBox.valueChanged.connect(lambda value, r=row: self.on_semaphore_changed(r, value))
            
            # Create CPU core line edit
            cpuCoreEdit = QLineEdit(self)
            cpuCoreEdit.setPlaceholderText("Optional")
            if filename in self.selected_files:
                cpuCoreEdit.setText(self.selected_files[filename].get('cpu_core', ''))
            cpuCoreEdit.textChanged.connect(lambda text, r=row: self.on_cpu_core_changed(r, text))
            
            # Add items to table
            self.tableWidget.setCellWidget(row, 0, checkBox)
            self.tableWidget.setItem(row, 1, filenameItem)
            self.tableWidget.setCellWidget(row, 2, portSpinBox)
            self.tableWidget.setCellWidget(row, 3, subsampleSpinBox)
            self.tableWidget.setCellWidget(row, 4, semaphoreSpinBox)
            self.tableWidget.setCellWidget(row, 5, cpuCoreEdit)
        
        # Apply current search filter
        self.filter_files()
        
        # Update selected count
        self.update_selected_count()

    def on_checkbox_changed(self, row, state):
        """Handle checkbox state change."""
        filenameItem = self.tableWidget.item(row, 1)
        if not filenameItem:
            return
            
        filename = filenameItem.text()
        portSpinBox = self.tableWidget.cellWidget(row, 2)
        subsampleSpinBox = self.tableWidget.cellWidget(row, 3)
        
        if state == Qt.Checked:
            # File was selected - assign next available port
            port = portSpinBox.value() if filename in self.selected_files else self.next_port
            subsample = subsampleSpinBox.value() if filename in self.selected_files else 1
            semaphoreSpinBox = self.tableWidget.cellWidget(row, 4)
            semaphore = semaphoreSpinBox.value() if filename in self.selected_files else 2
            cpuCoreEdit = self.tableWidget.cellWidget(row, 5)
            cpu_core = cpuCoreEdit.text().strip() if cpuCoreEdit else ''  # Always read current text from widget
            
            self.selected_files[filename] = {
                'port': port,
                'subsample': subsample,
                'semaphore': semaphore,
                'cpu_core': cpu_core
            }
            
            # Update spinboxes to show assigned values
            portSpinBox.setValue(port)
            subsampleSpinBox.setValue(subsample)
            semaphoreSpinBox.setValue(semaphore)
            cpuCoreEdit.setText(cpu_core)
            
            # Increment next port for next selection
            if filename not in self.selected_files or port == self.next_port:
                self.next_port = port + 1
                
            cpu_msg = f" on CPU {cpu_core}" if cpu_core else ""
            self.statusBar.showMessage(f"Selected {filename} on port {port} with subsample rate {subsample} and semaphore {semaphore}{cpu_msg}")
        else:
            # File was deselected
            if filename in self.selected_files:
                del self.selected_files[filename]
            self.statusBar.showMessage(f"Deselected {filename}")
        
        self.update_selected_count()
        self.check_port_duplicates()

    def on_port_changed(self, row, value):
        """Handle port number change."""
        filenameItem = self.tableWidget.item(row, 1)
        if not filenameItem:
            return
            
        filename = filenameItem.text()
        checkBox = self.tableWidget.cellWidget(row, 0)
        
        if checkBox and checkBox.isChecked():
            if filename in self.selected_files:
                self.selected_files[filename]['port'] = value
                self.statusBar.showMessage(f"Updated port for {filename} to {value}")
        self.check_port_duplicates()

    def on_subsample_changed(self, row, value):
        """Handle subsample rate change."""
        filenameItem = self.tableWidget.item(row, 1)
        if not filenameItem:
            return
            
        filename = filenameItem.text()
        checkBox = self.tableWidget.cellWidget(row, 0)
        
        if checkBox and checkBox.isChecked():
            if filename in self.selected_files:
                self.selected_files[filename]['subsample'] = value
                self.statusBar.showMessage(f"Updated subsample rate for {filename} to {value}")

    def on_semaphore_changed(self, row, value):
        """Handle semaphore change."""
        filenameItem = self.tableWidget.item(row, 1)
        if not filenameItem:
            return
            
        filename = filenameItem.text()
        checkBox = self.tableWidget.cellWidget(row, 0)
        
        if checkBox and checkBox.isChecked():
            if filename in self.selected_files:
                self.selected_files[filename]['semaphore'] = value
                self.statusBar.showMessage(f"Updated semaphore for {filename} to {value}")

    def on_cpu_core_changed(self, row, text):
        """Handle CPU core change."""
        filenameItem = self.tableWidget.item(row, 1)
        if not filenameItem:
            return
            
        filename = filenameItem.text()
        checkBox = self.tableWidget.cellWidget(row, 0)
        
        if checkBox and checkBox.isChecked():
            if filename in self.selected_files:
                self.selected_files[filename]['cpu_core'] = text.strip()
                cpu_msg = f" (CPU {text.strip()})" if text.strip() else " (no CPU affinity)"
                self.statusBar.showMessage(f"Updated CPU core for {filename}{cpu_msg}")

    def filter_files(self):
        """Filter the file table based on search text."""
        if not hasattr(self, 'search_filter') or self.search_filter is None:
            return
            
        search_text = self.search_filter.text().lower()
        
        for row in range(self.tableWidget.rowCount()):
            filenameItem = self.tableWidget.item(row, 1)
            if filenameItem:
                filename = filenameItem.text().lower()
                show_row = not search_text or search_text in filename
                self.tableWidget.setRowHidden(row, not show_row)

    def update_selected_count(self):
        """Update the selected files count label."""
        count = len(self.selected_files)
        self.selected_count_label.setText(f"Selected files: {count}")

    def validate_ip_address(self):
        """Validate the IP address format and highlight if invalid."""
        ip_text = self.destination_ip_edit.text().strip()
        
        # IPv4 pattern
        ipv4_pattern = r'^((25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.){3}(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)$'
        
        if re.match(ipv4_pattern, ip_text):
            # Valid IP - remove highlight
            self.destination_ip_edit.setStyleSheet("")
        else:
            # Invalid IP - highlight in red
            self.destination_ip_edit.setStyleSheet("background-color: #ffcccc;")

    def check_port_duplicates(self):
        """Check for duplicate ports and highlight rows."""
        port_to_rows = {}
        
        # Collect all ports and their rows (only for checked items)
        for row in range(self.tableWidget.rowCount()):
            checkBox = self.tableWidget.cellWidget(row, 0)
            if checkBox and checkBox.isChecked():
                portSpinBox = self.tableWidget.cellWidget(row, 2)
                if portSpinBox:
                    port = portSpinBox.value()
                    if port not in port_to_rows:
                        port_to_rows[port] = []
                    port_to_rows[port].append(row)
        
        # Determine which rows have duplicate ports
        duplicate_rows = set()
        for port, rows in port_to_rows.items():
            if len(rows) > 1:
                duplicate_rows.update(rows)
        
        # Apply highlighting only to rows with duplicates
        for row in range(self.tableWidget.rowCount()):
            filenameItem = self.tableWidget.item(row, 1)
            if filenameItem:
                if row in duplicate_rows:
                    # Duplicate port - highlight filename in yellow with black text
                    filenameItem.setBackground(QColor(255, 255, 150))
                    filenameItem.setForeground(QColor(0, 0, 0))
                else:
                    # No duplicate - clear any background/foreground colors
                    filenameItem.setData(Qt.BackgroundRole, None)
                    filenameItem.setData(Qt.ForegroundRole, None)

    def clear_all_selections(self):
        """Clear all file selections."""
        self.selected_files.clear()
        self.next_port = 8000
        
        # Uncheck all checkboxes
        for row in range(self.tableWidget.rowCount()):
            checkBox = self.tableWidget.cellWidget(row, 0)
            if checkBox:
                checkBox.setChecked(False)
        
        self.update_selected_count()
        self.statusBar.showMessage("Cleared all selections")

    def import_config(self):
        """Import configuration from a YAML file."""
        config_file, _ = QFileDialog.getOpenFileName(
            self,
            "Import Configuration",
            "",
            "YAML Files (*.yaml *.yml);;All Files (*)",
            options=QFileDialog.DontUseNativeDialog
        )
        
        if not config_file:
            return
        
        try:
            with open(config_file, 'r') as f:
                config_data = yaml.safe_load(f)
            
            # Validate configuration
            if 'destination' not in config_data:
                self.show_error("Configuration missing 'destination' field")
                return
            if 'streams' not in config_data:
                self.show_error("Configuration missing 'streams' field")
                return
            
            # Clear current selections
            self.clear_all_selections()
            
            # Set destination IP
            self.destination_ip_edit.setText(config_data['destination'])
            
            # Process each stream in the config
            missing_files = []
            loaded_files = []
            
            for stream in config_data['streams']:
                file_path = stream.get('file', '')
                port = stream.get('port', 8000)
                subsample = stream.get('subsample', 1)
                semaphore = stream.get('semaphore', 2)
                
                # Extract filename from full path
                filename = os.path.basename(file_path)
                
                # Find the row for this file in the table
                found = False
                for row in range(self.tableWidget.rowCount()):
                    filenameItem = self.tableWidget.item(row, 1)
                    if filenameItem and filenameItem.text() == filename:
                        found = True
                        # Found the file - set its configuration
                        checkBox = self.tableWidget.cellWidget(row, 0)
                        portSpinBox = self.tableWidget.cellWidget(row, 2)
                        subsampleSpinBox = self.tableWidget.cellWidget(row, 3)
                        semaphoreSpinBox = self.tableWidget.cellWidget(row, 4)
                        cpuCoreEdit = self.tableWidget.cellWidget(row, 5)
                        
                        if portSpinBox:
                            portSpinBox.setValue(port)
                        if subsampleSpinBox:
                            subsampleSpinBox.setValue(subsample)
                        if semaphoreSpinBox:
                            semaphoreSpinBox.setValue(semaphore)
                        if cpuCoreEdit:
                            cpu_core = stream.get('cpu_core', '')
                            # Convert to string for display (handle int, None, or string)
                            if cpu_core is None:
                                cpu_core = ''
                            else:
                                cpu_core = str(cpu_core)
                            cpuCoreEdit.setText(cpu_core)
                        
                        # Check the box last (this will trigger on_checkbox_changed)
                        if checkBox:
                            checkBox.setChecked(True)
                        
                        loaded_files.append(filename)
                        break
                
                if not found:
                    # File not found in table
                    missing_files.append(filename)
            
            # Show warning if files are missing
            if missing_files:
                missing_list = "\n".join(f"  - {f}" for f in missing_files)
                message = (
                    f"The following files from the configuration were not found in {self.file_location_prefix}:\n\n"
                    f"{missing_list}\n\n"
                    f"Loaded {len(loaded_files)} of {len(config_data['streams'])} files successfully."
                )
                
                msg_box = QMessageBox(self)
                msg_box.setIcon(QMessageBox.Warning)
                msg_box.setWindowTitle("Missing Files")
                msg_box.setText("Some files from the configuration were not found.")
                msg_box.setDetailedText(message)
                msg_box.setStandardButtons(QMessageBox.Ok)
                msg_box.exec_()
                
                self.statusBar.showMessage(f"Imported with warnings: {len(missing_files)} files not found")
                print(f"Warning: {len(missing_files)} files from config not found in {self.file_location_prefix}")
                for f in missing_files:
                    print(f"  - {f}")
            else:
                self.statusBar.showMessage(f"Successfully imported configuration from {config_file}")
                print(f"Loaded configuration: {len(self.selected_files)} files")
            
        except FileNotFoundError:
            self.show_error(f"Configuration file not found: {config_file}")
        except yaml.YAMLError as e:
            self.show_error(f"Error parsing YAML file: {e}")
        except Exception as e:
            self.show_error(f"Error importing configuration: {e}")

    def generate_config(self):
        """Generate configuration YAML files for the selected files."""
        if not self.selected_files:
            self.statusBar.showMessage("No files selected")
            return
        
        destination_ip = self.destination_ip_edit.text().strip()
        if not destination_ip:
            self.statusBar.showMessage("Please enter a destination IP address")
            return
        
        # Validate IP address
        ipv4_pattern = r'^((25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.){3}(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)$'
        if not re.match(ipv4_pattern, destination_ip):
            self.statusBar.showMessage("Invalid IP address format")
            return
        
        # Check for duplicate ports
        ports = [config['port'] for config in self.selected_files.values()]
        if len(ports) != len(set(ports)):
            self.statusBar.showMessage("Error: Duplicate port numbers detected")
            return
        
        # Create local config file dialog
        local_config_file, _ = QFileDialog.getSaveFileName(
            self,
            "Save Local Config",
            "local_config.yaml",
            "YAML Files (*.yaml *.yml)",
            options=QFileDialog.DontUseNativeDialog
        )
        
        if not local_config_file:
            return
        
        # Determine remote script filename
        remote_script_file = local_config_file.replace('.yaml', '_remote.py').replace('.yml', '_remote.py')
        
        # First, we need to get the shape and dtype information for each file
        # We'll read this from the actual shared memory files
        import dao
        file_info = {}
        
        for filename in self.selected_files.keys():
            try:
                shm_path = self.file_location_prefix + filename
                temp_shm = dao.shm(shm_path, logLevel=0)
                data = temp_shm.get_data()
                file_info[filename] = {
                    'shape': data.shape,
                    'dtype': str(data.dtype)
                }
                temp_shm.close()
            except Exception as e:
                self.statusBar.showMessage(f"Error reading {filename}: {e}")
                return
        
        # Build configuration structure
        config_data = {
            'destination': destination_ip,
            'streams': []
        }
        
        for filename, config in self.selected_files.items():
            # Convert cpu_core to int if it's a number, otherwise keep as string (for ranges like "0-3")
            cpu_core = config.get('cpu_core', '')
            if cpu_core and cpu_core.isdigit():
                cpu_core = int(cpu_core)
            elif not cpu_core:
                cpu_core = None  # Use None instead of empty string for cleaner YAML
            
            stream_entry = {
                'file': self.file_location_prefix + filename,
                'port': config['port'],
                'subsample': config['subsample'],
                'semaphore': config['semaphore'],
                'cpu_core': cpu_core
            }
            config_data['streams'].append(stream_entry)
        
        # Write local config file
        try:
            with open(local_config_file, 'w') as f:
                yaml.dump(config_data, f, default_flow_style=False, sort_keys=False)
            
            self.statusBar.showMessage(f"Configuration saved to {local_config_file}")
            print(f"Local configuration saved to: {local_config_file}")
            
        except Exception as e:
            self.statusBar.showMessage(f"Error saving configuration: {e}")
            print(f"Error: {e}")
            return
        
        # Generate remote Python script
        try:
            self.generate_remote_script(remote_script_file, file_info, destination_ip)
            self.statusBar.showMessage(f"Config and remote script saved successfully")
            print(f"Remote script saved to: {remote_script_file}")
            
        except Exception as e:
            self.statusBar.showMessage(f"Error saving remote script: {e}")
            print(f"Error: {e}")
    
    def generate_remote_script(self, script_path, file_info, destination_ip):
        """Generate the remote Python script to recreate SHM files and launch receivers."""
        
        script_content = f'''#!/usr/bin/env python3
"""
Auto-generated remote telemetry receiver script.
This script creates shared memory files and launches daoMudpiReceiver processes.
"""

import sys
import numpy as np
import dao
from daoLaunch import manage_process
import time

def main():
    print("Creating shared memory files...")
    
    shm_objects = []
    
'''
        
        # Add code to create each shared memory file
        for filename, config in self.selected_files.items():
            info = file_info[filename]
            shape_str = str(info['shape'])
            dtype_str = info['dtype']
            file_path = self.file_location_prefix + filename
            
            # Extract base name without path and extension for tmux session
            base_name = filename.replace('.im.shm', '')
            tmux_name = f"Telem_{base_name}"
            
            port = config['port']
            
            script_content += f'''    # Create {filename}
    print("Creating {file_path}...")
    data_{base_name} = np.zeros({shape_str}, dtype=np.{dtype_str})
    shm_{base_name} = dao.shm("{file_path}", data=data_{base_name})
    shm_objects.append(shm_{base_name})
    print(f"  Created with shape {shape_str} and dtype {dtype_str}")
    
'''
        
        script_content += '''    print("\\nAll shared memory files created successfully!")
    print("\\nLaunching daoMudpiReceiver processes...")
    
'''
        
        # Add code to launch each receiver process
        for filename, config in self.selected_files.items():
            base_name = filename.replace('.im.shm', '')
            tmux_name = f"Telem_{base_name}"
            file_path = self.file_location_prefix + filename
            port = config['port']
            
            script_content += f'''    # Launch receiver for {filename}
    print("Launching receiver for {filename} in tmux session '{tmux_name}'...")
    manage_process(
        action='launch',
        tmuxname='{tmux_name}',
        user=None,
        machine=None,
        processExe='daoMudpiReceiver',
        processArgs='-L {file_path} {destination_ip} {port}',
        workingDir='.'
    )
    time.sleep(0.2)  # Brief delay between launches
    
'''
        
        script_content += '''    print("\\nAll receivers launched successfully!")
    print("\\nReceiver processes:")
'''
        
        for filename, config in self.selected_files.items():
            base_name = filename.replace('.im.shm', '')
            tmux_name = f"Telem_{base_name}"
            file_path = self.file_location_prefix + filename
            port = config['port']
            
            script_content += f'''    print("  - {tmux_name}: {file_path} <- {destination_ip}:{port}")
'''
        
        script_content += '''
    print("\\nTo view a receiver session, use: tmux attach -t <session_name>")
    print("To kill all receivers, run this script with --kill")

def kill_all():
    """Kill all receiver processes."""
    print("Killing all receiver processes...")
    
'''
        
        for filename in self.selected_files.keys():
            base_name = filename.replace('.im.shm', '')
            tmux_name = f"Telem_{base_name}"
            
            script_content += f'''    manage_process(action='kill', tmuxname='{tmux_name}', user=None, machine=None)
'''
        
        script_content += '''
    print("All receivers killed.")

if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == '--kill':
        kill_all()
    else:
        main()
'''
        
        # Write the script file
        with open(script_path, 'w') as f:
            f.write(script_content)
        
        # Make it executable
        os.chmod(script_path, 0o755)

    def launch(self):
        """Launch telemetry streaming."""
        if not self.selected_files:
            self.statusBar.showMessage("No files selected")
            return
        
        destination_ip = self.destination_ip_edit.text().strip()
        if not destination_ip:
            self.statusBar.showMessage("Please enter a destination IP address")
            return
        
        # Validate IP address
        ipv4_pattern = r'^((25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.){3}(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)$'
        if not re.match(ipv4_pattern, destination_ip):
            self.statusBar.showMessage("Invalid IP address format")
            return
        
        # Check for duplicate ports
        ports = [config['port'] for config in self.selected_files.values()]
        if len(ports) != len(set(ports)):
            self.statusBar.showMessage("Error: Duplicate port numbers detected")
            return
        
        # Build config data structure
        config_data = {
            'destination': destination_ip,
            'streams': []
        }
        
        for filename, config in self.selected_files.items():
            # Convert cpu_core to int if it's a number, otherwise keep as string (for ranges like "0-3")
            cpu_core = config.get('cpu_core', '')
            if cpu_core and cpu_core.isdigit():
                cpu_core = int(cpu_core)
            elif not cpu_core:
                cpu_core = None
            
            stream_entry = {
                'file': self.file_location_prefix + filename,
                'port': config['port'],
                'subsample': config['subsample'],
                'semaphore': config['semaphore'],
                'cpu_core': cpu_core
            }
            config_data['streams'].append(stream_entry)
        
        # Launch senders
        self.statusBar.showMessage(f"Launching telemetry senders for {len(self.selected_files)} files to {destination_ip}...")
        
        try:
            launch_senders(config_data)
            self.statusBar.showMessage(f"Successfully launched {len(self.selected_files)} sender processes")
            
            # Print configuration for debugging
            print("Launched senders with configuration:")
            print(f"  Destination IP: {destination_ip}")
            for filename, config in self.selected_files.items():
                base_name = filename.replace('.im.shm', '')
                tmux_name = f"Telem_Send_{base_name}"
                cpu_msg = f" on CPU {config['cpu_core']}" if config.get('cpu_core') else ""
                print(f"  - {tmux_name}: {filename} -> {destination_ip}:{config['port']} (subsample={config['subsample']}, semaphore={config['semaphore']}){cpu_msg}")
        except Exception as e:
            self.statusBar.showMessage(f"Error launching senders: {e}")
            print(f"Error: {e}")

    def show_error(self, message):
        """Display error message in status bar."""
        self.statusBar.showMessage(f"Error: {message}")


def main():
    """Main application entry point."""
    
    # Check if running in command-line mode (config file provided)
    if len(sys.argv) > 1:
        config_file = sys.argv[1]
        # Command-line mode: launch senders from config file
        launch_from_config_file(config_file)
        return
    
    # GUI mode
    app = QApplication(sys.argv)
    app.setApplicationDisplayName("DAO SHM Telemetry Configurator")
    app.setOrganizationName("DAO")
    
    # Set application icon if available
    dao_root = os.getenv('DAOROOT')
    if dao_root:
        icon_path = os.path.join(dao_root, 'data/daoLogo.png')
        if os.path.exists(icon_path):
            app.setWindowIcon(QIcon(icon_path))
    
    # Handle Ctrl+C keyboard interrupt
    signal.signal(signal.SIGINT, signal.SIG_DFL)
    
    # Create and show main window
    viewer = daoShmTelemetryConfigurator()
    viewer.show()
    
    sys.exit(app.exec_())


if __name__ == "__main__":
    main()
