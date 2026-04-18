#ifndef OPENSSL_MOCKS_H
#define OPENSSL_MOCKS_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Types
typedef struct X509 { int dummy; } X509;
typedef struct EVP_PKEY { int dummy; } EVP_PKEY;
typedef struct PKCS12 { int dummy; } PKCS12;
typedef struct EVP_MD_CTX { int dummy; } EVP_MD_CTX;
typedef struct EVP_CIPHER_CTX { int dummy; } EVP_CIPHER_CTX;
typedef struct EVP_PKEY_CTX { int dummy; } EVP_PKEY_CTX;
typedef struct BIO { int dummy; } BIO;
typedef struct X509_NAME { int dummy; } X509_NAME;
typedef struct { int dummy; } ASN1_INTEGER;
typedef struct { int dummy; } ASN1_TIME;
typedef struct { int dummy; } ASN1_STRING;

typedef struct { 
    int length; 
    unsigned char* data; 
} ASN1_BIT_STRING;

// Constants
#define EVP_aes_128_ecb() (void*)0
#define EVP_sha256() (void*)0
#define EVP_PKEY_RSA 1
#define BIO_NOCLOSE 0
#define MBSTRING_ASC 0

// General Functions
static inline void SHA256(const unsigned char *d, size_t l, unsigned char *md) { if(md) memset(md, 0, 32); }
static inline void SHA1(const unsigned char *d, size_t l, unsigned char *md) { if(md) memset(md, 0, 20); }
static inline int RAND_bytes(unsigned char *buf, int num) { return 1; }
static inline void* OPENSSL_malloc(size_t size) { return malloc(size); }

// Digest Sign
static inline EVP_MD_CTX* EVP_MD_CTX_create() { return (EVP_MD_CTX*)1; }
static inline int EVP_DigestSignInit(EVP_MD_CTX *ctx, void* s, void* md, void* p, void* pkey) { return 1; }
static inline int EVP_DigestSignUpdate(EVP_MD_CTX *ctx, const void* data, size_t len) { return 1; }
static inline int EVP_DigestSignFinal(EVP_MD_CTX *ctx, unsigned char* sig, size_t* len) { 
    if (sig) { 
        *len = 64; 
        memset(sig, 0, 64); 
    } else if (len) { 
        *len = 64; 
    }
    return 1; 
}
static inline void EVP_MD_CTX_destroy(EVP_MD_CTX *ctx) {}

// Cipher
static inline EVP_CIPHER_CTX* EVP_CIPHER_CTX_new() { return (EVP_CIPHER_CTX*)1; }
static inline int EVP_EncryptInit(EVP_CIPHER_CTX* ctx, void* cipher, const unsigned char* key, void* iv) { return 1; }
static inline int EVP_CIPHER_CTX_set_padding(EVP_CIPHER_CTX* ctx, int padding) { return 1; }
static inline int EVP_EncryptUpdate(EVP_CIPHER_CTX* ctx, unsigned char* out, int* outl, const unsigned char* in, int inl) { 
    if(outl) *outl = inl; 
    if(out && in) memcpy(out, in, inl);
    return 1; 
}
static inline void EVP_CIPHER_CTX_free(EVP_CIPHER_CTX* ctx) {}

static inline int EVP_DecryptInit(EVP_CIPHER_CTX* ctx, void* cipher, const unsigned char* key, void* iv) { return 1; }
static inline int EVP_DecryptUpdate(EVP_CIPHER_CTX* ctx, unsigned char* out, int* outl, const unsigned char* in, int inl) { 
    if(outl) *outl = inl; 
    if(out && in) memcpy(out, in, inl);
    return 1; 
}

// PKEY CTX
static inline EVP_PKEY_CTX* EVP_PKEY_CTX_new_id(int id, void* p) { return (EVP_PKEY_CTX*)1; }
static inline int EVP_PKEY_keygen_init(EVP_PKEY_CTX* ctx) { return 1; }
static inline int EVP_PKEY_CTX_set_rsa_keygen_bits(EVP_PKEY_CTX* ctx, int bits) { return 1; }
static inline int EVP_PKEY_keygen(EVP_PKEY_CTX* ctx, EVP_PKEY** pkey) { 
    *pkey = (EVP_PKEY*)1; 
    return 1; 
}
static inline void EVP_PKEY_CTX_free(EVP_PKEY_CTX* ctx) {}

// X509 & PEM
static inline X509* PEM_read_X509(void* fp, void* x, void* p, void* f) { return (X509*)1; }
static inline EVP_PKEY* PEM_read_PrivateKey(void* fp, void* p, void* p2, void* f) { return (EVP_PKEY*)1; }
static inline int PEM_write_PrivateKey(void* fp, EVP_PKEY* p, void* p2, void* f, int opt, void* p3, void* p4) { return 1; }
static inline int PEM_write_X509(void* fp, X509* x) { return 1; }
static inline void X509_free(X509* x) {}
static inline void EVP_PKEY_free(EVP_PKEY* p) {}

// PKCS12
static inline PKCS12* PKCS12_create(const char* s, const char* a, EVP_PKEY* p, X509* x, void* p2, int i1, int i2, int i3, int i4, int i5) { return (PKCS12*)1; }
static inline void PKCS12_free(PKCS12* p) {}
static inline int i2d_PKCS12_fp(void* fp, PKCS12* p) { return 1; }

// BIO
static inline BIO* BIO_new(int type) { return (BIO*)1; }
static inline BIO* BIO_new_fp(FILE* fp, int flags) { return (BIO*)1; }
static inline int BIO_s_mem() { return 1; }
static inline int BIO_puts(BIO* b, const char* s) { return 0; }
static inline void BIO_free(BIO* b) {}

static inline X509* PEM_read_bio_X509(BIO* b, void* x, void* p, void* f) { return (X509*)1; }
static inline EVP_PKEY* X509_get_pubkey(X509* x) { return (EVP_PKEY*)1; }
static inline int EVP_DigestVerifyInit(EVP_MD_CTX* ctx, void* s, void* md, void* p, void* pubKey) { return 1; }
static inline int EVP_DigestVerifyUpdate(EVP_MD_CTX* ctx, const void* data, int len) { return 1; }
static inline int EVP_DigestVerifyFinal(EVP_MD_CTX* ctx, const void* sig, int len) { return 1; }

// X509 internals
static inline X509* X509_new() { return (X509*)1; }
static inline void X509_set_version(X509* x, int v) {}
static inline void ASN1_INTEGER_set(ASN1_INTEGER* i, int v) {}
static inline ASN1_INTEGER* X509_get_serialNumber(X509* x) { return (ASN1_INTEGER*)1; }
static inline void X509_gmtime_adj(void* t, int adj) {}
static inline ASN1_TIME* X509_get_notBefore(X509* x) { return (ASN1_TIME*)1; }
static inline ASN1_TIME* X509_get_notAfter(X509* x) { return (ASN1_TIME*)1; }
static inline ASN1_TIME* ASN1_STRING_dup(ASN1_TIME* t) { return (ASN1_TIME*)1; }
static inline void X509_set1_notBefore(X509* x, ASN1_TIME* t) {}
static inline void X509_set1_notAfter(X509* x, ASN1_TIME* t) {}
static inline void ASN1_STRING_free(ASN1_TIME* t) {}
static inline void X509_set_pubkey(X509* x, EVP_PKEY* p) {}
static inline X509_NAME* X509_get_subject_name(X509* x) { return (X509_NAME*)1; }
static inline int X509_NAME_add_entry_by_txt(X509_NAME* n, const char* cid, int type, const unsigned char* value, int len, int day, int year) { return 1; }
static inline void X509_set_issuer_name(X509* x, X509_NAME* n) {}
static inline int X509_sign(X509* x, EVP_PKEY* p, void* md) { return 1; }

static inline const ASN1_BIT_STRING* X509_get0_signature(X509* x, void* p, void* cert) { 
    static ASN1_BIT_STRING mock_sig;
    static unsigned char mock_data[128];
    mock_sig.length = 64;
    mock_sig.data = mock_data;
    return &mock_sig; 
}
#endif
