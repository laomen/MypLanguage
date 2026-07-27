// myp_stdlib.c — MYP stdlib C helpers
// Thin wrappers around C standard library for MYP FFI bridge.
// These are NOT intrinsics — they're regular C functions callable via FFI.

#include "mylang/runtime.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ---- Dynamic Array Helpers ----

// Allocate an array of n elements, each of size elem_size
void* myp_arr_alloc(int n, int elem_size) {
    return calloc(n, elem_size);
}

// Resize array
void* myp_arr_resize(void* ptr, int new_n, int elem_size) {
    return realloc(ptr, new_n * elem_size);
}

// Free array
void myp_arr_free(void* ptr) {
    free(ptr);
}

// Copy n elements from src to dst
void myp_arr_copy(void* dst, void* src, int n, int elem_size) {
    memcpy(dst, src, n * elem_size);
}

// ---- String Helpers ----

// Get string length
int myp_str_len(const char* s) {
    return (int)strlen(s);
}

// Compare two strings (returns 0 if equal)
int myp_str_cmp(const char* a, const char* b) {
    return strcmp(a, b);
}

// Copy string
char* myp_str_cpy(char* dst, const char* src) {
    return strcpy(dst, src);
}

// Concatenate strings
char* myp_str_cat(char* dst, const char* src) {
    return strcat(dst, src);
}

// Format string (simple wrapper around sprintf)
// Returns the formatted string (allocates new memory — caller must free)
char* myp_str_fmt(const char* fmt, int val) {
    char buf[256];
    snprintf(buf, sizeof(buf), fmt, val);
    char* result = (char*)malloc(strlen(buf) + 1);
    strcpy(result, buf);
    return result;
}

// ---- Utility ----

// Linear search in int array (returns index or -1)
int myp_arr_find_int(int* arr, int n, int val) {
    for (int i = 0; i < n; i++) {
        if (arr[i] == val) return i;
    }
    return -1;
}

// Quick sort wrapper for int arrays
void myp_arr_sort_int(int* arr, int n) {
    // Simple bubble sort for now (qsort later if needed)
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - i - 1; j++) {
            if (arr[j] > arr[j + 1]) {
                int t = arr[j];
                arr[j] = arr[j + 1];
                arr[j + 1] = t;
            }
        }
    }
}
