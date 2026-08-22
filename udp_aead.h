// ============================================================
// AUTONOMNI DRONOVI - VERZIJA 14
// Fajl: udp_aead.h
// Dodano: bez funkcionalnih promjena; zadrzana AES-256-GCM
//         zastita UDP TELEMETRY/KEEPALIVE payload-a.
// ============================================================

#ifndef SDP_UDP_AEAD_H
#define SDP_UDP_AEAD_H

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "json/json.h"

namespace sdpsec
{
using json = nlohmann::json;

inline std::string hex_encode(const unsigned char* data, std::size_t len)
{
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (std::size_t i = 0; i < len; ++i)
    {
        unsigned char b = data[i];
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0x0f]);
    }
    return out;
}

inline unsigned char hex_value(char c)
{
    if (c >= '0' && c <= '9') return static_cast<unsigned char>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<unsigned char>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<unsigned char>(c - 'A' + 10);
    throw std::runtime_error("Invalid hex character");
}

inline std::vector<unsigned char> hex_decode(const std::string& hex)
{
    if (hex.size() % 2 != 0)
        throw std::runtime_error("Invalid hex length");

    std::vector<unsigned char> out(hex.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<unsigned char>((hex_value(hex[2*i]) << 4) |
                                            hex_value(hex[2*i + 1]));
    return out;
}

inline json encrypt_udp_envelope(const std::string& drone_uri,
                                 const json& plaintext_json,
                                 const std::array<unsigned char, 32>& key)
{
    const std::string plaintext = plaintext_json.dump();
    const std::string aad = drone_uri;

    std::array<unsigned char, 12> nonce{};
    std::array<unsigned char, 16> tag{};
    if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1)
        throw std::runtime_error("RAND_bytes failed");

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        throw std::runtime_error("EVP_CIPHER_CTX_new failed");

    std::vector<unsigned char> ciphertext(plaintext.size() + 16);
    int len = 0;
    int total = 0;

    try
    {
        if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1)
            throw std::runtime_error("AES-GCM init failed");
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
                                static_cast<int>(nonce.size()), NULL) != 1)
            throw std::runtime_error("AES-GCM IV length failed");
        if (EVP_EncryptInit_ex(ctx, NULL, NULL, key.data(), nonce.data()) != 1)
            throw std::runtime_error("AES-GCM key/nonce init failed");

        if (!aad.empty())
        {
            if (EVP_EncryptUpdate(ctx, NULL, &len,
                                  reinterpret_cast<const unsigned char*>(aad.data()),
                                  static_cast<int>(aad.size())) != 1)
                throw std::runtime_error("AES-GCM AAD failed");
        }

        if (EVP_EncryptUpdate(ctx, ciphertext.data(), &len,
                              reinterpret_cast<const unsigned char*>(plaintext.data()),
                              static_cast<int>(plaintext.size())) != 1)
            throw std::runtime_error("AES-GCM encrypt failed");
        total = len;

        if (EVP_EncryptFinal_ex(ctx, ciphertext.data() + total, &len) != 1)
            throw std::runtime_error("AES-GCM final failed");
        total += len;

        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG,
                                static_cast<int>(tag.size()), tag.data()) != 1)
            throw std::runtime_error("AES-GCM tag failed");
    }
    catch (...)
    {
        EVP_CIPHER_CTX_free(ctx);
        throw;
    }

    EVP_CIPHER_CTX_free(ctx);
    ciphertext.resize(static_cast<std::size_t>(total));

    json envelope;
    envelope["TYPE"] = "PQC_UDP";
    envelope["DRONE_URI"] = drone_uri;
    envelope["NONCE"] = hex_encode(nonce.data(), nonce.size());
    envelope["TAG"] = hex_encode(tag.data(), tag.size());
    envelope["CIPHERTEXT"] = hex_encode(ciphertext.data(), ciphertext.size());
    return envelope;
}

inline json decrypt_udp_envelope(const json& envelope,
                                 const std::array<unsigned char, 32>& key)
{
    const std::string drone_uri = envelope.at("DRONE_URI").get<std::string>();
    const std::vector<unsigned char> nonce = hex_decode(envelope.at("NONCE").get<std::string>());
    const std::vector<unsigned char> tag = hex_decode(envelope.at("TAG").get<std::string>());
    const std::vector<unsigned char> ciphertext = hex_decode(envelope.at("CIPHERTEXT").get<std::string>());

    if (nonce.size() != 12 || tag.size() != 16)
        throw std::runtime_error("Invalid AES-GCM nonce/tag size");

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        throw std::runtime_error("EVP_CIPHER_CTX_new failed");

    std::vector<unsigned char> plaintext(ciphertext.size() + 1);
    int len = 0;
    int total = 0;

    try
    {
        if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1)
            throw std::runtime_error("AES-GCM decrypt init failed");
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
                                static_cast<int>(nonce.size()), NULL) != 1)
            throw std::runtime_error("AES-GCM IV length failed");
        if (EVP_DecryptInit_ex(ctx, NULL, NULL, key.data(), nonce.data()) != 1)
            throw std::runtime_error("AES-GCM decrypt key/nonce init failed");

        if (!drone_uri.empty())
        {
            if (EVP_DecryptUpdate(ctx, NULL, &len,
                                  reinterpret_cast<const unsigned char*>(drone_uri.data()),
                                  static_cast<int>(drone_uri.size())) != 1)
                throw std::runtime_error("AES-GCM AAD decrypt failed");
        }

        if (!ciphertext.empty())
        {
            if (EVP_DecryptUpdate(ctx, plaintext.data(), &len,
                                  ciphertext.data(),
                                  static_cast<int>(ciphertext.size())) != 1)
                throw std::runtime_error("AES-GCM decrypt failed");
            total = len;
        }

        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG,
                                static_cast<int>(tag.size()),
                                const_cast<unsigned char*>(tag.data())) != 1)
            throw std::runtime_error("AES-GCM set tag failed");

        int final_ok = EVP_DecryptFinal_ex(ctx, plaintext.data() + total, &len);
        if (final_ok != 1)
            throw std::runtime_error("AES-GCM authentication failed");
        total += len;
    }
    catch (...)
    {
        EVP_CIPHER_CTX_free(ctx);
        throw;
    }

    EVP_CIPHER_CTX_free(ctx);
    std::string decoded(reinterpret_cast<const char*>(plaintext.data()),
                        static_cast<std::size_t>(total));
    return json::parse(decoded);
}
} // namespace sdpsec

#endif
