# Copyright (c) The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# Disable files from being included in completions by default
complete --command quicksilver-vault --no-files

# Extract options
function __fish_quicksilver_vault_get_options
    set --local cmd (commandline -opc)[1]
    for option in ($cmd -help 2>&1 | string match -r '^  -.*' | string replace -r '  -' '-' | string replace -r '=.*' '=')
        echo $option
    end
end

# Extract commands
function __fish_quicksilver_vault_get_commands
    set --local cmd (commandline -opc)[1]
    for command in ($cmd -help | sed -e '1,/Commands:/d' -e 's/=/=\t/' -e 's/(=/=/' -e '/^  [a-z]/ p' -e d | string replace -r '\ \ ' '')
        echo $command
    end
end

# Add options
complete \
    --command quicksilver-vault \
    --condition "not __fish_seen_subcommand_from (__fish_quicksilver_vault_get_commands)" \
    --arguments "(__fish_quicksilver_vault_get_options)"

# Add commands
complete \
    --command quicksilver-vault \
    --condition "not __fish_seen_subcommand_from (__fish_quicksilver_vault_get_commands)" \
    --arguments "(__fish_quicksilver_vault_get_commands)"

# Add file completions for load and set commands
complete --command quicksilver-vault \
    --condition "string match -r -- '(dumpfile|datadir)*=' (commandline -pt)" \
    --force-files
