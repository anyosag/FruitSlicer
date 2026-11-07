# glad: OpenGL 3.3 core loader

Generated with glad 2.0.8 (https://github.com/Dav1dde/glad):

```sh
pip install glad2==2.0.8
glad --api gl:core=3.3 --extensions "" --out-path third_party/glad c
```

Licence of the generated files: `(WTFPL OR CC0-1.0) AND Apache-2.0` (see the
SPDX line in `include/glad/gl.h`; the Apache part covers the Khronos API
registry they are generated from). `include/KHR/khrplatform.h` is Khronos'
header under its own MIT-style licence, printed at the top of the file. Both
are compatible with FruitSlicer's AGPL-3.0. [`LICENSES.txt`](LICENSES.txt)
collects the texts; the macOS app bundle ships a copy.
