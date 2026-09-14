#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the encrypted vault archive: exportvaultarchive / importvaultarchive.

A thin vault cannot rescan, so the vault file is otherwise the only record of what was
sent, to whom, and when. A current vault-file backup preserves both spending keys and
history; `scantxoutset` cannot recover lost keys or past activity. This history-only
archive closes the metadata gap without copying private keys, so this test proves:

  1. history exists in a vault,
  2. it is exported to an encrypted file,
  3. a brand new vault that has never seen the chain imports it,
  4. the restored history matches, transaction for transaction.

See doc/design/vault-backup.md.
"""
import os

from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
)


class VaultArchiveTest(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_vault()

    def run_test(self):
        node = self.nodes[0]
        passphrase = "correct horse battery staple"
        archive_path = os.path.join(self.options.tmpdir, "history.qsva")

        self.log.info("Build a vault with some history")
        node.createvault(vault_name="source")
        source = node.get_vault_rpc("source")
        address = source.getnewaddress()
        self.generatetoaddress(node, 101, address)

        # Two sends, so the archive carries more than a single coinbase.
        for _ in range(2):
            source.sendtoaddress(source.getnewaddress(), 1)
            self.generate(node, 1)

        # A labelled destination, so the restored address book can be checked for content
        # rather than just for size.
        labelled = source.getnewaddress(label="a paid counterparty")
        source.sendtoaddress(labelled, 1)
        self.generate(node, 1)

        # listtransactions returns one row per category, so a self-send appears twice, and
        # it truncates to `count` rows. Compare the whole history as a set of txids: a
        # windowed comparison would only prove that two differently-ordered lists happen
        # to overlap.
        def history_txids(rpc):
            return sorted({entry["txid"] for entry in rpc.listtransactions("*", 100000)})

        expected_txids = history_txids(source)
        assert_greater_than(len(expected_txids), 2)

        self.log.info("Export the history to an encrypted archive")
        result = source.exportvaultarchive(archive_path, passphrase)
        assert_equal(result["path"], archive_path)
        assert_greater_than(result["transactions"], 0)
        assert_greater_than(result["descriptors"], 0)
        assert_greater_than(result["addresses"], 0)
        assert os.path.isfile(archive_path)

        self.log.info("The archive is encrypted: the passphrase is not optional")
        assert_raises_rpc_error(-4, "Vault archive passphrase is incorrect.",
                                source.importvaultarchive, archive_path, "wrong passphrase")

        self.log.info("A file that is not an archive is refused as such")
        not_an_archive = os.path.join(self.options.tmpdir, "not-an-archive.qsva")
        with open(not_an_archive, "wb") as handle:
            handle.write(b"this is not a vault archive")
        assert_raises_rpc_error(-4, "is not a vault archive",
                                source.importvaultarchive, not_an_archive, passphrase)

        self.log.info("A missing archive is reported rather than silently ignored")
        assert_raises_rpc_error(-4, "does not exist",
                                source.importvaultarchive,
                                os.path.join(self.options.tmpdir, "absent.qsva"), passphrase)

        self.log.info("Restore the history into a vault that has never seen it")
        node.createvault(vault_name="restored", blank=True)
        restored = node.get_vault_rpc("restored")
        assert_equal(restored.listtransactions("*", 100000), [])

        imported = restored.importvaultarchive(archive_path, passphrase)
        assert_equal(imported["transactions_imported"], result["transactions"])
        assert_equal(imported["transactions_skipped"], 0)
        assert_greater_than(imported["descriptors_imported"], 0)
        assert_equal(imported["addresses_imported"], result["addresses"])

        # The address book is what distinguishes a payment from change. Without it every
        # restored address reports ischange, and a self-payment produces NO history rows
        # at all -- which is how this was found. Assert the distinction survives, not just
        # the row count.
        assert_equal(restored.getaddressinfo(labelled)["ischange"], False)
        assert_equal(restored.getaddressinfo(labelled)["labels"], ["a paid counterparty"])
        assert_equal(restored.getaddressinfo(address)["ischange"], False)

        assert_equal(history_txids(restored), expected_txids)
        assert_equal(len(expected_txids), result["transactions"])

        self.log.info("Importing the same archive again changes nothing")
        again = restored.importvaultarchive(archive_path, passphrase)
        assert_equal(again["transactions_imported"], 0)
        assert_equal(again["transactions_skipped"], result["transactions"])
        assert_equal(again["descriptors_imported"], 0)
        assert_equal(history_txids(restored), expected_txids)

        self.log.info("The archive carries no spending material")
        with open(archive_path, "rb") as handle:
            raw = handle.read()
        for marker in (b"xprv", b"qprv", b"tqrv"):
            assert marker not in raw, f"{marker!r} found in archive"


if __name__ == '__main__':
    VaultArchiveTest(__file__).main()
