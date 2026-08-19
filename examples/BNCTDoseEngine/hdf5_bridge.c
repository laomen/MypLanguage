// hdf5_bridge.c — MYP FFI bridge for HDF5 cross-section reading
#include "mylang/runtime.h"
#include "/usr/include/hdf5/serial/hdf5.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// HDF5 uses int64_t for hid_t on 64-bit systems.
// Must use int64_t for file handles to avoid truncation.

// Open an HDF5 file, return file handle (int64_t, or -1 on error)
int64_t myp_h5_open(const char* path) {
    hid_t file = H5Fopen(path, H5F_ACC_RDONLY, H5P_DEFAULT);
    return file >= 0 ? (int64_t)file : -1;
}

// Close an HDF5 file
void myp_h5_close(int64_t file_id) {
    if (file_id >= 0) H5Fclose((hid_t)file_id);
}

// Get the size (number of elements) of a 1D double dataset
int64_t myp_h5_dataset_size(int64_t file_id, const char* path) {
    hid_t file = (hid_t)file_id;
    hid_t dset = H5Dopen2(file, path, H5P_DEFAULT);
    if (dset < 0) return -1;
    hid_t space = H5Dget_space(dset);
    hsize_t dims[1];
    int ndim = H5Sget_simple_extent_dims(space, dims, NULL);
    H5Sclose(space);
    H5Dclose(dset);
    return ndim == 1 ? (int64_t)dims[0] : -1;
}

// Read a 1D double dataset into a pre-allocated buffer
// Returns number of elements read, or -1 on error
int64_t myp_h5_read_double(int64_t file_id, const char* path,
                            double* buffer, int64_t size) {
    hid_t file = (hid_t)file_id;
    hid_t dset = H5Dopen2(file, path, H5P_DEFAULT);
    if (dset < 0) return -1;
    hid_t space = H5Dget_space(dset);
    hsize_t dims[1];
    H5Sget_simple_extent_dims(space, dims, NULL);
    int64_t count = (int64_t)dims[0];
    if (count > size) count = size;
    herr_t status = H5Dread(dset, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL,
                            H5P_DEFAULT, buffer);
    H5Sclose(space);
    H5Dclose(dset);
    return status >= 0 ? count : -1;
}
