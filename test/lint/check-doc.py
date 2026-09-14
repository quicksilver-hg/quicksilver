#!/usr/bin/env python3
# Copyright (c) 2015-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

'''
This checks if all command line args are documented.
Return value is 0 to indicate no error.

Author: @MarcoFalke
'''

from subprocess import check_output
import re

FOLDER_GREP = 'src'
FOLDER_TEST = 'src/test/'
REGEX_ARG = r'\b(?:GetArg|GetArgs|GetBoolArg|GetIntArg|GetPathArg|IsArgSet|get_net)\("(-[^"]+)"'
# quicksilver-agent.cpp reads its args through typed helpers rather than calling
# ArgsManager directly, e.g. ParseCinnabarArg(args, "-spendamount", ...). Without
# this the helper-read args look undocumented in one direction and unknown in the
# other, and neither direction is reported honestly.
REGEX_ARG_HELPER = r'\bParse[A-Za-z]*Arg\(\w+, "(-[^"]+)"'
REGEX_DOC = r'AddArg\("(-[^"=]+?)(?:=|")'
CMD_ROOT_DIR = '$(git rev-parse --show-toplevel)/{}'.format(FOLDER_GREP)
CMD_GREP_ARGS = r"git grep --perl-regexp '{}' -- {} ':(exclude){}'".format(REGEX_ARG, CMD_ROOT_DIR, FOLDER_TEST)
CMD_GREP_ARGS_HELPER = r"git grep --perl-regexp '{}' -- {} ':(exclude){}'".format(REGEX_ARG_HELPER, CMD_ROOT_DIR, FOLDER_TEST)
CMD_GREP_VAULT_ARGS = r"git grep --function-context 'void VaultInit::AddVaultOptions' -- {} | grep AddArg".format(CMD_ROOT_DIR)
CMD_GREP_VAULT_HIDDEN_ARGS = r"git grep --function-context 'void DummyVaultInit::AddVaultOptions' -- {}".format(CMD_ROOT_DIR)
CMD_GREP_DOCS = r"git grep --perl-regexp '{}' -- {} ':(exclude){}'".format(REGEX_DOC, CMD_ROOT_DIR, FOLDER_TEST)
# list unsupported, deprecated and duplicate args as they need no documentation
SET_DOC_OPTIONAL = set(['-h', '-?', '-dbcrashratio', '-forcecompactdb'])

# Args that are read through a loop variable instead of a string literal, so no
# call-site regex can see them. Each entry names where the read actually happens;
# they are genuinely live, not residue. Verified 2026-08-25.
SET_ARG_READ_INDIRECTLY = set([
    # src/zmq/zmqnotificationinterface.cpp: `gArgs.GetArgs(arg)` over the
    # notifier table, and src/init.cpp's address/port validation loop.
    '-zmqpubhashblock', '-zmqpubhashtx', '-zmqpubrawblock', '-zmqpubrawtx',
    '-zmqpubsequence', '-zmqpubhashblockhwm', '-zmqpubhashtxhwm',
    '-zmqpubrawblockhwm', '-zmqpubrawtxhwm', '-zmqpubsequencehwm',
    # src/common/args.cpp and src/common/config.cpp read this straight out of
    # m_settings via FindKey(), because it may not be passed on the command line.
    '-includeconf',
])


def get_hidden_args():
    """Return args registered through AddHiddenArgs({...}) literal lists.

    These are deliberately undocumented, so they must not be reported as missing
    documentation. src/dummyvault.cpp is excluded: it hides *every* vault arg for
    the no-vault build, so folding it in here would suppress genuinely missing
    vault documentation. It is checked separately by lint_missing_hidden_vault_args.
    """
    files = check_output(
        "git grep -l 'AddHiddenArgs' -- {} ':(exclude){}/dummyvault.cpp'".format(
            CMD_ROOT_DIR, CMD_ROOT_DIR),
        shell=True).decode('utf8').split()
    hidden = set()
    for path in files:
        with open(path, 'r', encoding='utf8') as f:
            text = f.read()
        for block in re.findall(r'AddHiddenArgs\(\{(.*?)\}\)', text, re.DOTALL):
            hidden.update(re.findall(r'"(-[^"=]+)"', block))
    return hidden


def lint_missing_argument_documentation():
    used = check_output(CMD_GREP_ARGS, shell=True).decode('utf8').strip()
    used_helper = check_output(CMD_GREP_ARGS_HELPER, shell=True).decode('utf8').strip()
    docd = check_output(CMD_GREP_DOCS, shell=True).decode('utf8').strip()

    args_used = set(re.findall(re.compile(REGEX_ARG), used))
    args_used |= set(re.findall(re.compile(REGEX_ARG_HELPER), used_helper))
    args_used |= SET_ARG_READ_INDIRECTLY
    args_docd = set(re.findall(re.compile(REGEX_DOC), docd)).union(SET_DOC_OPTIONAL).union(get_hidden_args())
    args_need_doc = args_used.difference(args_docd)
    args_unknown = args_docd.difference(args_used)

    print("Args used        : {}".format(len(args_used)))
    print("Args documented  : {}".format(len(args_docd)))
    print("Args undocumented: {}".format(len(args_need_doc)))
    print(args_need_doc)
    print("Args unknown     : {}".format(len(args_unknown)))
    print(args_unknown)

    assert 0 == len(args_need_doc), "Please document the following arguments: {}".format(args_need_doc)
    assert 0 == len(args_unknown), "Please remove the following unread arguments, or record where they are read: {}".format(args_unknown)


def lint_missing_hidden_vault_args():
    vault_args = check_output(CMD_GREP_VAULT_ARGS, shell=True).decode('utf8').strip()
    vault_hidden_args = check_output(CMD_GREP_VAULT_HIDDEN_ARGS, shell=True).decode('utf8').strip()

    vault_args = set(re.findall(re.compile(REGEX_DOC), vault_args))
    vault_hidden_args = set(re.findall(re.compile(r'    "([^"=]+)'), vault_hidden_args))

    hidden_missing = vault_args.difference(vault_hidden_args)
    if hidden_missing:
        assert 0, "Please add {} to the hidden args in DummyVaultInit::AddVaultOptions".format(hidden_missing)


def main():
    lint_missing_argument_documentation()
    lint_missing_hidden_vault_args()


if __name__ == "__main__":
    main()
