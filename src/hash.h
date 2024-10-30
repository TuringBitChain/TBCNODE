// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Copyright (c) 2024 TBCNODE DEV GROUP
// Distributed under the Open TBC software license, see the accompanying file LICENSE.


#ifndef BITCOIN_HASH_H
#define BITCOIN_HASH_H

#include "crypto/ripemd160.h"
#include "crypto/sha256.h"
#include "prevector.h"
#include "serialize.h"
#include "uint256.h"
#include "version.h"
#include "logging.h"

#include <vector>

typedef uint256 ChainCode;

#include <iostream> //zws
using namespace std; //zws
#include "primitives/transaction.h"  //zws

/** A hasher class for Bitcoin's 256-bit hash (double SHA-256). */
class CHash256 {
private:
    CSHA256 sha;

public:
    static const size_t OUTPUT_SIZE = CSHA256::OUTPUT_SIZE;

    void Finalize(bsv::span<unsigned char> output) {
        assert(output.size() == OUTPUT_SIZE);
        unsigned char buf[CSHA256::OUTPUT_SIZE];
        sha.Finalize(buf);
        sha.Reset().Write(buf, CSHA256::OUTPUT_SIZE).Finalize(output.data());
    }
    void Finalize(uint8_t hash[OUTPUT_SIZE]) {
        uint8_t buf[CSHA256::OUTPUT_SIZE];
        sha.Finalize(buf);
        sha.Reset().Write(buf, CSHA256::OUTPUT_SIZE).Finalize(hash);
    }
    void SingleFinalize(uint8_t hash[OUTPUT_SIZE]) {
        //uint8_t buf[CSHA256::OUTPUT_SIZE];
        //sha.Finalize(buf);
        //sha.Reset().Write(buf, CSHA256::OUTPUT_SIZE).Finalize(hash);
        sha.Finalize(hash);
    }

    CHash256 &Write(const uint8_t *data, size_t len) {
        sha.Write(data, len);
        return *this;
    }

    CHash256 &Reset() {
        sha.Reset();
        return *this;
    }
};

/** A hasher class for Bitcoin's 160-bit hash (SHA-256 + RIPEMD-160). */
class CHash160 {
private:
    CSHA256 sha;

public:
    static const size_t OUTPUT_SIZE = CRIPEMD160::OUTPUT_SIZE;

    void Finalize(uint8_t hash[OUTPUT_SIZE]) {
        uint8_t buf[CSHA256::OUTPUT_SIZE];
        sha.Finalize(buf);
        CRIPEMD160().Write(buf, CSHA256::OUTPUT_SIZE).Finalize(hash);
    }

    CHash160 &Write(const uint8_t *data, size_t len) {
        sha.Write(data, len);
        return *this;
    }

    CHash160 &Reset() {
        sha.Reset();
        return *this;
    }
};

/** Compute the 256-bit hash of an object. */
template<typename T>
inline uint256 Hash(const T& in1)
{
    uint256 result;
    auto tmpSpan = bsv::MakeUCharSpan(in1);
    CHash256().Write(tmpSpan.data(),tmpSpan.size()).Finalize(result);
    return result;
}

/** Compute the 256-bit hash of an object. */
template <typename T1> inline uint256 Hash(const T1 pbegin, const T1 pend) {
    static const uint8_t pblank[1] = {};
    uint256 result;
    CHash256()
        .Write(pbegin == pend ? pblank : (const uint8_t *)&pbegin[0],
               (pend - pbegin) * sizeof(pbegin[0]))
        .Finalize((uint8_t *)&result);
    return result;
}

/** Compute the 256-bit hash of the concatenation of two objects. */
template <typename T1, typename T2>
inline uint256 Hash(const T1 p1begin, const T1 p1end, const T2 p2begin,
                    const T2 p2end) {
    static const uint8_t pblank[1] = {};
    uint256 result;
    CHash256()
        .Write(p1begin == p1end ? pblank : (const uint8_t *)&p1begin[0],
               (p1end - p1begin) * sizeof(p1begin[0]))
        .Write(p2begin == p2end ? pblank : (const uint8_t *)&p2begin[0],
               (p2end - p2begin) * sizeof(p2begin[0]))
        .Finalize((uint8_t *)&result);
    return result;
}

/** Compute the 256-bit hash of the concatenation of three objects. */
template <typename T1, typename T2, typename T3>
inline uint256 Hash(const T1 p1begin, const T1 p1end, const T2 p2begin,
                    const T2 p2end, const T3 p3begin, const T3 p3end) {
    static const uint8_t pblank[1] = {};
    uint256 result;
    CHash256()
        .Write(p1begin == p1end ? pblank : (const uint8_t *)&p1begin[0],
               (p1end - p1begin) * sizeof(p1begin[0]))
        .Write(p2begin == p2end ? pblank : (const uint8_t *)&p2begin[0],
               (p2end - p2begin) * sizeof(p2begin[0]))
        .Write(p3begin == p3end ? pblank : (const uint8_t *)&p3begin[0],
               (p3end - p3begin) * sizeof(p3begin[0]))
        .Finalize((uint8_t *)&result);
    return result;
}

/** Compute the 160-bit hash an object. */
template <typename T1> inline uint160 Hash160(const T1 pbegin, const T1 pend) {
    static uint8_t pblank[1] = {};
    uint160 result;
    CHash160()
        .Write(pbegin == pend ? pblank : (const uint8_t *)&pbegin[0],
               (pend - pbegin) * sizeof(pbegin[0]))
        .Finalize((uint8_t *)&result);
    return result;
}

/** Compute the 160-bit hash of a vector. */
inline uint160 Hash160(const std::vector<uint8_t> &vch) {
    return Hash160(vch.begin(), vch.end());
}

/** Compute the 160-bit hash of a vector. */
template <unsigned int N>
inline uint160 Hash160(const prevector<N, uint8_t> &vch) {
    return Hash160(vch.begin(), vch.end());
}

/** A writer stream (for serialization) that computes a 256-bit hash. */
class CHashWriter {
private:
    CHash256 ctx;

    const int nType;
    const int nVersion;

public:
    CHashWriter(int nTypeIn, int nVersionIn)
        : nType(nTypeIn), nVersion(nVersionIn) {}

    int GetType() const { return nType; }
    int GetVersion() const { return nVersion; }

    void write(const char *pch, size_t size) {
        ctx.Write((const uint8_t *)pch, size);
    }

    void write(bsv::span<const std::byte> Span) {
        ctx.Write((const uint8_t *)Span.data(), Span.size());
    }

    // invalidates the object
    uint256 GetHash() {
        uint256 result;
        ctx.Finalize((uint8_t *)&result);
        return result;
    }

    uint256 GetSingleHash() {
        uint256 result;
        ctx.SingleFinalize((uint8_t *)&result);
        return result;
    }

    template <typename T> CHashWriter &operator<<(const T &obj) {
        // Serialize to this stream
        ::Serialize(*this, obj);
        return (*this);
    }
};


/** A writer stream (for serialization) that computes a 256-bit hash. */
class HashWriter
{
private:
    CSHA256 ctx;

public:
    void write(bsv::span<const std::byte> src)
    {
        ctx.Write(bsv::UCharCast(src.data()), src.size());
    }

    /** Compute the double-SHA256 hash of all data written to this object.
     *
     * Invalidates this object.
     */
    uint256 GetHash() {
        uint256 result;
        ctx.Finalize(result.begin());
        ctx.Reset().Write(result.begin(), CSHA256::OUTPUT_SIZE).Finalize(result.begin());
        return result;
    }

    /** Compute the SHA256 hash of all data written to this object.
     *
     * Invalidates this object.
     */
    uint256 GetSHA256() {
        uint256 result;
        ctx.Finalize(result.begin());
        return result;
    }

    /**
     * Returns the first 64 bits from the resulting hash.
     */
    inline uint64_t GetCheapHash() {
        uint256 result = GetHash();
        return ReadLE64(result.begin());
    }

    template <typename T>
    HashWriter& operator<<(const T& obj)
    {
        ::Serialize(*this, obj);
        return *this;
    }
};


/**
 * Reads data from an underlying stream, while hashing the read data.
 */
template <typename Source> class CHashVerifier : public CHashWriter {
private:
    Source *source;

public:
    CHashVerifier(Source *source_)
        : CHashWriter(source_->GetType(), source_->GetVersion()),
          source(source_) {}

    void read(char *pch, size_t nSize) {
        source->read(pch, nSize);
        this->write(pch, nSize);
    }

    void read(bsv::span<std::byte> dst)
    {
        source->read(dst);
        this->write(dst);
    }

    void ignore(size_t nSize) {
        char data[1024];
        while (nSize > 0) {
            size_t now = std::min<size_t>(nSize, 1024);
            read(data, now);
            nSize -= now;
        }
    }

    template <typename T> CHashVerifier<Source> &operator>>(T &obj) {
        // Unserialize from this stream
        ::Unserialize(*this, obj);
        return (*this);
    }
};


/** Compute the 256-bit hash of an object's serialization. */
template <typename T>
uint256 SerializeHash(const T &obj, int nType = SER_GETHASH,
                      int nVersion = PROTOCOL_VERSION) {
    //cout << "SerializeHash: nType:" << nType << ", nVersion:" << nVersion << endl;  //zws
    CHashWriter ss(nType, nVersion);
    ss << obj;
    return ss.GetHash();
}
/** Compute the 256-bit hash of an object's serialization. */
template <typename T>
uint256 SerializeSingleHash(const T &obj, int nType = SER_GETHASH,
                      int nVersion = PROTOCOL_VERSION) {
    //cout << "SerializeHash: nType:" << nType << ", nVersion:" << nVersion << endl;  //zws
    CHashWriter ss(nType, nVersion);
    ss.write( (char *) obj, obj.size() );
    return ss.GetSingleHash();
}
/** Compute the 256-bit hash of an object's serialization. */
template <typename T>
uint256 SerializeSingleHash_OpNoCSize(const T &obj, int nType = SER_GETHASH,
                      int nVersion = PROTOCOL_VERSION) {
    //cout << "SerializeHash: nType:" << nType << ", nVersion:" << nVersion << endl;  //zws
    CHashWriter ss(nType, nVersion);
    SerReadWrite_OpNoCSize(ss, obj, CSerActionSerialize() ) ;
    return ss.GetSingleHash();
}


template <typename T> //zws
uint256 TxSerializeHash(const T &obj, int nType = SER_GETHASH,
                      int nVersion = PROTOCOL_VERSION) {
    // cout << "TxSerializeHash::nType:" << nType << ", nVersion:" << nVersion << ", obj.nVersion:" << obj.nVersion << endl;
    // cout << "obj.vin.size():"  << obj.vin.size() << ", obj.vout.size():" << obj.vout.size()  << " sizeof(.size())" << sizeof(obj.vin.size()) << endl;  //zws
    
    uint256 result;
    uint256 hash_ss_in;
    uint256 hash_ss_in_unlock;

    if ( obj.nVersion >=10) {
        CHashWriter ss_root(nType, nVersion);
        ss_root << obj.nVersion; 
        ss_root << obj.nLockTime;
        
        // WriteCompactSize(ss_root, obj.vin.size() );
        // WriteCompactSize(ss_root, obj.vout.size() );
        //Serialize(ss_root, obj.vin.size() ) ;
        //Serialize(ss_root, obj.vout.size() );
        ser_writedata32(ss_root, obj.vin.size() );
        ser_writedata32(ss_root, obj.vout.size() );

        CHashWriter ss_in(nType, nVersion);
        CHashWriter ss_in_unlock(nType, nVersion);
        for (const CTxIn &iin : obj.vin) {
            ss_in << iin.prevout;
            ss_in << iin.nSequence;

            //CHashWriter ss_in_unlock_one(nType, nVersion);
            //SerReadWrite_OpNoCSize(ss_in_unlock_one, iin.scriptSig, CSerActionSerialize() );
            //ss_in_unlock << ss_in_unlock_one.GetSingleHash();
            //ss_in_unlock << SerializeHash( iin.scriptSig, SER_GETHASH, 0); 
            ss_in_unlock << SerializeSingleHash_OpNoCSize( iin.scriptSig, SER_GETHASH, 0 );
        }
        hash_ss_in = ss_in.GetSingleHash();
        // cout << "TuringTXID TxSerializeHash: ss_in.GetSingleHash().GetHex() :" << hash_ss_in.GetHex() << endl;  //zws        
        ss_root << hash_ss_in;

        hash_ss_in_unlock = ss_in_unlock.GetSingleHash();
        // cout << "TuringTXID TxSerializeHash: ss_in_unlock.GetSingleHash().GetHex() :" << hash_ss_in_unlock.GetHex() << endl;  //zws        
        ss_root << hash_ss_in_unlock;

        CHashWriter ss_out(nType, nVersion);
        for (const CTxOut &iout : obj.vout) {
            ss_out << iout.nValue;

            //ss_out << SerializeHash( iout.scriptPubKey, SER_GETHASH, 0);
            ss_out << SerializeSingleHash_OpNoCSize( iout.scriptPubKey, SER_GETHASH, 0);
        }
        ss_root << ss_out.GetSingleHash();
        
        //result = ss_root.GetSingleHash();
        result = ss_root.GetHash();

    }
    else {
        CHashWriter ss(nType, nVersion);
        ss << obj;
        result = ss.GetHash();

    }

    return result ;
}


unsigned int MurmurHash3(unsigned int nHashSeed,
                         const std::vector<uint8_t> &vDataToHash);

void BIP32Hash(const ChainCode &chainCode, unsigned int nChild, uint8_t header,
               const uint8_t data[32], uint8_t output[64]);

/** SipHash-2-4 */
class CSipHasher {
private:
    uint64_t v[4];
    uint64_t tmp;
    int count;

public:
    /** Construct a SipHash calculator initialized with 128-bit key (k0, k1) */
    CSipHasher(uint64_t k0, uint64_t k1);
    /**
     * Hash a 64-bit integer worth of data.
     * It is treated as if this was the little-endian interpretation of 8 bytes.
     * This function can only be used when a multiple of 8 bytes have been
     * written so far.
     */
    CSipHasher &Write(uint64_t data);
    /** Hash arbitrary bytes. */
    CSipHasher &Write(const uint8_t *data, size_t size);
    /** Compute the 64-bit SipHash-2-4 of the data written so far. The object
     * remains untouched. */
    uint64_t Finalize() const;
};

/** Optimized SipHash-2-4 implementation for uint256.
 *
 *  It is identical to:
 *    SipHasher(k0, k1)
 *      .Write(val.GetUint64(0))
 *      .Write(val.GetUint64(1))
 *      .Write(val.GetUint64(2))
 *      .Write(val.GetUint64(3))
 *      .Finalize()
 */
uint64_t SipHashUint256(uint64_t k0, uint64_t k1, const uint256 &val);
uint64_t SipHashUint256Extra(uint64_t k0, uint64_t k1, const uint256 &val,
                             uint32_t extra);


/** Return a HashWriter primed for tagged hashes (as specified in BIP 340).
 *
 * The returned object will have SHA256(tag) written to it twice (= 64 bytes).
 * A tagged hash can be computed by feeding the message into this object, and
 * then calling HashWriter::GetSHA256().
 */
HashWriter TaggedHash(const std::string& tag);

#endif // BITCOIN_HASH_H
