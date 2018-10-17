#include "Imaging.h"
#include <math.h>

#include <emmintrin.h>
#include <mmintrin.h>
#include <smmintrin.h>
#if defined(__AVX2__)
    #include <immintrin.h>
#endif


/* 8 bits for result. Table can overflow [0, 1.0] range,
   so we need extra bits for overflow and negative values.
   NOTE: This value should be the same as in _imaging/_prepare_lut_table() */
#define PRECISION_BITS (16 - 8 - 2)
#define PRECISION_ROUNDING (1<<(PRECISION_BITS-1))

/* 8 — scales are multiplied on byte.
   6 — max index in the table
       (max size is 65, but index 64 is not reachable) */
#define SCALE_BITS (32 - 8 - 6)
#define SCALE_MASK ((1<<SCALE_BITS) - 1)

#define SHIFT_BITS (16 - 1)


static inline UINT8 clip8(int in)
{
    return clip8_lookups[(in + PRECISION_ROUNDING) >> PRECISION_BITS];
}

static inline void
interpolate3(INT16 out[3], const INT16 a[3], const INT16 b[3], INT16 shift)
{
    out[0] = (a[0] * ((1<<SHIFT_BITS)-shift) + b[0] * shift) >> SHIFT_BITS;
    out[1] = (a[1] * ((1<<SHIFT_BITS)-shift) + b[1] * shift) >> SHIFT_BITS;
    out[2] = (a[2] * ((1<<SHIFT_BITS)-shift) + b[2] * shift) >> SHIFT_BITS;
}

static inline void
interpolate4(INT16 out[4], const INT16 a[4], const INT16 b[4], INT16 shift)
{
    out[0] = (a[0] * ((1<<SHIFT_BITS)-shift) + b[0] * shift) >> SHIFT_BITS;
    out[1] = (a[1] * ((1<<SHIFT_BITS)-shift) + b[1] * shift) >> SHIFT_BITS;
    out[2] = (a[2] * ((1<<SHIFT_BITS)-shift) + b[2] * shift) >> SHIFT_BITS;
    out[3] = (a[3] * ((1<<SHIFT_BITS)-shift) + b[3] * shift) >> SHIFT_BITS;
}

static inline int
table_index3D(int index1D, int index2D, int index3D,
               int size1D, int size1D_2D)
{
    return index1D + index2D * size1D + index3D * size1D_2D;
}


/*
 Transforms colors of imIn using provided 3D lookup table
 and puts the result in imOut. Returns imOut on success or 0 on error.

 imOut, imIn — images, should be the same size and may be the same image.
    Should have 3 or 4 channels.
 table_channels — number of channels in the lookup table, 3 or 4.
    Should be less or equal than number of channels in imOut image;
 size1D, size_2D and size3D — dimensions of provided table;
 table — flat table,
    array with table_channels × size1D × size2D × size3D elements,
    where channels are changed first, then 1D, then​ 2D, then 3D.
    Each element is signed 16-bit int where 0 is lowest output value
    and 255 << PRECISION_BITS (16320) is highest value.
*/
Imaging
ImagingColorLUT3D_linear(Imaging imOut, Imaging imIn, int table_channels,
                         int size1D, int size2D, int size3D,
                         INT16* table)
{
    /* This float to int conversion doesn't have rounding
       error compensation (+0.5) for two reasons:
       1. As we don't hit the highest value,
          we can use one extra bit for precision.
       2. For every pixel, we interpolate 8 elements from the table:
          current and +1 for every dimension and their combinations.
          If we hit the upper cells from the table,
          +1 cells will be outside of the table.
          With this compensation we never hit the upper cells
          but this also doesn't introduce any noticeable difference. */
    int size1D_2D = size1D * size2D;
    __m128i scale = _mm_set_epi32(0,
        (size3D - 1) / 255.0 * (1<<SCALE_BITS),
        (size2D - 1) / 255.0 * (1<<SCALE_BITS),
        (size1D - 1) / 255.0 * (1<<SCALE_BITS));
    __m128i scale_mask = _mm_set1_epi32(SCALE_MASK);
    __m128i index_mul = _mm_set_epi32(0, size1D_2D, size1D, 1);
    __m128i shuffle_source = _mm_set_epi8(-1,-1, -1,-1, 11,10, 5,4, 9,8, 3,2, 7,6, 1,0);
    __m128i left_mask = _mm_set1_epi32(0x0000ffff);
    __m128i right_mask = _mm_set1_epi32(0xffff0000);
    int x, y;
    ImagingSectionCookie cookie;

    if (table_channels < 3 || table_channels > 4) {
        PyErr_SetString(PyExc_ValueError, "table_channels could be 3 or 4");
        return NULL;
    }

    if (imIn->type != IMAGING_TYPE_UINT8 ||
        imOut->type != IMAGING_TYPE_UINT8 ||
        imIn->bands < 3 ||
        imOut->bands < table_channels
    ) {
        return (Imaging) ImagingError_ModeError();
    }

    /* In case we have one extra band in imOut and don't have in imIn.*/
    if (imOut->bands > table_channels && imOut->bands > imIn->bands) {
        return (Imaging) ImagingError_ModeError();
    }

    ImagingSectionEnter(&cookie);
    for (y = 0; y < imOut->ysize; y++) {
        UINT8* rowIn = (UINT8 *)imIn->image[y];
        UINT32* rowOut = (UINT32 *)imOut->image[y];
        __m128i index = _mm_mullo_epi32(scale,
            _mm_cvtepu8_epi32(*(__m128i *) &rowIn[0]));
        int idx = table_channels * _mm_extract_epi32(
            _mm_hadd_epi32(_mm_hadd_epi32(
                _mm_madd_epi16(index_mul, _mm_srli_epi32(index, SCALE_BITS)),
                _mm_setzero_si128()), _mm_setzero_si128()), 0);
        for (x = 0; x < imOut->xsize; x++) {
            __m128i next_index = _mm_mullo_epi32(scale,
                _mm_cvtepu8_epi32(*(__m128i *) &rowIn[x*4 + 4]));
            int next_idx = table_channels * _mm_extract_epi32(
                _mm_hadd_epi32(_mm_hadd_epi32(
                    _mm_madd_epi16(index_mul, _mm_srli_epi32(next_index, SCALE_BITS)),
                    _mm_setzero_si128()), _mm_setzero_si128()), 0);
            __m128i shift = _mm_srli_epi32(
                _mm_and_si128(scale_mask, index), (SCALE_BITS - SHIFT_BITS));
            __m128i shift1D, shift2D, shift3D;
            __m128i source, left, right, result;
            __m128i leftleft, leftright, rightleft, rightright;

            shift = _mm_or_si128(
                _mm_sub_epi32(_mm_set1_epi32((1<<SHIFT_BITS)-1), shift),
                _mm_slli_epi32(shift, 16));

            shift1D = _mm_shuffle_epi32(shift, 0x00);
            shift2D = _mm_shuffle_epi32(shift, 0x55);
            shift3D = _mm_shuffle_epi32(shift, 0xaa);

            if (table_channels == 3) {
                source = _mm_shuffle_epi8(
                    _mm_loadu_si128((__m128i *) &table[idx + 0]), shuffle_source);
                leftleft = _mm_and_si128(_mm_srai_epi32(_mm_madd_epi16(
                    source, shift1D), SHIFT_BITS), left_mask);

                source = _mm_shuffle_epi8(
                    _mm_loadu_si128((__m128i *) &table[idx + size1D*3]), shuffle_source);
                leftright = _mm_and_si128(_mm_slli_epi32(_mm_madd_epi16(
                    source, shift1D), 16 - SHIFT_BITS), right_mask);

                source = _mm_shuffle_epi8(
                    _mm_loadu_si128((__m128i *) &table[idx + size1D_2D*3]), shuffle_source);
                rightleft = _mm_and_si128(_mm_srai_epi32(_mm_madd_epi16(
                    source, shift1D), SHIFT_BITS), left_mask);
                
                source = _mm_shuffle_epi8(
                    _mm_loadu_si128((__m128i *) &table[idx + size1D_2D*3 + size1D*3]), shuffle_source);
                rightright = _mm_and_si128(_mm_slli_epi32(_mm_madd_epi16(
                    source, shift1D), 16 - SHIFT_BITS), right_mask);

                left = _mm_and_si128(_mm_srai_epi32(_mm_madd_epi16(
                    _mm_or_si128(leftleft, leftright), shift2D),
                    SHIFT_BITS), left_mask);

                right = _mm_and_si128(_mm_slli_epi32(_mm_madd_epi16(
                    _mm_or_si128(rightleft, rightright), shift2D),
                    16 - SHIFT_BITS), right_mask);

                result = _mm_madd_epi16(_mm_or_si128(left, right), shift3D);

                result = _mm_srai_epi32(_mm_add_epi32(
                    _mm_set1_epi32(PRECISION_ROUNDING<<SHIFT_BITS), result),
                    PRECISION_BITS + SHIFT_BITS);

                result = _mm_packs_epi32(result, result);
                rowOut[x] = _mm_cvtsi128_si32(_mm_packus_epi16(result, result));
            }

            // if (table_channels == 4) {
            //     interpolate4(leftleft, &table[idx + 0], &table[idx + 4], shift1D);
            //     interpolate4(leftright, &table[idx + size1D*4],
            //                  &table[idx + size1D*4 + 4], shift1D);
            //     interpolate4(left, leftleft, leftright, shift2D);

            //     interpolate4(rightleft, &table[idx + size1D_2D*4],
            //                  &table[idx + size1D_2D*4 + 4], shift1D);
            //     interpolate4(rightright, &table[idx + size1D_2D*4 + size1D*4],
            //                  &table[idx + size1D_2D*4 + size1D*4 + 4], shift1D);
            //     interpolate4(right, rightleft, rightright, shift2D);

            //     interpolate4(result, left, right, shift3D);

            //     rowOut[x] = MAKE_UINT32(
            //             clip8(result[0]), clip8(result[1]),
            //             clip8(result[2]), clip8(result[3]));
            // }
        }
    }
    ImagingSectionLeave(&cookie);

    return imOut;
}
