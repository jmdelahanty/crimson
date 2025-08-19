#!/usr/bin/env python3
"""
Script to inspect Zarr and H5 file structures to understand the data format
"""

import zarr
import h5py
import numpy as np
import json
import sys
from pathlib import Path

def inspect_zarr(zarr_path):
    """Inspect a Zarr file structure"""
    print(f"\n{'='*60}")
    print(f"ZARR FILE: {zarr_path}")
    print(f"{'='*60}")
    
    try:
        # Open the zarr store
        store = zarr.open(zarr_path, mode='r')
        
        # List all arrays and groups
        print("\nArrays and Groups:")
        for key in store.keys():
            item = store[key]
            if isinstance(item, zarr.Array):
                print(f"  {key}: Array - shape={item.shape}, dtype={item.dtype}, chunks={item.chunks}")
                
                # Show first few values if small enough
                if item.shape[0] > 0:
                    print(f"    First element: {item[0]}")
                    if item.ndim == 1 and item.shape[0] < 20:
                        print(f"    All values: {item[:]}")
            else:
                print(f"  {key}: Group")
        
        # Check for specific arrays we expect
        expected_arrays = ['bboxes', 'scores', 'class_ids', 'n_detections']
        print("\nExpected Arrays:")
        for arr_name in expected_arrays:
            if arr_name in store:
                arr = store[arr_name]
                print(f"  {arr_name}:")
                print(f"    Shape: {arr.shape}")
                print(f"    Dtype: {arr.dtype}")
                print(f"    Chunks: {arr.chunks}")
                print(f"    Fill value: {arr.fill_value}")
                
                # Show sample data
                if arr.shape[0] > 0:
                    print(f"    Frame 0 data: {arr[0]}")
                    
                    # Check for fill values
                    if arr_name == 'bboxes' and arr.shape[0] > 0:
                        # Check if any boxes have -1 (fill value)
                        sample = arr[:min(10, arr.shape[0])]
                        has_fill = np.any(sample == -1.0)
                        print(f"    Has fill values in first 10 frames: {has_fill}")
            else:
                print(f"  {arr_name}: NOT FOUND")
        
        # Check metadata
        if hasattr(store, 'attrs'):
            print("\nRoot Attributes:")
            for key, value in store.attrs.items():
                print(f"  {key}: {value}")
                
    except Exception as e:
        print(f"Error reading Zarr file: {e}")
        import traceback
        traceback.print_exc()

def inspect_h5_bounding_boxes(h5_path):
    """Inspect H5 file bounding box structure"""
    print(f"\n{'='*60}")
    print(f"H5 FILE: {h5_path}")
    print(f"{'='*60}")
    
    try:
        with h5py.File(h5_path, 'r') as f:
            # First, let's see all top-level groups
            print("\nTop-level groups/datasets:")
            for key in f.keys():
                item = f[key]
                if isinstance(item, h5py.Group):
                    print(f"  {key}/ (Group)")
                    # Show subgroups
                    for subkey in item.keys():
                        subitem = item[subkey]
                        if isinstance(subitem, h5py.Group):
                            print(f"    {subkey}/ (Group)")
                        else:
                            print(f"    {subkey} (Dataset: shape={subitem.shape}, dtype={subitem.dtype})")
                else:
                    print(f"  {key} (Dataset: shape={item.shape}, dtype={item.dtype})")
            
            # Look for bounding boxes in various locations
            bbox_paths = [
                '/bounding_boxes', 
                '/analysis/bounding_boxes', 
                '/logged_bounding_boxes',
                '/analysis/logged_bounding_boxes',
                '/session/bounding_boxes',
                '/session/logged_bounding_boxes'
            ]
            
            # Search recursively for anything with "box" in the name
            def find_datasets_with_keyword(group, keyword, path=""):
                results = []
                for key in group.keys():
                    item = group[key]
                    current_path = f"{path}/{key}"
                    if keyword.lower() in key.lower():
                        if isinstance(item, h5py.Dataset):
                            results.append((current_path, item))
                        else:
                            results.append((current_path + "/", item))
                    if isinstance(item, h5py.Group):
                        results.extend(find_datasets_with_keyword(item, keyword, current_path))
                return results
            
            print("\n" + "="*40)
            print("Searching for datasets containing 'box':")
            box_datasets = find_datasets_with_keyword(f, "box")
            
            for path, dataset in box_datasets:
                print(f"\nFound: {path}")
                if isinstance(dataset, h5py.Dataset):
                    print(f"  Shape: {dataset.shape}")
                    print(f"  Dtype: {dataset.dtype}")
                    
                    # For structured arrays, show field names
                    if dataset.dtype.names:
                        print(f"  Field names: {list(dataset.dtype.names)}")
                        
                        # Show first record
                        if dataset.shape[0] > 0:
                            first = dataset[0]
                            print(f"\n  First record:")
                            for field in dataset.dtype.names:
                                value = first[field]
                                # Handle byte strings
                                if isinstance(value, bytes):
                                    value = value.decode('utf-8', errors='ignore')
                                print(f"    {field}: {value} (type: {type(first[field]).__name__})")
                                
                            # Show the dtype in detail
                            print(f"\n  Detailed dtype:")
                            for name in dataset.dtype.names:
                                field_dtype = dataset.dtype.fields[name][0]
                                print(f"    {name}: {field_dtype}")
                    else:
                        # Regular array
                        if dataset.shape[0] > 0:
                            print(f"  First element: {dataset[0]}")
                elif isinstance(dataset, h5py.Group):
                    print(f"  Group with keys: {list(dataset.keys())}")
            
            # Also look for frame metadata
            print("\n" + "="*40)
            print("Searching for datasets containing 'frame':")
            frame_datasets = find_datasets_with_keyword(f, "frame")
            
            for path, dataset in frame_datasets:
                if isinstance(dataset, h5py.Dataset) and dataset.dtype.names:
                    print(f"\nFound: {path}")
                    print(f"  Shape: {dataset.shape}")
                    print(f"  Field names: {list(dataset.dtype.names)}")
                            
    except Exception as e:
        print(f"Error reading H5 file: {e}")
        import traceback
        traceback.print_exc()

def main():
    # Default paths - update these to your actual file paths
    zarr_path = "/home/delahantyj@hhmi.org/Desktop/escape_2/2025-08-12T20-25-51Z_arena_4_chaser_detections.zarr"
    h5_path = "/home/delahantyj@hhmi.org/Desktop/escape_2/out_analysis.h5"
    
    # Allow command line arguments
    if len(sys.argv) > 1:
        zarr_path = sys.argv[1]
    if len(sys.argv) > 2:
        h5_path = sys.argv[2]
    
    # Check if files exist
    if Path(zarr_path).exists():
        inspect_zarr(zarr_path)
    else:
        print(f"Zarr file not found: {zarr_path}")
    
    if Path(h5_path).exists():
        inspect_h5_bounding_boxes(h5_path)
    else:
        print(f"H5 file not found: {h5_path}")
    
    print(f"\n{'='*60}")
    print("SUMMARY")
    print(f"{'='*60}")
    print("\nBased on this inspection, update your C++ code to match the actual field names.")
    print("The LoggedBoundingBox structure in C++ should have the same fields as shown")
    print("in the H5 file's bounding_boxes dtype.")

if __name__ == "__main__":
    main()