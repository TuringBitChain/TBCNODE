// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UINT256_H
#define BITCOIN_UINT256_H

#include "crypto/common.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "span.h"

/** Template base class for fixed-sized opaque blobs. */
template <unsigned int BITS> class base_blob {
protected:
    enum { WIDTH = BITS / 8 };
    uint8_t m_data[WIDTH];

public:
    base_blob() { memset(m_data, 0, sizeof(m_data)); }

    explicit base_blob(const std::vector<uint8_t> &vch);

    constexpr explicit base_blob(bsv::span<const unsigned char> vch)
    {
        assert(vch.size() == WIDTH);
        std::copy(vch.begin(), vch.end(), m_data);
    }

    bool IsNull() const {
        for (int i = 0; i < WIDTH; i++)
            if (m_data[i] != 0) return false;
        return true;
    }

    void SetNull() { memset(m_data, 0, sizeof(m_data)); }

    inline int Compare(const base_blob &other) const {
        return memcmp(m_data, other.m_data, sizeof(m_data));
    }

    friend inline bool operator==(const base_blob &a, const base_blob &b) {
        return a.Compare(b) == 0;
    }
    friend inline bool operator!=(const base_blob &a, const base_blob &b) {
        return a.Compare(b) != 0;
    }
    friend inline bool operator<(const base_blob &a, const base_blob &b) {
        return a.Compare(b) < 0;
    }

    std::string GetHex() const;
    void SetHex(const char *psz);
    void SetHex(const std::string &str);
    std::string ToString() const;

    constexpr const unsigned char* data() const { return m_data; }
    constexpr unsigned char* data() { return m_data; }

    uint8_t* begin()
    {
        return &m_data[0];
    }

    uint8_t* end()
    {
        return &m_data[WIDTH];
    }

    const uint8_t* begin() const
    {
        return &m_data[0];
    }

    const uint8_t* end() const
    {
        return &m_data[WIDTH];
    }

    static constexpr unsigned int size() { return WIDTH; }

    uint64_t GetUint64(int pos) const {
        const uint8_t *ptr = m_data + pos * 8;
        return ((uint64_t)ptr[0]) | ((uint64_t)ptr[1]) << 8 |
               ((uint64_t)ptr[2]) << 16 | ((uint64_t)ptr[3]) << 24 |
               ((uint64_t)ptr[4]) << 32 | ((uint64_t)ptr[5]) << 40 |
               ((uint64_t)ptr[6]) << 48 | ((uint64_t)ptr[7]) << 56;
    }

    template <typename Stream> void Serialize(Stream &s) const {
        s.write(bsv::MakeByteSpan(m_data));
    }

    template <typename Stream> void Unserialize(Stream &s) {
        //s.read((char *)m_data, sizeof(m_data));
        applyRead(s,m_data,sizeof(m_data));
        // auto span = bsv::MakeWritableByteSpan(m_data);
        // s.read(span);
    }
};

/**
 * 160-bit opaque blob.
 * @note This type is called uint160 for historical reasons only. It is an
 * opaque blob of 160 bits and has no integer operations.
 */
class uint160 : public base_blob<160> {
public:
    uint160() {}
    uint160(const base_blob<160> &b) : base_blob<160>(b) {}
    explicit uint160(const std::vector<uint8_t> &vch) : base_blob<160>(vch) {}
};

/**
 * 256-bit opaque blob.
 * @note This type is called uint256 for historical reasons only. It is an
 * opaque blob of 256 bits and has no integer operations. Use arith_uint256 if
 * those are required.
 */
class uint256 : public base_blob<256> {
public:
    uint256() {}
    uint256(const base_blob<256> &b) : base_blob<256>(b) {}
    explicit uint256(const std::vector<uint8_t> &vch) : base_blob<256>(vch) {}
    constexpr explicit uint256(bsv::span<const unsigned char> vch) : base_blob<256>(vch) {}

    /**
     * A cheap hash function that just returns 64 bits from the result, it can
     * be used when the contents are considered uniformly random. It is not
     * appropriate when the value can easily be influenced from outside as e.g.
     * a network adversary could provide values to trigger worst-case behavior.
     */
    uint64_t GetCheapHash() const { return ReadLE64(m_data); }
};

/**
 * Specialise std::hash for uint256.
 */
namespace std
{
    template<>
    class hash<uint256> {
      public:
        size_t operator()(const uint256& u) const
        {
            return static_cast<size_t>(u.GetCheapHash());
        }
    };
}

/**
 * uint256 from const char *.
 * This is a separate function because the constructor uint256(const char*) can
 * result in dangerously catching uint256(0).
 */
inline uint256 uint256S(const char *str) {
    uint256 rv;
    rv.SetHex(str);
    return rv;
}

/**
 * uint256 from std::string.
 * This is a separate function because the constructor uint256(const std::string
 * &str) can result in dangerously catching uint256(0) via std::string(const
 * char*).
 */
inline uint256 uint256S(const std::string &str) {
    uint256 rv;
    rv.SetHex(str);
    return rv;
}

inline uint160 uint160S(const char *str) {
    uint160 rv;
    rv.SetHex(str);
    return rv;
}
inline uint160 uint160S(const std::string &str) {
    uint160 rv;
    rv.SetHex(str);
    return rv;
}

#endif // BITCOIN_UINT256_H
