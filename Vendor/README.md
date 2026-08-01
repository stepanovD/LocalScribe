# Vendored binary dependencies

## whisper.cpp

`whisper.xcframework` contains only the universal macOS slice from the official
whisper.cpp **v1.9.1** release asset:

- upstream: <https://github.com/ggml-org/whisper.cpp>
- release commit: `f049fff`
- original asset: `whisper-v1.9.1-xcframework.zip`
- original asset SHA-256:
  `8c3ecbe73f48b0cb9318fc3058264f951ab336fd530e82c4ccdd2298d1311a4c`
- extracted architectures: `arm64`, `x86_64`

The upstream XCFramework's mobile, television, and spatial-computing slices
were deliberately not copied. LocalScribe declares and builds only a macOS
application target.

whisper.cpp remains licensed under the MIT License. Its complete notice is in
`whisper-LICENSE` and is copied into every application bundle produced by the
repository's bundle script. LocalScribe's project-level noncommercial license
does not replace the MIT terms for this dependency.

Model weights are not vendored. The app requires a user-selected local
multilingual ggml model and fails preflight if it is missing. A model is never
downloaded while recording.
