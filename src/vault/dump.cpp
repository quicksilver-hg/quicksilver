// Copyright (c) 2020-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <vault/dump.h>

#include <common/args.h>
#include <util/fs.h>
#include <util/translation.h>
#include <vault/vault.h>
#include <vault/vaultdb.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace vault {
static const std::string DUMP_MAGIC = "QUICKSILVER_VAULT_DUMP";
uint32_t DUMP_VERSION = 1;

bool DumpVault(const ArgsManager& args, VaultDatabase& db, bilingual_str& error)
{
    // Get the dumpfile
    std::string dump_filename = args.GetArg("-dumpfile", "");
    if (dump_filename.empty()) {
        error = _("No dump file provided. To use dump, -dumpfile=<filename> must be provided.");
        return false;
    }

    fs::path path = fs::PathFromString(dump_filename);
    path = fs::absolute(path);
    if (fs::exists(path)) {
        error = strprintf(_("File %s already exists. If you are sure this is what you want, move it out of the way first."), fs::PathToString(path));
        return false;
    }
    std::ofstream dump_file;
    dump_file.open(path);
    if (dump_file.fail()) {
        error = strprintf(_("Unable to open %s for writing"), fs::PathToString(path));
        return false;
    }

    HashWriter hasher{};

    std::unique_ptr<DatabaseBatch> batch = db.MakeBatch();

    bool ret = true;
    std::unique_ptr<DatabaseCursor> cursor = batch->GetNewCursor();
    if (!cursor) {
        error = _("Error: Couldn't create cursor into database");
        ret = false;
    }

    // Write out a magic string with version
    std::string line = strprintf("%s,%u\n", DUMP_MAGIC, DUMP_VERSION);
    dump_file.write(line.data(), line.size());
    hasher << Span{line};

    // Write out the file format.
    line = strprintf("%s,%s\n", "format", db.Format());
    dump_file.write(line.data(), line.size());
    hasher << Span{line};

    if (ret) {

        // Read the records
        while (true) {
            DataStream ss_key{};
            DataStream ss_value{};
            DatabaseCursor::Status status = cursor->Next(ss_key, ss_value);
            if (status == DatabaseCursor::Status::DONE) {
                ret = true;
                break;
            } else if (status == DatabaseCursor::Status::FAIL) {
                error = _("Error reading next record from vault database");
                ret = false;
                break;
            }
            std::string key_str = HexStr(ss_key);
            std::string value_str = HexStr(ss_value);
            line = strprintf("%s,%s\n", key_str, value_str);
            dump_file.write(line.data(), line.size());
            hasher << Span{line};
        }
    }

    cursor.reset();
    batch.reset();

    if (ret) {
        // Write the hash
        tfm::format(dump_file, "checksum,%s\n", HexStr(hasher.GetHash()));
        dump_file.close();
    } else {
        // Remove the dumpfile on failure
        dump_file.close();
        fs::remove_quiet(path);
    }

    return ret;
}

// The standard vault deleter function blocks on the validation interface
// queue, which doesn't exist for the quicksilver-vault. Define our own
// deleter here.
static void VaultToolReleaseVault(CVault* vault)
{
    vault->VaultLogPrintf("down reason=released");
    vault->Close();
    delete vault;
}

bool CreateFromDump(const ArgsManager& args, const std::string& name, const fs::path& vault_path, bilingual_str& error, std::vector<bilingual_str>& warnings)
{
    // Get the dumpfile
    std::string dump_filename = args.GetArg("-dumpfile", "");
    if (dump_filename.empty()) {
        error = _("No dump file provided. To use createfromdump, -dumpfile=<filename> must be provided.");
        return false;
    }

    fs::path dump_path = fs::PathFromString(dump_filename);
    dump_path = fs::absolute(dump_path);
    if (!fs::exists(dump_path)) {
        error = strprintf(_("Dump file %s does not exist."), fs::PathToString(dump_path));
        return false;
    }
    std::ifstream dump_file{dump_path};

    // Compute the checksum
    HashWriter hasher{};
    uint256 checksum;

    // Check the magic and version
    std::string magic_key;
    std::getline(dump_file, magic_key, ',');
    std::string version_value;
    std::getline(dump_file, version_value, '\n');
    if (magic_key != DUMP_MAGIC) {
        error = strprintf(_("Error: Dumpfile identifier record is incorrect. Got \"%s\", expected \"%s\"."), magic_key, DUMP_MAGIC);
        dump_file.close();
        return false;
    }
    // Check the version number (value of first record)
    uint32_t ver;
    if (!ParseUInt32(version_value, &ver)) {
        error =strprintf(_("Error: Unable to parse version %u as a uint32_t"), version_value);
        dump_file.close();
        return false;
    }
    if (ver != DUMP_VERSION) {
        error = strprintf(_("Error: Dumpfile version is not supported. This version of quicksilver-vault only supports version 1 dumpfiles. Got dumpfile with version %s"), version_value);
        dump_file.close();
        return false;
    }
    std::string magic_hasher_line = strprintf("%s,%s\n", magic_key, version_value);
    hasher << Span{magic_hasher_line};

    // Get the stored file format
    std::string format_key;
    std::getline(dump_file, format_key, ',');
    std::string format_value;
    std::getline(dump_file, format_value, '\n');
    if (format_key != "format") {
        error = strprintf(_("Error: Dumpfile format record is incorrect. Got \"%s\", expected \"format\"."), format_key);
        dump_file.close();
        return false;
    }
    // Get the data file format with format_value as the default
    std::string file_format = args.GetArg("-format", format_value);
    if (file_format.empty()) {
        error = _("No vault file format provided. To use createfromdump, -format=<format> must be provided.");
        return false;
    }
    if (file_format != "sqlite") {
        error = strprintf(_("Unknown vault file format \"%s\" provided. Please provide \"sqlite\"."), file_format);
        return false;
    }
    if (file_format != format_value) {
        warnings.push_back(strprintf(_("Warning: Dumpfile vault format \"%s\" does not match command line specified format \"%s\"."), format_value, file_format));
    }
    std::string format_hasher_line = strprintf("%s,%s\n", format_key, format_value);
    hasher << Span{format_hasher_line};

    DatabaseOptions options;
    DatabaseStatus status;
    ReadDatabaseArgs(args, options);
    options.require_create = true;
    options.require_format = DatabaseFormat::SQLITE;
    std::unique_ptr<VaultDatabase> database = MakeDatabase(vault_path, options, status, error);
    if (!database) return false;

    // dummy chain interface
    bool ret = true;
    std::shared_ptr<CVault> vault(new CVault(/*chain=*/nullptr, name, std::move(database)), VaultToolReleaseVault);
    {
        LOCK(vault->cs_vault);
        DBErrors load_vault_ret = vault->LoadVault();
        if (load_vault_ret != DBErrors::LOAD_OK) {
            error = strprintf(_("Error creating %s"), name);
            return false;
        }

        // Get the database handle
        VaultDatabase& db = vault->GetDatabase();
        std::unique_ptr<DatabaseBatch> batch = db.MakeBatch();
        batch->TxnBegin();

        // Read the records from the dump file and write them to the database
        while (dump_file.good()) {
            std::string key;
            std::getline(dump_file, key, ',');
            std::string value;
            std::getline(dump_file, value, '\n');

            if (key == "checksum") {
                std::vector<unsigned char> parsed_checksum = ParseHex(value);
                if (parsed_checksum.size() != checksum.size()) {
                    error = Untranslated("Error: Checksum is not the correct size");
                    ret = false;
                    break;
                }
                std::copy(parsed_checksum.begin(), parsed_checksum.end(), checksum.begin());
                break;
            }

            std::string line = strprintf("%s,%s\n", key, value);
            hasher << Span{line};

            if (key.empty() || value.empty()) {
                continue;
            }

            if (!IsHex(key)) {
                error = strprintf(_("Error: Got key that was not hex: %s"), key);
                ret = false;
                break;
            }
            if (!IsHex(value)) {
                error = strprintf(_("Error: Got value that was not hex: %s"), value);
                ret = false;
                break;
            }

            std::vector<unsigned char> k = ParseHex(key);
            std::vector<unsigned char> v = ParseHex(value);
            if (!batch->Write(Span{k}, Span{v})) {
                error = strprintf(_("Error: Unable to write record to new vault"));
                ret = false;
                break;
            }
        }

        if (ret) {
            uint256 comp_checksum = hasher.GetHash();
            if (checksum.IsNull()) {
                error = _("Error: Missing checksum");
                ret = false;
            } else if (checksum != comp_checksum) {
                error = strprintf(_("Error: Dumpfile checksum does not match. Computed %s, expected %s"), HexStr(comp_checksum), HexStr(checksum));
                ret = false;
            }
        }

        if (ret) {
            batch->TxnCommit();
        } else {
            batch->TxnAbort();
        }

        batch.reset();

        dump_file.close();
    }
    vault.reset(); // The pointer deleter will close the vault for us.

    // Remove the vault dir if we have a failure
    if (!ret) {
        fs::remove_all(vault_path);
    }

    return ret;
}
} // namespace vault
