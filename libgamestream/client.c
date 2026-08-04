/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2017 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include "http.h"
#include "xml.h"
#include "mkcert.h"
#include "client.h"
#include "errors.h"
#include "limits.h"

#include <Limelight.h>

#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#ifdef __3DS__
#include "uuid.h"
#else
#include <uuid/uuid.h>
#endif
#include <openssl/sha.h>
#include <openssl/aes.h>
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/err.h>

#define UNIQUE_FILE_NAME "uniqueid.dat"
#define P12_FILE_NAME "client.p12"

#define UNIQUEID_BYTES 8
#define UNIQUEID_CHARS (UNIQUEID_BYTES*2)
#define PAIRING_CHALLENGE_MAX_BYTES 64

static char unique_id[UNIQUEID_CHARS+1];
static X509 *cert;
static char cert_hex[8192];
static EVP_PKEY *privateKey;

const char* gs_error;

static char pair_error_buffer[256];

static bool cert_regenerated;

#define LEN_AS_HEX_STR(x) ((x) * 2 + 1)
#define SIZEOF_AS_HEX_STR(x) LEN_AS_HEX_STR(sizeof(x))

#define UUID_STRLEN 37

static int mkdirtree(const char* directory) {
  char buffer[PATH_MAX];
  char* p = buffer;

  // The passed in string could be a string literal
  // so we must copy it first
  strncpy(p, directory, PATH_MAX - 1);
  buffer[PATH_MAX - 1] = '\0';

  while (*p != 0) {
    // Find the end of the path element
    do {
      p++;
    } while (*p != 0 && *p != '/');

    char oldChar = *p;
    *p = 0;

    // Create the directory if it doesn't exist already
    if (mkdir(buffer, 0775) == -1 && errno != EEXIST) {
        return -1;
    }

    *p = oldChar;
  }

  return 0;
}

static int load_unique_id(const char* keyDirectory) {
  char uniqueFilePath[PATH_MAX];
  snprintf(uniqueFilePath, PATH_MAX, "%s/%s", keyDirectory, UNIQUE_FILE_NAME);

  FILE *fd = fopen(uniqueFilePath, "rb");
  bool loaded = false;
  if (fd != NULL) {
    loaded = fread(unique_id, 1, UNIQUEID_CHARS, fd) == UNIQUEID_CHARS;
    fclose(fd);
  }

  if (!loaded) {
    unsigned char random_id[UNIQUEID_BYTES];
    if (RAND_bytes(random_id, sizeof(random_id)) == 1) {
      for (int i = 0; i < UNIQUEID_BYTES; i++) {
        sprintf(unique_id + (i * 2), "%02x", random_id[i]);
      }
    } else {
      snprintf(unique_id, UNIQUEID_CHARS + 1, "0123456789ABCDEF");
    }

    fd = fopen(uniqueFilePath, "wb");
    if (fd == NULL) {
      gs_error = "Can't write unique client ID";
      return GS_FAILED;
    }

    if (fwrite(unique_id, 1, UNIQUEID_CHARS, fd) != UNIQUEID_CHARS) {
      fclose(fd);
      gs_error = "Can't write unique client ID";
      return GS_FAILED;
    }
    fclose(fd);
  }
  unique_id[UNIQUEID_CHARS] = 0;

  return GS_OK;
}

static int load_cert(const char* keyDirectory) {
  char certificateFilePath[PATH_MAX];
  snprintf(certificateFilePath, PATH_MAX, "%s/%s", keyDirectory, CERTIFICATE_FILE_NAME);

  char keyFilePath[PATH_MAX];
  snprintf(&keyFilePath[0], PATH_MAX, "%s/%s", keyDirectory, KEY_FILE_NAME);

  FILE *fd = fopen(certificateFilePath, "r");
  if (fd == NULL) {
    CERT_KEY_PAIR cert = mkcert_generate();

    char p12FilePath[PATH_MAX];
    snprintf(p12FilePath, PATH_MAX, "%s/%s", keyDirectory, P12_FILE_NAME);

    if (!mkcert_save(certificateFilePath, p12FilePath, keyFilePath, cert)) {
      mkcert_free(cert);
      gs_error = "Can't save generated certificate";
      return GS_FAILED;
    }
    mkcert_free(cert);
    cert_regenerated = true;
    fd = fopen(certificateFilePath, "r");
  }

  if (fd == NULL) {
    gs_error = "Can't open certificate file";
    return GS_FAILED;
  }

  if (!(cert = PEM_read_X509(fd, NULL, NULL, NULL))) {
    fclose(fd);
    gs_error = "Error loading cert into memory";
    return GS_FAILED;
  }

  rewind(fd);

  int c;
  int length = 0;
  while ((c = fgetc(fd)) != EOF) {
    if ((size_t)length + 2 >= sizeof(cert_hex)) {
      fclose(fd);
      gs_error = "Certificate is too large";
      return GS_FAILED;
    }
    sprintf(cert_hex + length, "%02x", c);
    length += 2;
  }
  cert_hex[length] = 0;

  fclose(fd);

  fd = fopen(keyFilePath, "r");
  if (fd == NULL) {
    gs_error = "Error loading key into memory";
    return GS_FAILED;
  }

  privateKey = PEM_read_PrivateKey(fd, NULL, NULL, NULL);
  fclose(fd);
  if (privateKey == NULL) {
    gs_error = "Error loading key into memory";
    return GS_FAILED;
  }

  return GS_OK;
}

static void clear_server_xml_fields(PSERVER_DATA server) {
  if (server == NULL)
    return;

  free(server->gpuType);
  server->gpuType = NULL;
  free(server->gsVersion);
  server->gsVersion = NULL;
  free((char*)server->serverInfo.serverInfoAppVersion);
  server->serverInfo.serverInfoAppVersion = NULL;
  free((char*)server->serverInfo.serverInfoGfeVersion);
  server->serverInfo.serverInfoGfeVersion = NULL;
  free((char*)server->serverInfo.rtspSessionUrl);
  server->serverInfo.rtspSessionUrl = NULL;
  xml_free_modelist(server->modes);
  server->modes = NULL;
}

static int load_serverinfo(PSERVER_DATA server, bool https) {
  uuid_t uuid;
  char uuid_str[UUID_STRLEN];
  char url[4096];
  int ret = GS_INVALID;
  char *pairedText = NULL;
  char *currentGameText = NULL;
  char *stateText = NULL;
  char *serverCodecModeSupportText = NULL;
  char *httpsPortText = NULL;

  clear_server_xml_fields(server);

  uuid_generate_random(uuid);
  uuid_unparse(uuid, uuid_str);

  snprintf(url, sizeof(url), "%s://%s:%d/serverinfo?uniqueid=%s&uuid=%s",
    https ? "https" : "http", server->serverInfo.address, https ? server->httpsPort : server->httpPort, unique_id, uuid_str);

  PHTTP_DATA data = http_create_data();
  if (data == NULL) {
    ret = GS_OUT_OF_MEMORY;
    goto cleanup;
  }
  if (http_request(url, data) != GS_OK) {
    ret = GS_IO_ERROR;
    goto cleanup;
  }

  if ((ret = xml_status(data->memory, data->size)) != GS_OK) {
    goto cleanup;
  }

  if (xml_search(data->memory, data->size, "currentgame", &currentGameText) != GS_OK) {
    goto cleanup;
  }

  if (xml_search(data->memory, data->size, "PairStatus", &pairedText) != GS_OK)
    goto cleanup;

  if (xml_search(data->memory, data->size, "appversion", (char**) &server->serverInfo.serverInfoAppVersion) != GS_OK)
    goto cleanup;

  if (xml_search(data->memory, data->size, "state", &stateText) != GS_OK)
    goto cleanup;

  if (xml_search(data->memory, data->size, "ServerCodecModeSupport", &serverCodecModeSupportText) != GS_OK)
    goto cleanup;

  if (xml_search(data->memory, data->size, "gputype", &server->gpuType) != GS_OK) {
    server->gpuType = strdup("Sunshine host");
    if (server->gpuType == NULL) {
      ret = GS_OUT_OF_MEMORY;
      goto cleanup;
    }
  }

  if (xml_search(data->memory, data->size, "GsVersion", &server->gsVersion) != GS_OK) {
    server->gsVersion = strdup(server->serverInfo.serverInfoAppVersion);
    if (server->gsVersion == NULL) {
      ret = GS_OUT_OF_MEMORY;
      goto cleanup;
    }
  }

  if (xml_search(data->memory, data->size, "GfeVersion", (char**) &server->serverInfo.serverInfoGfeVersion) != GS_OK)
    goto cleanup;

  if (xml_search(data->memory, data->size, "HttpsPort", &httpsPortText) != GS_OK)
    goto cleanup;

  if (xml_modelist(data->memory, data->size, &server->modes) != GS_OK)
    server->modes = NULL;

  // These fields are present on all version of GFE that this client supports
  if (!strlen(currentGameText) || !strlen(pairedText) || !strlen(server->serverInfo.serverInfoAppVersion) || !strlen(stateText))
    goto cleanup;

  server->paired = pairedText != NULL && strcmp(pairedText, "1") == 0;
  server->currentGame = currentGameText == NULL ? 0 : atoi(currentGameText);
  server->serverInfo.serverCodecModeSupport = serverCodecModeSupportText == NULL ? SCM_H264 : atoi(serverCodecModeSupportText);
  server->serverMajorVersion = atoi(server->serverInfo.serverInfoAppVersion);
  server->isNvidiaSoftware = strstr(stateText, "MJOLNIR") != NULL;

  server->httpsPort = atoi(httpsPortText);
  if (!server->httpsPort)
    server->httpsPort = 47984;

  if (strstr(stateText, "_SERVER_BUSY") == NULL) {
    // After GFE 2.8, current game remains set even after streaming
    // has ended. We emulate the old behavior by forcing it to zero
    // if streaming is not active.
    server->currentGame = 0;
  }
  ret = GS_OK;

  cleanup:
  if (data != NULL)
    http_free_data(data);

  if (pairedText != NULL)
    free(pairedText);

  if (currentGameText != NULL)
    free(currentGameText);

  if (serverCodecModeSupportText != NULL)
    free(serverCodecModeSupportText);

  if (httpsPortText != NULL)
    free(httpsPortText);

  if (stateText != NULL)
    free(stateText);

  if (ret != GS_OK)
    clear_server_xml_fields(server);

  return ret;
}

static int load_server_status(PSERVER_DATA server) {
  int ret;
  int i;

  /* Fetch the HTTPS port if we don't have one yet */
  if (!server->httpsPort) {
    ret = load_serverinfo(server, false);
    if (ret != GS_OK)
      return ret;
  }

  if (!server->isNvidiaSoftware)
    return GS_OK;

  ret = GS_INVALID;
  for (i = 0; i < 2 && ret != GS_OK; i++) {
    ret = load_serverinfo(server, i == 0);
  }

  if (ret == GS_OK && !server->unsupported) {
    if (server->serverMajorVersion > MAX_SUPPORTED_GFE_VERSION) {
      gs_error = "Ensure you're running the latest version of Moonlight Embedded or downgrade GeForce Experience and try again";
      ret = GS_UNSUPPORTED_VERSION;
    } else if (server->serverMajorVersion < MIN_SUPPORTED_GFE_VERSION) {
      gs_error = "Moonlight Embedded requires a newer version of GeForce Experience. Please upgrade GFE on your PC and try again.";
      ret = GS_UNSUPPORTED_VERSION;
    }
  }

  return ret;
}

static void bytes_to_hex(const unsigned char *in, char *out, size_t len) {
  for (int i = 0; i < len; i++) {
    sprintf(out + i * 2, "%02x", in[i]);
  }
  out[len * 2] = 0;
}

static bool is_hex_string(const char *in) {
  if (in == NULL || in[0] == '\0')
    return false;

  for (size_t i = 0; in[i] != '\0'; i++) {
    if (!isxdigit((unsigned char)in[i]))
      return false;
  }
  return true;
}

static bool hex_to_bytes(const char *in, unsigned char* out, size_t len) {
  if (in == NULL || out == NULL || (len % 2) != 0 || !is_hex_string(in))
    return false;

  for (int count = 0; count < len; count += 2) {
    if (sscanf(&in[count], "%2hhx", &out[count / 2]) != 1)
      return false;
  }
  return true;
}

static int sign_it(const char *msg, size_t mlen, unsigned char **sig, size_t *slen, EVP_PKEY *pkey) {
  int result = GS_FAILED;

  *sig = NULL;
  *slen = 0;

  if (msg == NULL || sig == NULL || slen == NULL || pkey == NULL)
    return GS_FAILED;

  EVP_MD_CTX *ctx = EVP_MD_CTX_create();
  if (ctx == NULL)
    return GS_FAILED;

  int rc = EVP_DigestSignInit(ctx, NULL, EVP_sha256(), NULL, pkey);
  if (rc != 1)
    goto cleanup;

  rc = EVP_DigestSignUpdate(ctx, msg, mlen);
  if (rc != 1)
    goto cleanup;

  size_t req = 0;
  rc = EVP_DigestSignFinal(ctx, NULL, &req);
  if (rc != 1 || !(req > 0))
    goto cleanup;

  *sig = OPENSSL_malloc(req);
  if (*sig == NULL)
    goto cleanup;

  *slen = req;
  rc = EVP_DigestSignFinal(ctx, *sig, slen);
  if (rc != 1 || req != *slen)
    goto cleanup;

  result = GS_OK;

  cleanup:
  if (result != GS_OK && *sig != NULL) {
    OPENSSL_free(*sig);
    *sig = NULL;
    *slen = 0;
  }
  EVP_MD_CTX_destroy(ctx);
  ctx = NULL;

  return result;
}

static bool verifySignature(const char *data, int dataLength, char *signature, int signatureLength, const char *cert) {
    if (data == NULL || signature == NULL || cert == NULL ||
        dataLength <= 0 || signatureLength <= 0) {
        return false;
    }

    X509* x509;
    BIO* bio = BIO_new(BIO_s_mem());
    if (!bio) {
        return false;
    }
    BIO_puts(bio, cert);
    x509 = PEM_read_bio_X509(bio, NULL, NULL, NULL);

    BIO_free(bio);

    if (!x509) {
        return false;
    }

    EVP_PKEY* pubKey = X509_get_pubkey(x509);
    if (!pubKey) {
        X509_free(x509);
        return false;
    }
    EVP_MD_CTX *mdctx = NULL;
    mdctx = EVP_MD_CTX_create();
    if (!mdctx) {
        X509_free(x509);
        EVP_PKEY_free(pubKey);
        return false;
    }

    int result = 0;
    if (EVP_DigestVerifyInit(mdctx, NULL, EVP_sha256(), NULL, pubKey) == 1 &&
        EVP_DigestVerifyUpdate(mdctx, data, dataLength) == 1) {
        result = EVP_DigestVerifyFinal(mdctx, signature, signatureLength);
    }

    X509_free(x509);
    EVP_PKEY_free(pubKey);
    EVP_MD_CTX_destroy(mdctx);

    return result > 0;
}

static int encrypt(const unsigned char *plaintext, int plaintextLen, const unsigned char *key, unsigned char *ciphertext) {
  EVP_CIPHER_CTX* cipher = EVP_CIPHER_CTX_new();
  if (cipher == NULL)
    return GS_FAILED;

  if (EVP_EncryptInit(cipher, EVP_aes_128_ecb(), key, NULL) != 1 ||
      EVP_CIPHER_CTX_set_padding(cipher, 0) != 1) {
    EVP_CIPHER_CTX_free(cipher);
    return GS_FAILED;
  }

  int ciphertextLen = 0;
  if (EVP_EncryptUpdate(cipher, ciphertext, &ciphertextLen, plaintext, plaintextLen) != 1) {
    EVP_CIPHER_CTX_free(cipher);
    return GS_FAILED;
  }

  EVP_CIPHER_CTX_free(cipher);
  return GS_OK;
}

static int decrypt(const unsigned char *ciphertext, int ciphertextLen, const unsigned char *key, unsigned char *plaintext) {
  EVP_CIPHER_CTX* cipher = EVP_CIPHER_CTX_new();
  if (cipher == NULL)
    return GS_FAILED;

  if (EVP_DecryptInit(cipher, EVP_aes_128_ecb(), key, NULL) != 1 ||
      EVP_CIPHER_CTX_set_padding(cipher, 0) != 1) {
    EVP_CIPHER_CTX_free(cipher);
    return GS_FAILED;
  }

  int plaintextLen = 0;
  if (EVP_DecryptUpdate(cipher, plaintext, &plaintextLen, ciphertext, ciphertextLen) != 1) {
    EVP_CIPHER_CTX_free(cipher);
    return GS_FAILED;
  }

  EVP_CIPHER_CTX_free(cipher);
  return GS_OK;
}

int gs_unpair(PSERVER_DATA server) {
  int ret = GS_OK;
  char url[4096];
  uuid_t uuid;
  char uuid_str[UUID_STRLEN];
  if (server == NULL) {
    gs_error = "Invalid server data";
    return GS_INVALID;
  }

  PHTTP_DATA data = http_create_data();
  if (data == NULL)
    return GS_OUT_OF_MEMORY;

  uuid_generate_random(uuid);
  uuid_unparse(uuid, uuid_str);
  snprintf(url, sizeof(url), "http://%s:%u/unpair?uniqueid=%s&uuid=%s", server->serverInfo.address, server->httpPort, unique_id, uuid_str);
  ret = http_request(url, data);

  http_free_data(data);
  return ret;
}

int gs_pair(PSERVER_DATA server, char* pin) {
  int ret = GS_OK;
  const char *pair_stage = "Pairing initialization failed";
  char* result = NULL;
  size_t url_max_len = 16384;
  char* url = malloc(url_max_len);
  uuid_t uuid;
  char uuid_str[UUID_STRLEN];
  char* plaincert = NULL;
  char* challenge_response = NULL;
  char* pairing_secret = NULL;
  char* client_pairing_secret = NULL;
  char* client_pairing_secret_hex = NULL;
  unsigned char *signature = NULL;
  PHTTP_DATA data = NULL;

  if (server == NULL || pin == NULL) {
    gs_error = "Invalid pairing request";
    ret = GS_INVALID;
    goto cleanup;
  }

  if (url == NULL) {
    gs_error = "Not enough memory for pairing request";
    ret = GS_OUT_OF_MEMORY;
    goto cleanup;
  }

  if (server->paired) {
    gs_error = "Already paired";
    ret = GS_WRONG_STATE;
    goto cleanup;
  }

  unsigned char salt_data[16];
  char salt_hex[SIZEOF_AS_HEX_STR(salt_data)];
  pair_stage = "Failed before requesting the server certificate";
  if (RAND_bytes(salt_data, sizeof(salt_data)) != 1) {
    gs_error = "Failed to generate pairing salt";
    ret = GS_FAILED;
    goto cleanup;
  }
  bytes_to_hex(salt_data, salt_hex, sizeof(salt_data));

  uuid_generate_random(uuid);
  uuid_unparse(uuid, uuid_str);
  pair_stage = "Failed while requesting the server certificate";
  snprintf(url, url_max_len, "http://%s:%u/pair?uniqueid=%s&uuid=%s&devicename=roth&updateState=1&phrase=getservercert&salt=%s&clientcert=%s", server->serverInfo.address, server->httpPort, unique_id, uuid_str, salt_hex, cert_hex);
  data = http_create_data();
  if (data == NULL) {
    gs_error = "Not enough memory for pairing HTTP data";
    ret = GS_OUT_OF_MEMORY;
    goto cleanup;
  } else if ((ret = http_request(url, data)) != GS_OK)
    goto cleanup;

  if ((ret = xml_status(data->memory, data->size)) != GS_OK)
    goto cleanup;
  else if ((ret = xml_search(data->memory, data->size, "paired", &result)) != GS_OK)
    goto cleanup;

  if (strcmp(result, "1") != 0) {
    gs_error = "Pairing failed";
    ret = GS_FAILED;
    goto cleanup;
  }

  free(result);
  result = NULL;
  if ((ret = xml_search(data->memory, data->size, "plaincert", &result)) != GS_OK)
    goto cleanup;

  size_t plaincertlen = strlen(result)/2;
  if ((strlen(result) % 2) != 0 || plaincertlen == 0) {
    gs_error = "Invalid server certificate";
    ret = GS_INVALID;
    goto cleanup;
  }
  plaincert = malloc(plaincertlen + 1);
  if (plaincert == NULL) {
    ret = GS_OUT_OF_MEMORY;
    goto cleanup;
  }
  if (!hex_to_bytes(result, (unsigned char*)plaincert, plaincertlen*2)) {
    gs_error = "Invalid server certificate";
    ret = GS_INVALID;
    goto cleanup;
  }
  plaincert[plaincertlen] = 0;

  unsigned char salt_pin[sizeof(salt_data) + 4];
  unsigned char aes_key[32]; // Must fit SHA256
  memcpy(salt_pin, salt_data, sizeof(salt_data));
  memcpy(salt_pin+sizeof(salt_data), pin, 4);

  int hash_length = server->serverMajorVersion >= 7 ? 32 : 20;
  if (server->serverMajorVersion >= 7)
    SHA256(salt_pin, sizeof(salt_pin), aes_key);
  else
    SHA1(salt_pin, sizeof(salt_pin), aes_key);

  unsigned char challenge_data[16];
  unsigned char challenge_enc[sizeof(challenge_data)];
  char challenge_hex[SIZEOF_AS_HEX_STR(challenge_enc)];
  pair_stage = "Failed before sending the client challenge";
  if (RAND_bytes(challenge_data, sizeof(challenge_data)) != 1) {
    gs_error = "Failed to generate pairing challenge";
    ret = GS_FAILED;
    goto cleanup;
  }
  if (encrypt(challenge_data, sizeof(challenge_data), aes_key, challenge_enc) != GS_OK) {
    gs_error = "Failed to encrypt pairing challenge";
    ret = GS_FAILED;
    goto cleanup;
  }
  bytes_to_hex(challenge_enc, challenge_hex, sizeof(challenge_enc));

  uuid_generate_random(uuid);
  uuid_unparse(uuid, uuid_str);
  pair_stage = "Failed while sending the client challenge";
  snprintf(url, url_max_len, "http://%s:%u/pair?uniqueid=%s&uuid=%s&devicename=roth&updateState=1&clientchallenge=%s", server->serverInfo.address, server->httpPort, unique_id, uuid_str, challenge_hex);
  if ((ret = http_request(url, data)) != GS_OK)
    goto cleanup;

  free(result);
  result = NULL;
  if ((ret = xml_status(data->memory, data->size)) != GS_OK)
    goto cleanup;
  else if ((ret = xml_search(data->memory, data->size, "paired", &result)) != GS_OK)
    goto cleanup;

  if (strcmp(result, "1") != 0) {
    gs_error = "Pairing failed";
    ret = GS_FAILED;
    goto cleanup;
  }

  free(result);
  result = NULL;
  if (xml_search(data->memory, data->size, "challengeresponse", &result) != GS_OK) {
    ret = GS_INVALID;
    goto cleanup;
  }

  unsigned char challenge_response_data_enc[PAIRING_CHALLENGE_MAX_BYTES] = {0};
  unsigned char challenge_response_data[sizeof(challenge_response_data_enc)] = {0};
  size_t challenge_response_len = strlen(result) / 2;

  if ((strlen(result) % 2) != 0 ||
      challenge_response_len < (size_t)hash_length + sizeof(challenge_data) ||
      challenge_response_len > sizeof(challenge_response_data_enc) ||
      (challenge_response_len % 16) != 0) {
    gs_error = "Invalid server challenge response";
    ret = GS_FAILED;
    goto cleanup;
  }

  if (!hex_to_bytes(result, challenge_response_data_enc, strlen(result)) ||
      decrypt(challenge_response_data_enc, challenge_response_len, aes_key, challenge_response_data) != GS_OK) {
    gs_error = "Failed to decode server challenge response";
    ret = GS_FAILED;
    goto cleanup;
  }

  char client_secret_data[16];
  if (RAND_bytes((unsigned char*)client_secret_data, sizeof(client_secret_data)) != 1) {
    gs_error = "Failed to generate client pairing secret";
    ret = GS_FAILED;
    goto cleanup;
  }

  const ASN1_BIT_STRING *asnSignature = NULL;
  X509_get0_signature(&asnSignature, NULL, cert);
  if (asnSignature == NULL || asnSignature->data == NULL || asnSignature->length <= 0) {
    gs_error = "Invalid client certificate signature";
    ret = GS_FAILED;
    goto cleanup;
  }

  challenge_response = malloc(16 + asnSignature->length + sizeof(client_secret_data));
  if (challenge_response == NULL) {
    ret = GS_OUT_OF_MEMORY;
    goto cleanup;
  }
  char challenge_response_hash[32];
  char challenge_response_hash_enc[sizeof(challenge_response_hash)];
  char challenge_response_hex[SIZEOF_AS_HEX_STR(challenge_response_hash_enc)];
  memcpy(challenge_response, challenge_response_data + hash_length, 16);
  memcpy(challenge_response + 16, asnSignature->data, asnSignature->length);
  memcpy(challenge_response + 16 + asnSignature->length, client_secret_data, sizeof(client_secret_data));
  if (server->serverMajorVersion >= 7)
    SHA256(challenge_response, 16 + asnSignature->length + sizeof(client_secret_data), challenge_response_hash);
  else
    SHA1(challenge_response, 16 + asnSignature->length + sizeof(client_secret_data), challenge_response_hash);

  if (encrypt((unsigned char*)challenge_response_hash, sizeof(challenge_response_hash), aes_key, (unsigned char*)challenge_response_hash_enc) != GS_OK) {
    gs_error = "Failed to encrypt pairing response";
    ret = GS_FAILED;
    goto cleanup;
  }
  bytes_to_hex(challenge_response_hash_enc, challenge_response_hex, sizeof(challenge_response_hash_enc));

  uuid_generate_random(uuid);
  uuid_unparse(uuid, uuid_str);
  pair_stage = "Failed while sending the server challenge response";
  snprintf(url, url_max_len, "http://%s:%u/pair?uniqueid=%s&uuid=%s&devicename=roth&updateState=1&serverchallengeresp=%s", server->serverInfo.address, server->httpPort, unique_id, uuid_str, challenge_response_hex);
  if ((ret = http_request(url, data)) != GS_OK)
    goto cleanup;

  free(result);
  result = NULL;
  if ((ret = xml_status(data->memory, data->size)) != GS_OK)
    goto cleanup;
  else if ((ret = xml_search(data->memory, data->size, "paired", &result)) != GS_OK)
    goto cleanup;

  if (strcmp(result, "1") != 0) {
    gs_error = "Pairing failed";
    ret = GS_FAILED;
    goto cleanup;
  }

  free(result);
  result = NULL;
  if (xml_search(data->memory, data->size, "pairingsecret", &result) != GS_OK) {
    ret = GS_INVALID;
    goto cleanup;
  }

  size_t pairing_secret_len = strlen(result) / 2;
  if ((strlen(result) % 2) != 0 || pairing_secret_len <= 16) {
    gs_error = "Invalid pairing secret";
    ret = GS_INVALID;
    goto cleanup;
  }

  pairing_secret = malloc(pairing_secret_len);
  if (pairing_secret == NULL) {
    ret = GS_OUT_OF_MEMORY;
    goto cleanup;
  }
  if (!hex_to_bytes(result, (unsigned char*)pairing_secret, pairing_secret_len*2)) {
    gs_error = "Invalid pairing secret";
    ret = GS_INVALID;
    goto cleanup;
  }
  if (!verifySignature(pairing_secret, 16, pairing_secret+16, pairing_secret_len-16, plaincert)) {
    gs_error = "MITM attack detected";
    ret = GS_FAILED;
    goto cleanup;
  }

  size_t s_len;
  if (sign_it(client_secret_data, sizeof(client_secret_data), &signature, &s_len, privateKey) != GS_OK) {
      gs_error = "Failed to sign data";
      ret = GS_FAILED;
      goto cleanup;
  }

  client_pairing_secret = malloc(sizeof(client_secret_data) + s_len);
  client_pairing_secret_hex = malloc(LEN_AS_HEX_STR(sizeof(client_secret_data) + s_len));
  if (client_pairing_secret == NULL || client_pairing_secret_hex == NULL) {
    ret = GS_OUT_OF_MEMORY;
    goto cleanup;
  }
  memcpy(client_pairing_secret, client_secret_data, sizeof(client_secret_data));
  memcpy(client_pairing_secret + sizeof(client_secret_data), signature, s_len);
  bytes_to_hex((unsigned char*)client_pairing_secret, client_pairing_secret_hex, sizeof(client_secret_data) + s_len);

  uuid_generate_random(uuid);
  uuid_unparse(uuid, uuid_str);
  pair_stage = "Failed while sending the client pairing secret";
  snprintf(url, url_max_len, "http://%s:%u/pair?uniqueid=%s&uuid=%s&devicename=roth&updateState=1&clientpairingsecret=%s", server->serverInfo.address, server->httpPort, unique_id, uuid_str, client_pairing_secret_hex);
  if ((ret = http_request(url, data)) != GS_OK)
    goto cleanup;

  free(result);
  result = NULL;
  if ((ret = xml_status(data->memory, data->size)) != GS_OK)
    goto cleanup;
  else if ((ret = xml_search(data->memory, data->size, "paired", &result)) != GS_OK)
    goto cleanup;

  if (strcmp(result, "1") != 0) {
    gs_error = "Pairing failed";
    ret = GS_FAILED;
    goto cleanup;
  }

  if (!server->isNvidiaSoftware) {
    server->paired = true;
    ret = GS_OK;
    goto cleanup;
  }

  uuid_generate_random(uuid);
  uuid_unparse(uuid, uuid_str);
  pair_stage = "Failed while confirming NVIDIA pairing over HTTPS";
  snprintf(url, url_max_len, "https://%s:%u/pair?uniqueid=%s&uuid=%s&devicename=roth&updateState=1&phrase=pairchallenge", server->serverInfo.address, server->httpsPort, unique_id, uuid_str);
  if ((ret = http_request(url, data)) != GS_OK)
    goto cleanup;

  free(result);
  result = NULL;
  if ((ret = xml_status(data->memory, data->size)) != GS_OK)
    goto cleanup;
  else if ((ret = xml_search(data->memory, data->size, "paired", &result)) != GS_OK)
    goto cleanup;

  if (strcmp(result, "1") != 0) {
    gs_error = "Pairing failed";
    ret = GS_FAILED;
    goto cleanup;
  }

  server->paired = true;

  cleanup:
  if (ret != GS_OK) {
    if (gs_error != NULL && gs_error[0] != '\0' && gs_error != pair_stage)
      snprintf(pair_error_buffer, sizeof(pair_error_buffer), "%s (%s)",
               pair_stage, gs_error);
    else
      snprintf(pair_error_buffer, sizeof(pair_error_buffer), "%s", pair_stage);
    gs_error = pair_error_buffer;
  }
  const char *pair_error = gs_error;

  if (ret != GS_OK && server != NULL) {
    gs_unpair(server);
    gs_error = pair_error;
  }

  free(url);
  free(plaincert);
  free(challenge_response);
  free(pairing_secret);
  free(client_pairing_secret);
  free(client_pairing_secret_hex);
  free(result);
  if (signature != NULL)
    OPENSSL_free(signature);

  http_free_data(data);

  // If we failed when attempting to pair with a game running, that's likely the issue.
  // Sunshine supports pairing with an active session, but GFE does not.
  if (ret != GS_OK && server != NULL && server->currentGame != 0) {
    gs_error = "The computer is currently in a game. You must close the game before pairing.";
    ret = GS_WRONG_STATE;
  }

  return ret;
}

int gs_applist(PSERVER_DATA server, PAPP_LIST *list) {
  int ret = GS_OK;
  char url[4096];
  uuid_t uuid;
  char uuid_str[UUID_STRLEN];
  if (server == NULL || list == NULL) {
    gs_error = "Invalid app list request";
    return GS_INVALID;
  }

  PHTTP_DATA data = http_create_data();
  if (data == NULL)
    return GS_OUT_OF_MEMORY;

  uuid_generate_random(uuid);
  uuid_unparse(uuid, uuid_str);
  snprintf(url, sizeof(url), "https://%s:%u/applist?uniqueid=%s&uuid=%s", server->serverInfo.address, server->httpsPort, unique_id, uuid_str);
  if ((ret = http_request(url, data)) != GS_OK)
    ret = GS_IO_ERROR;
  else if ((ret = xml_status(data->memory, data->size)) != GS_OK)
    ret = ret == GS_ERROR ? GS_ERROR : ret;
  else if (xml_applist(data->memory, data->size, list) != GS_OK)
    ret = GS_INVALID;

  http_free_data(data);
  return ret;
}

int gs_start_app(PSERVER_DATA server, STREAM_CONFIGURATION *config, int appId, bool sops, bool localaudio, int gamepad_mask) {
  int ret = GS_OK;
  uuid_t uuid;
  char* result = NULL;
  char uuid_str[UUID_STRLEN];

  if (server == NULL || config == NULL) {
    gs_error = "Invalid stream launch request";
    return GS_INVALID;
  }

  PDISPLAY_MODE mode = server->modes;
  bool correct_mode = server->modes == NULL;
  while (mode != NULL) {
    if (mode->width == config->width && mode->height == config->height) {
      if (mode->refresh == config->fps)
        correct_mode = true;
    }

    mode = mode->next;
  }

  if (!correct_mode && !server->unsupported)
    return GS_NOT_SUPPORTED_MODE;

  RAND_bytes(config->remoteInputAesKey, sizeof(config->remoteInputAesKey));
  memset(config->remoteInputAesIv, 0, sizeof(config->remoteInputAesIv));

  char url[4096];
  u_int32_t rikeyid = 0;
  RAND_bytes(config->remoteInputAesIv, sizeof(rikeyid));
  memcpy(&rikeyid, config->remoteInputAesIv, sizeof(rikeyid));
  rikeyid = htonl(rikeyid);
  char rikey_hex[SIZEOF_AS_HEX_STR(config->remoteInputAesKey)];
  bytes_to_hex(config->remoteInputAesKey, rikey_hex, sizeof(config->remoteInputAesKey));

  PHTTP_DATA data = http_create_data();
  if (data == NULL)
    return GS_OUT_OF_MEMORY;

  // Using an FPS value over 60 causes SOPS to default to 720p60,
  // so force it to 0 to ensure the correct resolution is set. We
  // used to use 60 here but that locked the frame rate to 60 FPS
  // on GFE 3.20.3.
  int fps = (server->isNvidiaSoftware && config->fps > 60) ? 0 : config->fps;

  uuid_generate_random(uuid);
  uuid_unparse(uuid, uuid_str);
  int surround_info = SURROUNDAUDIOINFO_FROM_AUDIO_CONFIGURATION(config->audioConfiguration);
  snprintf(url, sizeof(url), "https://%s:%u/%s?uniqueid=%s&uuid=%s&appid=%d&mode=%dx%dx%d&additionalStates=1&sops=%d&rikey=%s&rikeyid=%d&localAudioPlayMode=%d&surroundAudioInfo=%d&remoteControllersBitmap=%d&gcmap=%d%s%s",
           server->serverInfo.address, server->httpsPort, server->currentGame ? "resume" : "launch", unique_id, uuid_str, appId, config->width, config->height, fps, sops, rikey_hex, rikeyid, localaudio, surround_info, gamepad_mask, gamepad_mask,
           (config->supportedVideoFormats & VIDEO_FORMAT_MASK_10BIT) ? "&hdrMode=1&clientHdrCapVersion=0&clientHdrCapSupportedFlagsInUint32=0&clientHdrCapMetaDataId=NV_STATIC_METADATA_TYPE_1&clientHdrCapDisplayData=0x0x0x0x0x0x0x0x0x0x0" : "",
           LiGetLaunchUrlQueryParameters());
  if ((ret = http_request(url, data)) == GS_OK)
    server->currentGame = appId;
  else
    goto cleanup;

  if ((ret = xml_status(data->memory, data->size)) != GS_OK)
    goto cleanup;
  else if ((ret = xml_search(data->memory, data->size, "gamesession", &result)) != GS_OK &&
           (ret = xml_search(data->memory, data->size, "resume", &result)) != GS_OK)
    goto cleanup;

  if (!strcmp(result, "0")) {
    ret = GS_FAILED;
    goto cleanup;
  }

  free(result);
  result = NULL;

  if (xml_search(data->memory, data->size, "sessionUrl0", &result) == GS_OK) {
    server->serverInfo.rtspSessionUrl = result;
    result = NULL;
  }

  cleanup:
  if (result != NULL)
    free(result);

  http_free_data(data);
  return ret;
}

int gs_quit_app(PSERVER_DATA server) {
  int ret = GS_OK;
  char url[4096];
  uuid_t uuid;
  char uuid_str[UUID_STRLEN];
  char* result = NULL;
  if (server == NULL) {
    gs_error = "Invalid server data";
    return GS_INVALID;
  }

  PHTTP_DATA data = http_create_data();
  if (data == NULL)
    return GS_OUT_OF_MEMORY;

  uuid_generate_random(uuid);
  uuid_unparse(uuid, uuid_str);
  snprintf(url, sizeof(url), "https://%s:%u/cancel?uniqueid=%s&uuid=%s", server->serverInfo.address, server->httpsPort, unique_id, uuid_str);
  if ((ret = http_request(url, data)) != GS_OK)
    goto cleanup;

  if ((ret = xml_status(data->memory, data->size)) != GS_OK)
    goto cleanup;
  else if ((ret = xml_search(data->memory, data->size, "cancel", &result)) != GS_OK)
    goto cleanup;

  if (strcmp(result, "0") == 0) {
    ret = GS_FAILED;
    goto cleanup;
  }

  cleanup:
  if (result != NULL)
    free(result);

  http_free_data(data);
  return ret;
}

bool gs_cert_was_regenerated() {
  return cert_regenerated;
}

int gs_init(PSERVER_DATA server, char *address, unsigned short httpPort, const char *keyDirectory, int log_level, bool unsupported) {
  if (server == NULL || address == NULL || keyDirectory == NULL) {
    gs_error = "Invalid server initialization request";
    return GS_INVALID;
  }

  if (mkdirtree(keyDirectory) != 0) {
    gs_error = "Can't create key directory";
    return GS_FAILED;
  }
  if (load_unique_id(keyDirectory) != GS_OK) {
    if (gs_error == NULL || gs_error[0] == '\0')
      gs_error = "Can't load unique client ID";
    return GS_FAILED;
  }

  if (load_cert(keyDirectory)) {
    if (gs_error == NULL || gs_error[0] == '\0')
      gs_error = "Can't load client certificate";
    return GS_FAILED;
  }

  if (http_init(keyDirectory, log_level) != GS_OK) {
    gs_error = "Can't initialize HTTP client";
    return GS_FAILED;
  }

  LiInitializeServerInformation(&server->serverInfo);
  server->serverInfo.address = address;
  server->unsupported = unsupported;
  server->httpPort = httpPort ? httpPort : 47989;
  server->httpsPort = 0; /* Populated by load_server_status() */
  int status = load_server_status(server);
  if (status != GS_OK && (gs_error == NULL || gs_error[0] == '\0'))
    gs_error = "Host status query failed";
  return status;
}

void gs_cleanup() {
  if (cert != NULL) {
    X509_free(cert);
    cert = NULL;
  }
  if (privateKey != NULL)
  {
    EVP_PKEY_free(privateKey);
    // Set to NULL to avoid reconnection issues
    privateKey = NULL;
  }
  http_cleanup();
}
