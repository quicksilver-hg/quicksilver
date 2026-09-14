// Copyright (c) 2019-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_VAULT_EXTERNAL_SIGNER_SCRIPTPUBKEYMAN_H
#define QUICKSILVER_VAULT_EXTERNAL_SIGNER_SCRIPTPUBKEYMAN_H

#include <vault/scriptpubkeyman.h>

#include <memory>

struct bilingual_str;

namespace vault {
class ExternalSignerScriptPubKeyMan : public DescriptorScriptPubKeyMan
{
  public:
  ExternalSignerScriptPubKeyMan(VaultStorage& storage, VaultDescriptor& descriptor, int64_t keypool_size)
      :   DescriptorScriptPubKeyMan(storage, descriptor, keypool_size)
      {}
  ExternalSignerScriptPubKeyMan(VaultStorage& storage, int64_t keypool_size)
      :   DescriptorScriptPubKeyMan(storage, keypool_size)
      {}

  /** Provide a descriptor at setup time
  * Returns false if already setup or setup fails, true if setup is successful
  */
  bool SetupDescriptor(VaultBatch& batch, std::unique_ptr<Descriptor>desc);

  static ExternalSigner GetExternalSigner();

  /**
  * Display address on the device and verify that the returned value matches.
  * @returns nothing or an error message
  */
 util::Result<void> DisplayAddress(const CTxDestination& dest, const ExternalSigner& signer) const;

  std::optional<common::PSQTError> FillPSQT(PartiallySignedQuicksilverTransaction& psqt, const PrecomputedTransactionData& txdata, int sighash_type = 1 /* SIGHASH_ALL */, bool sign = true, bool bip32derivs = false, int* n_signed = nullptr, bool finalize = true) const override;
};
} // namespace vault
#endif // QUICKSILVER_VAULT_EXTERNAL_SIGNER_SCRIPTPUBKEYMAN_H
