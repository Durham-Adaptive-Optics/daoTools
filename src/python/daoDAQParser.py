#!/usr/bin/env python3
"""
daoDAQ Output Parser

Parses FITS files created by daoDAQ into structured Python dictionaries.

Author: Generated for daoTools
Description: Parse daoDAQ session output directories containing FITS files
             with multiple HDUs, extracting both image data and metadata.
"""

import os
import re
from pathlib import Path
from typing import Dict, List, Optional, Union, Any
from collections import defaultdict
from dataclasses import dataclass, field
from datetime import datetime

try:
    from astropy.io import fits
    import numpy as np
except ImportError:
    raise ImportError(
        "Required dependencies not found. Install with:\n"
        "  pip install astropy numpy"
    )


@dataclass
class SampleMetadata:
    """Metadata for a single acquired sample/frame."""
    atype: int  # Data type code
    atime: int  # Timestamp in nanoseconds
    cnt0: int   # Counter 0
    cnt1: int   # Counter 1
    cnt2: int   # Counter 2
    hdu_index: int  # Index of HDU in FITS file
    
    def __repr__(self):
        return (f"SampleMetadata(atime={self.atime}, "
                f"cnt0={self.cnt0}, cnt1={self.cnt1}, cnt2={self.cnt2})")


@dataclass
class Sample:
    """A single acquired sample with data and metadata."""
    data: np.ndarray
    metadata: SampleMetadata
    file_path: str
    
    def __repr__(self):
        return (f"Sample(shape={self.data.shape}, dtype={self.data.dtype}, "
                f"metadata={self.metadata})")


@dataclass
class Resource:
    """A single data acquisition resource."""
    name: str
    samples: List[Sample] = field(default_factory=list)
    file_paths: List[str] = field(default_factory=list)
    
    @property
    def num_samples(self) -> int:
        """Total number of samples in this resource."""
        return len(self.samples)
    
    @property
    def shape(self) -> Optional[tuple]:
        """Shape of sample data (if samples exist)."""
        if self.samples:
            return self.samples[0].data.shape
        return None
    
    @property
    def dtype(self) -> Optional[np.dtype]:
        """Data type of samples (if samples exist)."""
        if self.samples:
            return self.samples[0].data.dtype
        return None
    
    def get_all_data(self) -> np.ndarray:
        """
        Get all sample data as a single array.
        
        Returns:
            Array with shape (n_samples, *sample_shape)
        """
        if not self.samples:
            return np.array([])
        return np.array([s.data for s in self.samples])
    
    def get_all_metadata(self) -> List[SampleMetadata]:
        """Get list of all sample metadata."""
        return [s.metadata for s in self.samples]
    
    def get_timestamps(self) -> np.ndarray:
        """Get array of all sample timestamps (atime)."""
        return np.array([s.metadata.atime for s in self.samples])
    
    def get_counters(self) -> Dict[str, np.ndarray]:
        """Get dictionary of counter arrays."""
        return {
            'cnt0': np.array([s.metadata.cnt0 for s in self.samples]),
            'cnt1': np.array([s.metadata.cnt1 for s in self.samples]),
            'cnt2': np.array([s.metadata.cnt2 for s in self.samples]),
        }
    
    def __repr__(self):
        return (f"Resource(name='{self.name}', num_samples={self.num_samples}, "
                f"shape={self.shape}, dtype={self.dtype})")


@dataclass
class Session:
    """A daoDAQ acquisition session."""
    session_dir: str
    timestamp: str
    resources: Dict[str, Resource] = field(default_factory=dict)
    
    @property
    def num_resources(self) -> int:
        """Number of resources in this session."""
        return len(self.resources)
    
    @property
    def resource_names(self) -> List[str]:
        """List of resource names."""
        return list(self.resources.keys())
    
    def __repr__(self):
        return (f"Session(timestamp='{self.timestamp}', "
                f"num_resources={self.num_resources}, "
                f"resources={self.resource_names})")


class DAODAQParser:
    """
    Parser for daoDAQ output files.
    
    Parses session directories containing FITS files with daoDAQ metadata.
    """
    
    # Regex pattern for session directory names (YYYY-MM-DD_HH-MM-SS)
    SESSION_DIR_PATTERN = re.compile(r'^\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2}$')
    
    # Regex pattern for FITS files with rollover (localName_N.fits)
    ROLLOVER_FILE_PATTERN = re.compile(r'^(.+)_(\d+)\.fits$')
    
    # Regex pattern for single FITS files (localName.fits)
    SINGLE_FILE_PATTERN = re.compile(r'^(.+)\.fits$')
    
    def __init__(self, root_storage: Union[str, Path]):
        """
        Initialize parser with root storage directory.
        
        Args:
            root_storage: Path to daoDAQ root storage directory
        """
        self.root_storage = Path(root_storage)
        if not self.root_storage.exists():
            raise FileNotFoundError(f"Root storage directory not found: {root_storage}")
        if not self.root_storage.is_dir():
            raise NotADirectoryError(f"Not a directory: {root_storage}")
    
    @staticmethod
    def parse_directory(directory_path: Union[str, Path]) -> Session:
        """
        Parse a specific directory directly without needing root_storage.
        
        This static method allows you to parse any directory containing FITS files
        without initializing the parser with a root storage path. Useful for parsing
        individual session directories or arbitrary directories with daoDAQ output.
        
        Args:
            directory_path: Path to directory containing FITS files
            
        Returns:
            Session object containing all resources found in the directory
            
        Example:
            # Parse a specific session directory directly
            session = DAODAQParser.parse_directory('/path/to/2026-05-26_10-30-00')
            
            # Parse any directory with FITS files
            session = DAODAQParser.parse_directory('/custom/output/directory')
        """
        directory_path = Path(directory_path)
        if not directory_path.exists():
            raise FileNotFoundError(f"Directory not found: {directory_path}")
        if not directory_path.is_dir():
            raise NotADirectoryError(f"Not a directory: {directory_path}")
        
        # Create a temporary parser instance for the helper methods
        temp_parser = object.__new__(DAODAQParser)
        
        session = Session(
            session_dir=str(directory_path),
            timestamp=directory_path.name
        )
        
        # Find all resources in the directory
        # Resources can be:
        # 1. Single FITS files in directory root: localName.fits
        # 2. Subdirectories with rollover files: localName/localName_N.fits
        
        for item in directory_path.iterdir():
            if item.is_file() and item.suffix == '.fits':
                # Single FITS file
                resource = temp_parser.parse_resource(item)
                session.resources[resource.name] = resource
            
            elif item.is_dir():
                # Check if this is a resource directory (contains rollover FITS files)
                rollover_files = list(item.glob(f"{item.name}_*.fits"))
                if rollover_files:
                    resource = temp_parser.parse_resource(item)
                    session.resources[resource.name] = resource
        
        return session
    
    def parse_fits_file(self, fits_path: Union[str, Path]) -> List[Sample]:
        """
        Parse a single FITS file containing daoDAQ data.
        
        Args:
            fits_path: Path to FITS file
            
        Returns:
            List of Sample objects, one per HDU
        """
        fits_path = Path(fits_path)
        samples = []
        
        with fits.open(fits_path) as hdul:
            # Skip primary HDU (index 0) if it's empty, start from image HDUs
            for idx, hdu in enumerate(hdul):
                if idx == 0 and isinstance(hdu, fits.PrimaryHDU) and hdu.data is None:
                    continue
                
                if hdu.data is None:
                    continue
                
                # Extract metadata from header
                header = hdu.header
                metadata = SampleMetadata(
                    atype=header.get('ATYPE', 0),
                    atime=header.get('ATIME', 0),
                    cnt0=header.get('CNT0', 0),
                    cnt1=header.get('CNT1', 0),
                    cnt2=header.get('CNT2', 0),
                    hdu_index=idx
                )
                
                # Extract image data
                # Note: FITS files store data in Fortran order (column-major)
                # but numpy uses C order (row-major). astropy handles this.
                data = hdu.data.copy()
                
                samples.append(Sample(
                    data=data,
                    metadata=metadata,
                    file_path=str(fits_path)
                ))
        
        return samples
    
    def parse_resource(self, resource_path: Union[str, Path]) -> Resource:
        """
        Parse a single resource (single FITS file or directory with rollover files).
        
        Args:
            resource_path: Path to FITS file or directory containing rollover files
            
        Returns:
            Resource object containing all samples
        """
        resource_path = Path(resource_path)
        
        if resource_path.is_file():
            # Single FITS file
            name = resource_path.stem
            samples = self.parse_fits_file(resource_path)
            resource = Resource(name=name, file_paths=[str(resource_path)])
            resource.samples = samples
            return resource
        
        elif resource_path.is_dir():
            # Directory with rollover files
            name = resource_path.name
            resource = Resource(name=name)
            
            # Find all FITS files matching the rollover pattern
            fits_files = sorted(
                resource_path.glob(f"{name}_*.fits"),
                key=lambda p: int(self.ROLLOVER_FILE_PATTERN.match(p.name).group(2))
            )
            
            for fits_file in fits_files:
                samples = self.parse_fits_file(fits_file)
                resource.samples.extend(samples)
                resource.file_paths.append(str(fits_file))
            
            return resource
        
        else:
            raise ValueError(f"Invalid resource path: {resource_path}")
    
    def parse_session(self, session_dir: Union[str, Path]) -> Session:
        """
        Parse a complete daoDAQ session directory.
        
        Args:
            session_dir: Path to session directory
            
        Returns:
            Session object containing all resources
        """
        session_dir = Path(session_dir)
        if not session_dir.exists():
            raise FileNotFoundError(f"Session directory not found: {session_dir}")
        
        session = Session(
            session_dir=str(session_dir),
            timestamp=session_dir.name
        )
        
        # Find all resources in the session directory
        # Resources can be:
        # 1. Single FITS files in session root: localName.fits
        # 2. Subdirectories with rollover files: localName/localName_N.fits
        
        for item in session_dir.iterdir():
            if item.is_file() and item.suffix == '.fits':
                # Single FITS file
                resource = self.parse_resource(item)
                session.resources[resource.name] = resource
            
            elif item.is_dir():
                # Check if this is a resource directory (contains rollover FITS files)
                rollover_files = list(item.glob(f"{item.name}_*.fits"))
                if rollover_files:
                    resource = self.parse_resource(item)
                    session.resources[resource.name] = resource
        
        return session
    
    def find_sessions(self) -> List[str]:
        """
        Find all session directories in root storage.
        
        Returns:
            List of session directory names (timestamps)
        """
        sessions = []
        for item in self.root_storage.iterdir():
            if item.is_dir() and self.SESSION_DIR_PATTERN.match(item.name):
                sessions.append(item.name)
        return sorted(sessions)
    
    def parse_all_sessions(self) -> Dict[str, Session]:
        """
        Parse all sessions in root storage.
        
        Returns:
            Dictionary mapping session timestamp to Session object
        """
        sessions = {}
        for session_name in self.find_sessions():
            session_path = self.root_storage / session_name
            try:
                session = self.parse_session(session_path)
                sessions[session_name] = session
            except Exception as e:
                print(f"Warning: Failed to parse session {session_name}: {e}")
        
        return sessions
    
    def parse_latest_session(self) -> Optional[Session]:
        """
        Parse the most recent session.
        
        Returns:
            Session object for latest session, or None if no sessions found
        """
        session_names = self.find_sessions()
        if not session_names:
            return None
        
        latest = session_names[-1]
        return self.parse_session(self.root_storage / latest)
    
    def to_dict(self, session: Session, include_data: bool = True) -> Dict[str, Any]:
        """
        Convert a Session object to a dictionary.
        
        Args:
            session: Session object to convert
            include_data: If True, include sample data arrays. If False, only metadata.
            
        Returns:
            Dictionary representation of the session
        """
        result = {
            'session_dir': session.session_dir,
            'timestamp': session.timestamp,
            'num_resources': session.num_resources,
            'resources': {}
        }
        
        for name, resource in session.resources.items():
            resource_dict = {
                'name': resource.name,
                'num_samples': resource.num_samples,
                'shape': resource.shape,
                'dtype': str(resource.dtype) if resource.dtype else None,
                'file_paths': resource.file_paths,
                'samples': []
            }
            
            for sample in resource.samples:
                sample_dict = {
                    'metadata': {
                        'atype': sample.metadata.atype,
                        'atime': sample.metadata.atime,
                        'cnt0': sample.metadata.cnt0,
                        'cnt1': sample.metadata.cnt1,
                        'cnt2': sample.metadata.cnt2,
                        'hdu_index': sample.metadata.hdu_index,
                    },
                    'file_path': sample.file_path,
                }
                
                if include_data:
                    sample_dict['data'] = sample.data
                
                resource_dict['samples'].append(sample_dict)
            
            result['resources'][name] = resource_dict
        
        return result


def main():
    """Example usage of the parser."""
    import argparse
    
    parser = argparse.ArgumentParser(
        description='Parse daoDAQ FITS output files',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # List all sessions
  %(prog)s /path/to/root_storage --list

  # Parse latest session and print summary
  %(prog)s /path/to/root_storage --latest

  # Parse specific session
  %(prog)s /path/to/root_storage --session 2026-05-26_14-30-00

  # Parse all sessions
  %(prog)s /path/to/root_storage --all
        """
    )
    
    parser.add_argument('root_storage', help='Path to daoDAQ root storage directory')
    parser.add_argument('--list', action='store_true', help='List all available sessions')
    parser.add_argument('--latest', action='store_true', help='Parse latest session')
    parser.add_argument('--all', action='store_true', help='Parse all sessions')
    parser.add_argument('--session', help='Parse specific session by timestamp')
    parser.add_argument('--resource', help='Show details for specific resource')
    parser.add_argument('--no-data', action='store_true', help='Exclude data arrays from output')
    
    args = parser.parse_args()
    
    # Initialize parser
    daq_parser = DAODAQParser(args.root_storage)
    
    if args.list:
        # List all sessions
        sessions = daq_parser.find_sessions()
        print(f"Found {len(sessions)} session(s) in {args.root_storage}:")
        for session_name in sessions:
            print(f"  - {session_name}")
    
    elif args.latest:
        # Parse latest session
        session = daq_parser.parse_latest_session()
        if session is None:
            print("No sessions found")
            return
        
        print(f"\nLatest Session: {session.timestamp}")
        print(f"Session Directory: {session.session_dir}")
        print(f"Number of Resources: {session.num_resources}")
        
        for name, resource in session.resources.items():
            print(f"\n  Resource: {name}")
            print(f"    Samples: {resource.num_samples}")
            print(f"    Shape: {resource.shape}")
            print(f"    Dtype: {resource.dtype}")
            print(f"    Files: {len(resource.file_paths)}")
            
            if args.resource and args.resource == name:
                print(f"\n    Sample details:")
                for i, sample in enumerate(resource.samples[:5]):  # Show first 5
                    print(f"      [{i}] {sample.metadata}")
                if resource.num_samples > 5:
                    print(f"      ... and {resource.num_samples - 5} more")
    
    elif args.all:
        # Parse all sessions
        sessions = daq_parser.parse_all_sessions()
        print(f"Parsed {len(sessions)} session(s):")
        
        for timestamp, session in sessions.items():
            print(f"\n  {timestamp}:")
            print(f"    Resources: {session.num_resources}")
            for name, resource in session.resources.items():
                print(f"      - {name}: {resource.num_samples} samples, shape {resource.shape}")
    
    elif args.session:
        # Parse specific session
        session_path = Path(args.root_storage) / args.session
        session = daq_parser.parse_session(session_path)
        
        print(f"Session: {session.timestamp}")
        print(f"Number of Resources: {session.num_resources}")
        
        for name, resource in session.resources.items():
            print(f"\n  Resource: {name}")
            print(f"    Samples: {resource.num_samples}")
            print(f"    Shape: {resource.shape}")
            print(f"    Dtype: {resource.dtype}")
            
            if args.resource and args.resource == name:
                print(f"\n    All samples:")
                for i, sample in enumerate(resource.samples):
                    print(f"      [{i}] {sample.metadata}")
    
    else:
        parser.print_help()


if __name__ == '__main__':
    main()
