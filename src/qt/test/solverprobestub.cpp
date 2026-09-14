// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

// A do-nothing stand-in for the GPU solver, for the options-dialog startup
// probe test. OptionsDialog::validateGpuSolverPath() accepts a solver when it
// exits 0, so this ignores its arguments and exits 0.
//
// It is a real compiled executable rather than a script fixture because
// "executable" is spelled differently per platform: POSIX reads the execute
// bit, Windows reads the file extension. QProcess reaches CreateProcess on
// Windows, which cannot launch a .bat or .cmd, so only a genuine .exe will
// start there.
int main(int, char**)
{
    return 0;
}
