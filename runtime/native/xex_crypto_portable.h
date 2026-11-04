#pragma once
// Portable SHA-256 and AES-128-CBC decryption for the XEX loader.
//
// The Windows build used bcrypt for both. The console has no bcrypt, so the
// loader's two cryptographic steps live here in plain C++20 and both platforms
// share them: one implementation, one set of test vectors, and a console image
// that hashes exactly like the verified host build.
//
// Scope is exactly what decodeXex needs: a whole-buffer SHA-256 and in-place
// AES-128-CBC decryption with a zero IV. Nothing is authenticated, nothing is
// key-scheduled beyond 128-bit keys, and no padding is applied.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace DarkRecomp::Native::XexCrypto {

// --- GF(2^8) helpers -------------------------------------------------------

inline uint8_t Multiply(uint8_t a, uint8_t b) {
    uint8_t product = 0;
    for (int i = 0; i < 8; ++i) {
        if (b & 1) product ^= a;
        const bool high = (a & 0x80) != 0;
        a = uint8_t(a << 1);
        if (high) a ^= 0x1b;
        b = uint8_t(b >> 1);
    }
    return product;
}

inline uint8_t RotateLeft8(uint8_t value, int amount) {
    return uint8_t((value << amount) | (value >> (8 - amount)));
}

// The AES S-box is the multiplicative inverse in GF(2^8) followed by the
// affine transform. Deriving it removes 256 hand-copied constants (and the
// transcription mistakes that come with them); the inverse table is derived
// from it the same way.
struct AesTables {
    uint8_t substitute[256];
    uint8_t inverse[256];
    AesTables() {
        for (int value = 0; value < 256; ++value) {
            uint8_t result = value == 0 ? 0 : 1;
            if (value != 0) {
                // value^254 is the multiplicative inverse of value.
                for (int i = 0; i < 254; ++i) result = Multiply(result, uint8_t(value));
            }
            const uint8_t s = uint8_t(result ^ RotateLeft8(result, 1) ^ RotateLeft8(result, 2) ^
                                      RotateLeft8(result, 3) ^ RotateLeft8(result, 4) ^ 0x63);
            substitute[value] = s;
            inverse[s] = uint8_t(value);
        }
    }
};

inline const AesTables& Tables() {
    static const AesTables tables;
    return tables;
}

// --- AES-128 ---------------------------------------------------------------

// The 11 round keys, 16 bytes each, expanded from a 128-bit key.
inline std::array<uint8_t, 176> ExpandKey(std::span<const uint8_t, 16> key) {
    std::array<uint8_t, 176> schedule{};
    std::memcpy(schedule.data(), key.data(), 16);
    const uint8_t* substitute = Tables().substitute;
    uint8_t round = 1;
    for (size_t i = 16; i < schedule.size(); i += 4) {
        uint8_t word[4] = {schedule[i - 4], schedule[i - 3], schedule[i - 2], schedule[i - 1]};
        if (i % 16 == 0) {
            const uint8_t first = word[0];
            word[0] = uint8_t(substitute[word[1]] ^ round);
            word[1] = substitute[word[2]];
            word[2] = substitute[word[3]];
            word[3] = substitute[first];
            round = Multiply(round, 2);
        }
        for (size_t b = 0; b < 4; ++b) schedule[i + b] = uint8_t(schedule[i - 16 + b] ^ word[b]);
    }
    return schedule;
}

inline void AddRoundKey(uint8_t* state, const std::array<uint8_t, 176>& schedule, int round) {
    for (int i = 0; i < 16; ++i) state[i] ^= schedule[round * 16 + i];
}

// Rows are shifted right by their row index. The state is column-major:
// state[row + 4 * column].
inline void InverseShiftRows(uint8_t* state) {
    uint8_t shifted[16];
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            shifted[row + 4 * ((column + row) % 4)] = state[row + 4 * column];
        }
    }
    std::memcpy(state, shifted, sizeof(shifted));
}

inline void InverseSubstituteBytes(uint8_t* state) {
    const uint8_t* inverse = Tables().inverse;
    for (int i = 0; i < 16; ++i) state[i] = inverse[state[i]];
}

inline void InverseMixColumns(uint8_t* state) {
    for (int column = 0; column < 4; ++column) {
        uint8_t* c = state + 4 * column;
        const uint8_t a0 = c[0], a1 = c[1], a2 = c[2], a3 = c[3];
        c[0] = uint8_t(Multiply(a0, 0x0e) ^ Multiply(a1, 0x0b) ^ Multiply(a2, 0x0d) ^ Multiply(a3, 0x09));
        c[1] = uint8_t(Multiply(a0, 0x09) ^ Multiply(a1, 0x0e) ^ Multiply(a2, 0x0b) ^ Multiply(a3, 0x0d));
        c[2] = uint8_t(Multiply(a0, 0x0d) ^ Multiply(a1, 0x09) ^ Multiply(a2, 0x0e) ^ Multiply(a3, 0x0b));
        c[3] = uint8_t(Multiply(a0, 0x0b) ^ Multiply(a1, 0x0d) ^ Multiply(a2, 0x09) ^ Multiply(a3, 0x0e));
    }
}

// The inverse cipher: the last round key first, then the inverse rounds down to
// the first key.
inline std::array<uint8_t, 16> DecryptBlock(std::span<const uint8_t, 16> block,
                                            const std::array<uint8_t, 176>& schedule) {
    std::array<uint8_t, 16> state{};
    std::memcpy(state.data(), block.data(), 16);
    AddRoundKey(state.data(), schedule, 10);
    for (int round = 9; round >= 1; --round) {
        InverseShiftRows(state.data());
        InverseSubstituteBytes(state.data());
        AddRoundKey(state.data(), schedule, round);
        InverseMixColumns(state.data());
    }
    InverseShiftRows(state.data());
    InverseSubstituteBytes(state.data());
    AddRoundKey(state.data(), schedule, 0);
    return state;
}

// Decrypts every 16-byte block in place, chaining ciphertext blocks with a zero
// initialization vector. The span length is the caller's to validate.
inline void DecryptCbc(std::vector<uint8_t>& bytes, std::span<const uint8_t, 16> key) {
    const auto schedule = ExpandKey(key);
    std::array<uint8_t, 16> previous{};
    for (size_t offset = 0; offset + 16 <= bytes.size(); offset += 16) {
        std::array<uint8_t, 16> cipher{};
        std::memcpy(cipher.data(), bytes.data() + offset, 16);
        const auto plain = DecryptBlock(std::span<const uint8_t, 16>(cipher.data(), 16), schedule);
        for (int i = 0; i < 16; ++i) bytes[offset + i] = uint8_t(plain[i] ^ previous[i]);
        previous = cipher;
    }
}

// --- SHA-256 ---------------------------------------------------------------

inline uint32_t RotateRight(uint32_t value, unsigned amount) {
    return (value >> amount) | (value << (32 - amount));
}

inline std::array<uint8_t, 32> Sha256(std::span<const uint8_t> bytes) {
    static constexpr uint32_t kRound[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
        0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
        0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
        0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
        0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
        0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

    // The message is padded in place: 0x80, zeros to 56 mod 64, then the bit
    // length big-endian.
    std::vector<uint8_t> message(bytes.begin(), bytes.end());
    const uint64_t bitLength = uint64_t(bytes.size()) * 8;
    message.push_back(0x80);
    while (message.size() % 64 != 56) message.push_back(0);
    for (int shift = 56; shift >= 0; shift -= 8) {
        message.push_back(uint8_t(bitLength >> shift));
    }

    uint32_t hash[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    for (size_t offset = 0; offset < message.size(); offset += 64) {
        uint32_t words[64];
        for (int i = 0; i < 16; ++i) {
            const uint8_t* w = message.data() + offset + i * 4;
            words[i] = uint32_t(w[0]) << 24 | uint32_t(w[1]) << 16 | uint32_t(w[2]) << 8 | w[3];
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = RotateRight(words[i - 15], 7) ^ RotateRight(words[i - 15], 18) ^
                                (words[i - 15] >> 3);
            const uint32_t s1 = RotateRight(words[i - 2], 17) ^ RotateRight(words[i - 2], 19) ^
                                (words[i - 2] >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }
        uint32_t a = hash[0], b = hash[1], c = hash[2], d = hash[3];
        uint32_t e = hash[4], f = hash[5], g = hash[6], h = hash[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t s1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
            const uint32_t choice = (e & f) ^ (~e & g);
            const uint32_t t1 = h + s1 + choice + kRound[i] + words[i];
            const uint32_t s0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
            const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = s0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        hash[0] += a; hash[1] += b; hash[2] += c; hash[3] += d;
        hash[4] += e; hash[5] += f; hash[6] += g; hash[7] += h;
    }

    std::array<uint8_t, 32> digest{};
    for (int i = 0; i < 8; ++i) {
        digest[i * 4 + 0] = uint8_t(hash[i] >> 24);
        digest[i * 4 + 1] = uint8_t(hash[i] >> 16);
        digest[i * 4 + 2] = uint8_t(hash[i] >> 8);
        digest[i * 4 + 3] = uint8_t(hash[i]);
    }
    return digest;
}

inline std::string Sha256Hex(std::span<const uint8_t> bytes) {
    const auto hash = Sha256(bytes);
    constexpr char hex[] = "0123456789abcdef";
    std::string result(64, '0');
    for (size_t i = 0; i < hash.size(); ++i) {
        result[i * 2] = hex[hash[i] >> 4];
        result[i * 2 + 1] = hex[hash[i] & 15];
    }
    return result;
}

} // namespace DarkRecomp::Native::XexCrypto
