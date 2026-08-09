# Plugin signatures

HDF5 can verify an RSA signature on a plugin before `dlopen`-ing it. When the
HDF5 in use was built with `-DHDF5_REQUIRE_SIGNED_PLUGINS=ON`, an unsigned
`vol-stream` is rejected, and the error does not obviously say "unsigned":

```
H5PL__find_plugin_in_path(): search in directory failed
H5PL__open(): plugin signature verification failed for: .../libvol_stream.so
H5PL__verify_signature_appended(): cannot read or validate signature footer
H5PL__read_and_validate_footer(): not a signed HDF5 plugin (bad magic or
                                  unsupported format version)
```

It reads like a corrupt or incompatible plugin, which is why it is written down
here. The connector itself is fine; the loader simply will not accept it.

Note this affects the **plugin path only**. An application that links the
connector and calls `H5VL_stream_register()` never goes through the plugin
loader, so it is unaffected. That asymmetry matters because the M0 exit gate
drives HDF5's `test/API` suite through `HDF5_VOL_CONNECTOR`, which *is* the
plugin path.

## Checking whether your HDF5 enforces it

```bash
grep HDF5_REQUIRE_SIGNED_PLUGINS /path/to/hdf5-build/CMakeCache.txt
```

The HDF5 default is `OFF`. If a build has it `ON` and you control that build,
turning it off is the simplest fix for a development setup:

```bash
cmake -S /path/to/hdf5 -B /path/to/hdf5-build -DHDF5_REQUIRE_SIGNED_PLUGINS=OFF
cmake --build /path/to/hdf5-build
```

## Signing a build instead

When enforcement has to stay on — a shared or production install — generate a
key pair, sign the plugin, and give readers a keystore holding the public key.

Generate a development key pair. **Do not commit either file**; `.gitignore`
excludes `*.pem` and `keystore/` for this reason:

```bash
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:4096 -out dev_private.pem
chmod 600 dev_private.pem
mkdir -p keystore
openssl rsa -in dev_private.pem -pubout -out keystore/dev_public.pem
```

Have the build sign the plugin as a post-build step:

```bash
cmake -S . -B build \
  -DCMAKE_PREFIX_PATH=/path/to/hdf5-install \
  -DVOL_STREAM_SIGN_KEY=$PWD/dev_private.pem
cmake --build build
```

The build invokes `h5sign -f`, so an incremental rebuild re-signs rather than
failing on the previous signature. If `h5sign` is not on `PATH` or under
`${HDF5_ROOT}/bin`, point at it with `-DH5SIGN_EXECUTABLE=/path/to/h5sign`.

Then run with the keystore visible:

```bash
export HDF5_PLUGIN_PATH=$PWD/build
export HDF5_PLUGIN_KEYSTORE=$PWD/keystore
export HDF5_VOL_CONNECTOR="vol-stream"
```

A keystore may hold public keys from several developers; HDF5 accepts the plugin
if any key verifies it.

## Two things to watch

**A stale unsigned copy shadowing the signed one.** `HDF5_PLUGIN_PATH` is
searched as a directory, so a leftover unsigned `libvol_stream.so` beside the
signed one can be the copy that gets tried and rejected. Keep one plugin per
directory when debugging this.

**A locked keystore.** If HDF5 was built with `-DHDF5_LOCK_PLUGIN_KEYSTORE=ON`,
the `HDF5_PLUGIN_KEYSTORE` environment variable is ignored and only the
compile-time `HDF5_PLUGIN_KEYSTORE_DIR` applies. In that case the public key has
to go in the directory chosen when HDF5 was built.

## Reference

`docs/PLUGIN_SIGNATURE_README.md` in the HDF5 source tree, and `h5sign --help`.
