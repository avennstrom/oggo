#include "vm_ring.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

// Create ring; size must be >0 and multiple of system page size.
vm_ring_t vm_ring_create(SIZE_T size) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    SIZE_T page = si.dwPageSize;
    // round up to page size
    SIZE_T rounded = ((size + page - 1) / page) * page;
    vm_ring_t r = {0};
    r.size = rounded;

    // Create a file-mapping backed by the page file
    r.section = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 
        (DWORD)((rounded >> 32) & 0xFFFFFFFF), (DWORD)(rounded & 0xFFFFFFFF),
        NULL);
    assert(r.section != NULL);

    // Reserve a contiguous virtual region twice the logical size
    void *reserve = VirtualAlloc(NULL, rounded * 2, MEM_RESERVE, PAGE_NOACCESS);
    assert(reserve != NULL);

    // Map the section into the first half
    void *map1 = MapViewOfFileEx(r.section, FILE_MAP_ALL_ACCESS, 0, 0, rounded, reserve);
    assert(map1 == reserve);

    // Map the same section into the second half (immediately after first)
    void *map2 = MapViewOfFileEx(r.section, FILE_MAP_ALL_ACCESS, 0, 0, rounded, (char*)reserve + rounded);
    assert(map2 == (char*)reserve + rounded);

    r.base = reserve;
    r.head = r.tail = 0;
    return r;
}

void vm_ring_destroy(vm_ring_t *r) {
    if (!r || r->base == NULL) return;
    // Unmap both views
    BOOL ok1 = UnmapViewOfFile(r->base);
    BOOL ok2 = UnmapViewOfFile((char*)r->base + r->size);
    assert(ok1 && ok2);
    // Free reservation
    BOOL ok3 = VirtualFree(r->base, 0, MEM_RELEASE);
    assert(ok3);
    CloseHandle(r->section);
    r->base = NULL;
}

// Write up to len bytes; returns number of bytes written.
SIZE_T vm_ring_write(vm_ring_t *r, const void *data, SIZE_T len) {
    assert(r && r->base);
    SIZE_T free_space = r->size - (r->head - r->tail);
    if (len > free_space) len = free_space;
    // simple memcpy into double-mapped region starting at head offset
    void *dst = (char*)r->base + (r->head % r->size);
    memcpy(dst, data, len);
    // advance head
    r->head += len;
    return len;
}

// Read up to len bytes; returns number of bytes read.
SIZE_T vm_ring_read(vm_ring_t *r, void *out, SIZE_T len) {
    assert(r && r->base);
    SIZE_T available = r->head - r->tail;
    if (len > available) len = available;
    void *src = (char*)r->base + (r->tail % r->size);
    memcpy(out, src, len);
    r->tail += len;
    return len;
}