# Vendored build inputs

The ignored batch-print source directory and required LibreOffice submodules
are pinned by `debian/vendor-sources.lock`:

- `libreoffice-batch-print`
- `dictionaries`
- `helpcontent2`
- `translations`

Run `debian/scripts/fetch-vendor-sources` on a connected preparation host.
The proprietary OFD and OFDRW source directories are also ignored. They are
not needed by `debian/rules`: a connected preparation run downloads the
compiled OFD OXT from the URL in `debian/ofd-oxt.url` and places it in the
offline tarball input directory.

LibreOffice's configuration-selected third-party archives are stored in the
ignored `libreoffice-tarballs` directory. Prepare them on a connected host with
`debian/scripts/prepare-libreoffice-tarballs`; every build validates its
configuration manifest and SHA256 checksums before compiling.
