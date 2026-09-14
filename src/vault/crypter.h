// Copyright (c) 2009-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_VAULT_CRYPTER_H
#define QUICKSILVER_VAULT_CRYPTER_H

#include <serialize.h>
#include <support/allocators/secure.h>
#include <script/signingprovider.h>


namespace vault {
const unsigned int VAULT_CRYPTO_KEY_SIZE = 32;
const unsigned int VAULT_CRYPTO_SALT_SIZE = 8;
const unsigned int VAULT_CRYPTO_IV_SIZE = 16;

/**
 * Private key encryption is done based on a CMasterKey,
 * which holds a salt and random encryption key.
 *
 * CMasterKeys are encrypted using AES-256-CBC using a key
 * derived with SHA-512 (EVP_BytesToKey equivalent) and
 * derivation iterations nDeriveIterations.
 *
 * Vault Private Keys are then encrypted using AES-256-CBC
 * with the double-sha256 of the public key as the IV, and the
 * master key's key as the encryption key (see keystore.[ch]).
 */

/** Master key for vault encryption */
class CMasterKey
{
public:
    std::vector<unsigned char> vchCryptedKey;
    std::vector<unsigned char> vchSalt;
    unsigned int nDeriveIterations;

    SERIALIZE_METHODS(CMasterKey, obj)
    {
        READWRITE(obj.vchCryptedKey, obj.vchSalt, obj.nDeriveIterations);
        SER_READ(obj, if (obj.nDeriveIterations == 0) throw std::ios_base::failure(
            "Pre-Quicksilver master key record; this vault cannot be opened"));
    }

    CMasterKey()
    {
        // 25000 rounds is just under 0.1 seconds on a 1.86 GHz Pentium M
        // ie slightly lower than the lowest hardware we need bother supporting
        nDeriveIterations = 25000;
    }
};

typedef std::vector<unsigned char, secure_allocator<unsigned char> > CKeyingMaterial;

namespace vault_crypto_tests
{
    class TestCrypter;
}

/** Encryption/decryption context with key information */
class CCrypter
{
friend class vault_crypto_tests::TestCrypter; // for test access to chKey/chIV
private:
    std::vector<unsigned char, secure_allocator<unsigned char>> vchKey;
    std::vector<unsigned char, secure_allocator<unsigned char>> vchIV;
    bool fKeySet;

    int BytesToKeySHA512AES(std::span<const unsigned char> salt, const SecureString& key_data, int count, unsigned char* key, unsigned char* iv) const;

public:
    bool SetKeyFromPassphrase(const SecureString& key_data, std::span<const unsigned char> salt, const unsigned int rounds);
    bool Encrypt(const CKeyingMaterial& vchPlaintext, std::vector<unsigned char> &vchCiphertext) const;
    bool Decrypt(std::span<const unsigned char> ciphertext, CKeyingMaterial& plaintext) const;
    bool SetKey(const CKeyingMaterial& new_key, std::span<const unsigned char> new_iv);

    void CleanKey()
    {
        memory_cleanse(vchKey.data(), vchKey.size());
        memory_cleanse(vchIV.data(), vchIV.size());
        fKeySet = false;
    }

    CCrypter()
    {
        fKeySet = false;
        vchKey.resize(VAULT_CRYPTO_KEY_SIZE);
        vchIV.resize(VAULT_CRYPTO_IV_SIZE);
    }

    ~CCrypter()
    {
        CleanKey();
    }
};

bool EncryptSecret(const CKeyingMaterial& vMasterKey, const CKeyingMaterial &vchPlaintext, const uint256& nIV, std::vector<unsigned char> &vchCiphertext);
bool DecryptSecret(const CKeyingMaterial& master_key, std::span<const unsigned char> ciphertext, const uint256& iv, CKeyingMaterial& plaintext);
bool DecryptKey(const CKeyingMaterial& master_key, std::span<const unsigned char> crypted_secret, const CPubKey& pub_key, CKey& key);
} // namespace vault

#endif // QUICKSILVER_VAULT_CRYPTER_H
