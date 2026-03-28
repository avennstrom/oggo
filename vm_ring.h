#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef struct {
    SIZE_T size;             // logical buffer size (bytes)
    HANDLE section;          // file-mapping handle (backed by page-file)
    void *base;              // start address of the double-mapped region
    volatile SIZE_T head;    // write position
    volatile SIZE_T tail;    // read position
} vm_ring_t;

vm_ring_t vm_ring_create(SIZE_T size);
void vm_ring_destroy(vm_ring_t *r);
SIZE_T vm_ring_write(vm_ring_t *r, const void *data, SIZE_T len);
SIZE_T vm_ring_read(vm_ring_t *r, void *out, SIZE_T len);