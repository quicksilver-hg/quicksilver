This directory contains the source code for the Quicksilver graphical user interface (GUI). It uses the [Qt](https://www1.qt.io/developers/) cross-platform framework.

The current precise version for Qt 5 is specified in [qt.mk](../../depends/packages/qt.mk).

## Compile and run

See build instructions: [Unix](../../doc/build-unix.md), [macOS support status (unsupported)](../../doc/build-osx.md), [Windows](../../doc/build-windows-msvc.md), [FreeBSD](../../doc/build-freebsd.md), [NetBSD](../../doc/build-netbsd.md), [OpenBSD](../../doc/build-openbsd.md)

When following your systems build instructions, make sure to install the `Qt` dependencies.

To run:

```sh
./build/bin/quicksilver
```

## Files and Directories

#### forms/

- A directory that contains [Designer UI](https://doc.qt.io/qt-5.9/designer-using-a-ui-file.html) files. These files specify the characteristics of form elements in XML. Qt UI files can be edited with [Qt Creator](#using-qt-creator-as-an-ide) or using any text editor.

#### locale/

#### res/

 - Contains graphical resources used to enhance the UI experience.

#### test/

- Functional tests used to ensure proper functionality of the GUI. Significant changes to the GUI code normally require new or updated tests.

#### quicksilvergui.(h/cpp)

- Represents the main window of the Quicksilver UI.

#### \*model.(h/cpp)

- The model. When it has a corresponding controller, it generally inherits from  [QAbstractTableModel](https://doc.qt.io/qt-5/qabstracttablemodel.html). Models that are used by controllers as helpers inherit from other Qt classes like [QValidator](https://doc.qt.io/qt-5/qvalidator.html).
- ClientModel is used by the main application `quicksilvergui` and several models like `peertablemodel`.

#### \*page.(h/cpp)

- A controller. `:NAMEpage.cpp` generally includes `:NAMEmodel.h` and `forms/:NAME.page.ui` with a similar `:NAME`.

#### \*dialog.(h/cpp)

- Various dialogs, e.g. to open a URL. Inherit from [QDialog](https://doc.qt.io/qt-5/qdialog.html).

#### paymentserver.(h/cpp)

- Used to process BIP21 payment URI requests. Also handles URI-based application switching (e.g. when following a quicksilver:... link from a browser).

#### vaultview.(h/cpp)

- Represents the view to a single vault.

#### Other .h/cpp files

* UI elements like QuicksilverAmountField, which inherit from QWidget.
* `quicksilverunits.(h/cpp)`: Quicksilver / cinnabar display and amount handling
* `callback.h`
* `guiconstants.h`: UI colors, app name, etc.
* `guiutil.h`: several helper functions
* `macdockiconhandler.(h/mm)`: macOS dock icon handler
* `macnotificationhandler.(h/mm)`: display notifications in macOS

## Contribute

See [CONTRIBUTING.md](../../CONTRIBUTING.md) for general guidelines.

## Using Qt Creator as an IDE

[Qt Creator](https://www.qt.io/product/development-tools) is a powerful tool which packages a UI designer tool (Qt Designer) and a C++ IDE into one application. This is especially useful if you want to change the UI layout.

The macOS notes in this section describe Qt Creator itself, not a supported
Quicksilver build. Quicksilver 0.1.x has never been built, run, or gated on
macOS, and its GUI has never been launched there; see the
[macOS support status](../../doc/build-osx.md).

#### Download Qt Creator

On Unix and macOS, Qt Creator can be installed through your package manager. Alternatively, you can download a binary from the [Qt Website](https://www.qt.io/download/).

**Note:** If installing from a binary grabbed from the Qt Website: During the installation process, uncheck everything except for `Qt Creator`.

##### macOS

```sh
brew install qt-creator
```

##### Ubuntu & Debian

```sh
sudo apt-get install qtcreator
```

#### Setup Qt Creator

1. Make sure you've installed all dependencies specified in your systems build instructions
2. Follow the compile instructions for your system, adding the `-DCMAKE_BUILD_TYPE=Debug` build flag
3. Start Qt Creator. At the start page, do: `New` -> `Import Project` -> `Import Existing Project`
4. Enter `quicksilver` as the Project Name and enter the absolute path to `src/qt` as Location
5. Check over the file selection, you may need to select the `forms` directory (necessary if you intend to edit *.ui files)
6. Confirm the `Summary` page
7. In the `Projects` tab, select `Manage Kits...`

 **macOS**
 - Under `Kits`: select the default "Desktop" kit
 - Under `Compilers`: select `"Clang (x86 64bit in /usr/bin)"`
 - Under `Debuggers`: select `"LLDB"` as debugger (you might need to set the path to your LLDB installation)

 **Ubuntu & Debian**

 Note: Some of these options may already be set

 - Under `Kits`: select the default "Desktop" kit
 - Under `Compilers`: select `"GCC (x86 64bit in /usr/bin)"`
 - Under `Debuggers`: select `"GDB"` as debugger

8. While in the `Projects` tab, ensure that you have the `quicksilver` executable specified under `Run`
 - If the executable is not specified: click `"Choose..."`, navigate to `build/bin`, and select `quicksilver`
9. You're all set! Start developing, building, and debugging the Quicksilver GUI
