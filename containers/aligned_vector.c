#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <stdio.h>

#ifdef _arch_dreamcast
#include "../GL/private.h"
#else
#define FASTCPY memcpy
#endif

#include "aligned_vector.h"

extern inline void* aligned_vector_resize(AlignedVector* vector, const uint32_t element_count);
extern inline void* aligned_vector_extend(AlignedVector* vector, const uint32_t additional_count);
extern inline void* aligned_vector_push_back(AlignedVector* vector, const void* objs, uint32_t count);

void aligned_vector_init(AlignedVector* vector, uint32_t element_size) {
    /* Now initialize the header*/
    AlignedVectorHeader* const hdr = &vector->hdr;
    hdr->size = 0;
    hdr->capacity = 0;
    hdr->element_size = element_size;
    vector->data = NULL;

    /* Reserve some initial capacity. This will do the allocation but not set up the header */
    void* ptr = aligned_vector_reserve(vector, ALIGNED_VECTOR_CHUNK_SIZE);
    assert(ptr);
    (void) ptr;
}

void* aligned_vector_reserve(AlignedVector* vector, uint32_t element_count) {
    AlignedVectorHeader* hdr = &vector->hdr;

    if(vector->data && element_count <= hdr->capacity) {
        /* reserve never changes logical size; return the logical-end address,
           matching the allocation branch below. aligned_vector_at() is
           intentionally unsuitable here because size itself is a valid
           one-past-logical-end pointer value. */
        return vector->data + hdr->size * hdr->element_size;
    }

    uint32_t original_byte_size = (hdr->size * hdr->element_size);

    /* Overflow must be LOUD in Release (AUD-001-OPA-19/OPA-07), and the guard
       must run on the RAW request: ROUND_TO_CHUNK_SIZE itself wraps above
       UINT32_MAX-255, and the growth bump below adds up to 2048+256 more —
       the 4096 headroom covers both, so the byte-size multiply cannot wrap.
       Cold path — only reached when actually growing. */
    av_assert(element_count < UINT32_MAX / hdr->element_size - 4096u);

    /* We overallocate so that we don't make small allocations during push backs */
    element_count = ROUND_TO_CHUNK_SIZE(element_count);

    /* Bounded growth (AUD-001-OPA-03): exact-fit growth memcpys the whole
       accumulated vector every 256 elements — an O(N^2/512)-byte copy cascade
       on a first-peak climb. Growing by min(capacity/2, 2048) elements past
       the request is geometric below 4096 elements and linear (+2048 steps)
       above — roughly 8x fewer copies at city peaks — while capping the
       overshoot at 2048 elements (64 KiB for 32-byte records) per vector. */
    {
        uint32_t half = hdr->capacity / 2;
        uint32_t bump = (half > 2048u) ? 2048u : half;
        uint32_t grown = hdr->capacity + bump;
        if(grown > element_count) element_count = ROUND_TO_CHUNK_SIZE(grown);
    }

    uint32_t new_byte_size = (element_count * hdr->element_size);
    uint8_t* original_data = vector->data;

    #ifdef __XBOX__
            vector->data = (unsigned char*) aligned_alloc(0x20, new_byte_size);
    #else
            vector->data = (unsigned char*) memalign(0x20, new_byte_size);
    #endif

    av_assert(vector->data);

    AV_MEMCPY4(vector->data, original_data, original_byte_size);
    free(original_data);

    hdr->capacity = element_count;
    return vector->data + original_byte_size;
}

void aligned_vector_shrink_to_fit(AlignedVector* vector) {
    AlignedVectorHeader* const hdr = &vector->hdr;
    if(hdr->size == 0) {
        uint32_t element_size = hdr->element_size;
        free(vector->data);

        /* Reallocate the header */
        vector->data = NULL;
        hdr->size = hdr->capacity = 0;
        hdr->element_size = element_size;
    } else {
        uint32_t new_byte_size = (hdr->size * hdr->element_size);
        uint8_t* original_data = vector->data;
#ifdef __XBOX__
        vector->data = (unsigned char*) aligned_alloc(0x20, new_byte_size);
#else
        vector->data = (unsigned char*) memalign(0x20, new_byte_size);
#endif
        av_assert(vector->data);
        if(original_data) {
            FASTCPY(vector->data, original_data, new_byte_size);
            free(original_data);
        }
        hdr->capacity = hdr->size;
    }
}

void aligned_vector_cleanup(AlignedVector* vector) {
    aligned_vector_clear(vector);
    aligned_vector_shrink_to_fit(vector);
}
