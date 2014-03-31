/*
 * Copyright (C) 2005-2014 Christoph Rupp (chris@crupp.de).
 * All Rights Reserved.
 *
 * NOTICE: All information contained herein is, and remains the property
 * of Christoph Rupp and his suppliers, if any. The intellectual and
 * technical concepts contained herein are proprietary to Christoph Rupp
 * and his suppliers and may be covered by Patents, patents in process,
 * and are protected by trade secret or copyright law. Dissemination of
 * this information or reproduction of this material is strictly forbidden
 * unless prior written permission is obtained from Christoph Rupp.
 *
 * See the file COPYING for License information.
 *
 */

#ifndef HAM_COMPRESSOR_H__
#define HAM_COMPRESSOR_H__

#include "ham/types.h"
#include "util.h"
#include "endianswap.h"

namespace hamsterdb {

class Compressor {
  public:
    // Constructor
    Compressor()
      : m_skip(0) {
    }

    // Virtual destructor - can be overwritten
    virtual ~Compressor() {
    }

    // Compresses |inlength1| bytes of data in |inp1|. If |inp2| is supplied
    // then |inp2| will be compressed immediately after |inp1|.
    // The compressed data can then be retrieved with |get_output_data()|.
    //
    // Returns the length of the compressed data.
    ham_u32_t compress(const ham_u8_t *inp1, ham_u32_t inlength1,
                    const ham_u8_t *inp2 = 0, ham_u32_t inlength2 = 0) {
      ham_u32_t clen = 0;
      ham_u32_t arena_size = m_skip + get_compressed_length(inlength1);
      if (inp2 != 0)
        arena_size += get_compressed_length(inlength2);
      m_arena.resize(arena_size);

      ham_u8_t *out = (ham_u8_t *)m_arena.get_ptr() + m_skip;

      clen = do_compress(inp1, inlength1, out,
                                  m_arena.get_size() - m_skip);
      if (inp2)
        clen += do_compress(inp2, inlength2, out + clen,
                          m_arena.get_size() - clen - m_skip);
      return (clen);
    }

    // Reserves |n| bytes in the output buffer; can be used by the caller
    // to insert flags or sizes
    void reserve(int n) {
      m_skip = n;
    }

    // Decompresses |inlength| bytes of data in |inp|. |outlength| is the
    // expected size of the decompressed data.
    void decompress(const ham_u8_t *inp, ham_u32_t inlength,
                    ham_u32_t outlength) {
      m_arena.resize(outlength);
      do_decompress(inp, inlength, (ham_u8_t *)m_arena.get_ptr(), outlength);
    }

    // Decompresses |inlength| bytes of data in |inp|. |outlength| is the
    // expected size of the decompressed data. Uses the caller's |arena|
    // for storage.
    void decompress(const ham_u8_t *inp, ham_u32_t inlength,
                    ham_u32_t outlength, ByteArray *arena) {
      arena->resize(outlength);
      do_decompress(inp, inlength, (ham_u8_t *)arena->get_ptr(), outlength);
    }

    // Decompresses |inlength| bytes of data in |inp|. |outlength| is the
    // expected size of the decompressed data. Uses the caller's |destination|
    // for storage.
    void decompress(const ham_u8_t *inp, ham_u32_t inlength,
                    ham_u32_t outlength, ham_u8_t *destination) {
      do_decompress(inp, inlength, destination, outlength);
    }

    // Retrieves the compressed (or decompressed) data, including its size
    const ham_u8_t *get_output_data() const {
      return ((ham_u8_t *)m_arena.get_ptr());
    }

    // Same as above, but non-const
    ham_u8_t *get_output_data() {
      return ((ham_u8_t *)m_arena.get_ptr());
    }

    // Returns the internal memory arena
    ByteArray *get_arena() {
      return (&m_arena);
    }

  protected:
    // Returns the maximum number of bytes that are required for
    // compressing |length| bytes.
    virtual ham_u32_t get_compressed_length(ham_u32_t length) = 0;

    // Performs the actual compression. |outp| points into |m_arena| and
    // has sufficient size (allocated with |get_compressed_length()|).
    //
    // Returns the length of the compressed data.
    // In case of an error: returns length of the uncompressed data + 1
    virtual ham_u32_t do_compress(const ham_u8_t *inp, ham_u32_t inlength,
                            ham_u8_t *outp, ham_u32_t outlength) = 0;

    // Performs the actual decompression. |outp| points into |m_arena| and
    // has sufficient size for the decompressed data.
    virtual void do_decompress(const ham_u8_t *inp, ham_u32_t inlength,
                            ham_u8_t *outp, ham_u32_t outlength) = 0;

  private:
    // The ByteArray which stores the compressed (or decompressed) data
    ByteArray m_arena;

    // Number of bytes to reserve for the caller
    int m_skip;
};

}; // namespace hamsterdb

#endif // HAM_COMPRESSOR_H__