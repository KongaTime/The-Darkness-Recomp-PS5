// Checks the portable XEX crypto against published vectors.
//
// The console XEX loader hashes and decrypts with this code instead of bcrypt.
// A wrong S-box or a bad rotation shows up here, on the host, rather than as an
// unsupported-dump error on the console.
//
// Usage: XexCryptoPortableTests [FILE [SHA256]]
//   With a file and its expected digest, also checks a whole-file hash, which
//   is the loader's revision gate.
#include "runtime/native/xex_crypto_portable.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool ok, const char* what) {
    std::printf("%-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) ++failures;
}

std::vector<uint8_t> hex(const char* text) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; text[i] != '\0' && text[i + 1] != '\0'; i += 2) {
        unsigned value = 0;
        std::sscanf(text + i, "%2x", &value);
        bytes.push_back(uint8_t(value));
    }
    return bytes;
}

} // namespace

int main(int argc, char** argv) {
    using namespace DarkRecomp::Native::XexCrypto;

    // FIPS 180-2 test vectors.
    expect(Sha256Hex(std::span<const uint8_t>()) ==
               "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
           "SHA-256 of the empty message");
    const std::string abc = "abc";
    expect(Sha256Hex(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(abc.data()),
                                              abc.size())) ==
               "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
           "SHA-256 of \"abc\"");
    // A message that crosses the 56-byte padding boundary.
    const std::string padded =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    expect(Sha256Hex(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(padded.data()),
                                              padded.size())) ==
               "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
           "SHA-256 across the padding boundary");

    // NIST SP 800-38A F.2.1: AES-128-CBC decryption of four blocks. The
    // vector's IV is zero here because the loader always starts from zero.
    const auto key = hex("2b7e151628aed2a6abf7158809cf4f3c");
    const auto cipher = hex(
        "7649abac8119b246cee98e9b12e9197d"
        "5086cb9b507219ee95db113a917678b2"
        "73bed6b8e3c1743b7116e69e22229516"
        "3ff1caa1681fac09120eca307586e1a7");
    const auto plain = hex(
        "6bc1bee22e409f96e93d7e117393172a"
        "ae2d8a571e03ac9c9eb76fac45af8e51"
        "30c81c46a35ce411e5fbc1191a0a52ef"
        "f69f2445df4f9b17ad2b417be66c3710");
    {
        // The published IV is not zero, so decrypt the blocks under it and
        // confirm the inverse cipher itself before checking the loader's path.
        const auto expanded = ExpandKey(std::span<const uint8_t, 16>(key.data(), 16));
        const auto iv = hex("000102030405060708090a0b0c0d0e0f");
        std::array<uint8_t, 16> previous{};
        std::memcpy(previous.data(), iv.data(), 16);
        std::vector<uint8_t> recovered(cipher.size());
        for (size_t offset = 0; offset < cipher.size(); offset += 16) {
            const auto block =
                DecryptBlock(std::span<const uint8_t, 16>(cipher.data() + offset, 16), expanded);
            for (int i = 0; i < 16; ++i) recovered[offset + i] = uint8_t(block[i] ^ previous[i]);
            std::memcpy(previous.data(), cipher.data() + offset, 16);
        }
        expect(recovered == plain, "AES-128 inverse cipher (NIST SP 800-38A F.2.1)");
    }
    {
        // The loader's path: a zero IV instead of the vector's. Only the first
        // block differs from the published plaintext, by that IV.
        const auto iv = hex("000102030405060708090a0b0c0d0e0f");
        std::vector<uint8_t> single(cipher.begin(), cipher.begin() + 16);
        DecryptCbc(single, std::span<const uint8_t, 16>(key.data(), 16));
        std::vector<uint8_t> expected(plain.begin(), plain.begin() + 16);
        for (size_t i = 0; i < 16; ++i) expected[i] ^= iv[i];
        expect(single == expected, "AES-128-CBC first block from a zero IV");
    }
    {
        // Every later block chains the same ciphertext, so a zero IV changes
        // only the first block: blocks 1..3 match the published plaintext.
        std::vector<uint8_t> chained = cipher;
        DecryptCbc(chained, std::span<const uint8_t, 16>(key.data(), 16));
        expect(std::memcmp(chained.data() + 16, plain.data() + 16, plain.size() - 16) == 0,
               "AES-128-CBC chains the previous ciphertext block");
    }

    if (argc >= 2) {
        std::ifstream file(argv[1], std::ios::binary);
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                         std::istreambuf_iterator<char>());
        expect(!bytes.empty(), "read the file to hash");
        const std::string digest = Sha256Hex(bytes);
        std::printf("%-58s %s\n", "whole-file SHA-256", digest.c_str());
        if (argc >= 3) expect(digest == argv[2], "matches the expected revision digest");
    }

    std::printf("\n%s\n", failures == 0 ? "all crypto checks passed" : "CRYPTO CHECKS FAILED");
    return failures == 0 ? 0 : 1;
}
