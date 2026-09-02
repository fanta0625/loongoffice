# Vendored build inputs

The ignored batch-print source directory is pinned by
`debian/vendor-sources.lock`:

- `libreoffice-batch-print`

Run `debian/scripts/fetch-vendor-sources` on a connected preparation host.
The proprietary OFD and OFDRW source directories are also ignored, but are
used only on trusted hosts to produce the prebuilt OXT described in
`debian/README.source`. `debian/rules` does not download any input.

LibreOffice's configuration-selected third-party archives are stored in the
ignored `libreoffice-tarballs` directory. Prepare them on a connected host with
`debian/scripts/prepare-libreoffice-tarballs`; every build validates its
configuration manifest and SHA256 checksums before compiling.
