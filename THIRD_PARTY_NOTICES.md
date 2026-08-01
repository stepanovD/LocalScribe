# Third-party notices

LocalScribe's PolyForm Noncommercial License applies only to the original
LocalScribe code and documentation. It does not replace or restrict the licenses
of the components listed below.

## whisper.cpp

LocalScribe vendors the universal macOS slice of whisper.cpp v1.9.1. whisper.cpp
is licensed under the MIT License. The complete copyright and license notice is
included in [`Vendor/whisper-LICENSE`](Vendor/whisper-LICENSE).

The MIT License permits use, modification, distribution, sublicensing, and sale,
provided its copyright and permission notice is retained. Those terms continue
to apply to whisper.cpp itself. They do not grant a commercial license to the
original LocalScribe code.

## SQLite

LocalScribe links to the SQLite library supplied by the operating system. SQLite
is not vendored in this repository. The SQLite authors have dedicated the
deliverable SQLite code and documentation to the public domain. See the
[official SQLite copyright statement](https://sqlite.org/copyright.html).

## Apple system frameworks and toolchain libraries

LocalScribe links to AppKit, AVFoundation, CoreAudio, CoreMedia,
ScreenCaptureKit, SwiftUI, the C++ standard library, and other libraries supplied
with macOS or Apple's developer tools. These libraries are not copied into this
repository and remain governed by Apple's applicable software agreements.

## Whisper model files

Model weights are not included in LocalScribe. Users select a local model file
at runtime and are responsible for the terms that apply to that model.
