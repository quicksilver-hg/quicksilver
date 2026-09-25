# Copyright (c) The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# Disable files from being included in completions by default
complete --command quicksilver --no-files

# Extract options
function __fish_quicksilver_get_options
    argparse 'nofiles' -- $argv
    set --local cmd (commandline -opc)[1]
    set --local options

    if set -q _flag_nofiles
        set --append options ($cmd -help-debug | string match -r '^  -.*' | string replace -r '  -' '-' | string replace -r '=.*' '=' | string match --invert -r '^.*=$')
    else
        set --append options ($cmd -help-debug | string match -r '^  -.*' | string replace -r '  -' '-' | string replace -r '=.*' '=' | string match -r '^.*=$')
    end

    for option in $options
        echo $option
    end
end


# Add options with file completion
complete \
    --command quicksilver \
    --arguments "(__fish_quicksilver_get_options)"
# Enable file completions only if the commandline now contains a `*.=` style option
complete -c quicksilver \
    --condition 'string match --regex -- ".*=" (commandline -pt)' \
    --force-files

# Add options without file completion
complete \
    --command quicksilver \
    --arguments "(__fish_quicksilver_get_options --nofiles)"
