#ifndef SDP_PQC_TLS_UTILS_H
#define SDP_PQC_TLS_UTILS_H

#include <boost/asio/ssl.hpp>
#include <openssl/ssl.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/evp.h>
#include <openssl/err.h>

#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace sdpsec
{
inline std::string openssl_error_string()
{
    unsigned long code = ERR_get_error();
    if (code == 0)
        return "unknown OpenSSL error";

    char buffer[256];
    ERR_error_string_n(code, buffer, sizeof(buffer));
    return std::string(buffer);
}

inline void require_ssl(int ok, const std::string& what)
{
    if (ok != 1)
        throw std::runtime_error(what + ": " + openssl_error_string());
}

inline std::string env_or(const char* name, const std::string& fallback)
{
    const char* value = std::getenv(name);
    return (value && *value) ? std::string(value) : fallback;
}

inline void configure_pqc_common(boost::asio::ssl::context& ctx)
{
    SSL_CTX* native = ctx.native_handle();

    require_ssl(SSL_CTX_set_min_proto_version(native, TLS1_3_VERSION),
                "Cannot enforce minimum TLS 1.3");
    require_ssl(SSL_CTX_set_max_proto_version(native, TLS1_3_VERSION),
                "Cannot enforce maximum TLS 1.3");

    // Hibridni klasicni + post-kvantni KEM iz OpenSSL 3.5+.
    require_ssl(SSL_CTX_set1_groups_list(native, "X25519MLKEM768"),
                "Cannot enable X25519MLKEM768");

    // Post-kvantni potpis koji koristimo u ML-DSA-44 certifikatima.
    require_ssl(SSL_CTX_set1_sigalgs_list(native, "ML-DSA-44"),
                "Cannot enable ML-DSA-44 signatures");
}

inline void load_pqc_server_identity(boost::asio::ssl::context& ctx,
                                     const std::string& cert_file,
                                     const std::string& key_file)
{
    BIO* bio_key = BIO_new_file(key_file.c_str(), "r");
    if (!bio_key)
        throw std::runtime_error("Cannot open PQC private key " + key_file +
                                 ": " + openssl_error_string());

    EVP_PKEY* key = PEM_read_bio_PrivateKey(bio_key, NULL, NULL, NULL);
    BIO_free(bio_key);
    if (!key)
        throw std::runtime_error("Cannot parse PQC private key " + key_file +
                                 ": " + openssl_error_string());

    BIO* bio_cert = BIO_new_file(cert_file.c_str(), "r");
    if (!bio_cert)
    {
        EVP_PKEY_free(key);
        throw std::runtime_error("Cannot open PQC certificate " + cert_file +
                                 ": " + openssl_error_string());
    }

    X509* cert = PEM_read_bio_X509(bio_cert, NULL, NULL, NULL);
    BIO_free(bio_cert);
    if (!cert)
    {
        EVP_PKEY_free(key);
        throw std::runtime_error("Cannot parse PQC certificate " + cert_file +
                                 ": " + openssl_error_string());
    }

    SSL_CTX* native = ctx.native_handle();
    int cert_ok = SSL_CTX_use_certificate(native, cert);
    int key_ok = SSL_CTX_use_PrivateKey(native, key);

    X509_free(cert);
    EVP_PKEY_free(key);

    require_ssl(cert_ok, "SSL_CTX_use_certificate failed");
    require_ssl(key_ok, "SSL_CTX_use_PrivateKey failed");
    require_ssl(SSL_CTX_check_private_key(native),
                "PQC certificate/private key mismatch");
}

inline void configure_pqc_server(boost::asio::ssl::context& ctx,
                                 const std::string& cert_file,
                                 const std::string& key_file)
{
    configure_pqc_common(ctx);
    load_pqc_server_identity(ctx, cert_file, key_file);
    ctx.set_verify_mode(boost::asio::ssl::verify_none);
}

inline void configure_pqc_client(boost::asio::ssl::context& ctx,
                                 const std::string& trusted_server_cert)
{
    configure_pqc_common(ctx);
    ctx.set_verify_mode(boost::asio::ssl::verify_peer);

    require_ssl(SSL_CTX_load_verify_file(ctx.native_handle(),
                                        trusted_server_cert.c_str()),
                "Cannot load trusted PQC certificate " + trusted_server_cert);
}

inline void print_tls_session(SSL* ssl, const std::string& prefix)
{
    const char* version = SSL_get_version(ssl);
    const char* cipher = SSL_get_cipher_name(ssl);
    const char* group = SSL_get0_group_name(ssl);

    std::cerr << prefix
              << " TLS=" << (version ? version : "UNKNOWN")
              << " | GROUP=" << (group ? group : "UNKNOWN")
              << " | CIPHER=" << (cipher ? cipher : "UNKNOWN");

    X509* peer = SSL_get1_peer_certificate(ssl);
    if (peer)
    {
        EVP_PKEY* pub = X509_get_pubkey(peer);
        if (pub)
        {
            const char* type_name = EVP_PKEY_get0_type_name(pub);
            std::cerr << " | PEER_KEY=" << (type_name ? type_name : "UNKNOWN");
            EVP_PKEY_free(pub);
        }
        X509_free(peer);
    }

    std::cerr << std::endl;
}

inline std::array<unsigned char, 32> export_udp_key(SSL* ssl)
{
    std::array<unsigned char, 32> key{};
    static const char label[] = "EXPORTER-SDP-PQC-UDP-AES256GCM-v1";

    require_ssl(SSL_export_keying_material(
                    ssl,
                    key.data(), key.size(),
                    label, sizeof(label) - 1,
                    NULL, 0, 0),
                "TLS exporter failed");

    return key;
}
} // namespace sdpsec

#endif
